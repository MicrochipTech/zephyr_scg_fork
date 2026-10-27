/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Kernel system timer for the Microchip XEC Input Capture and Compare Timer
 * (CCT), datasheet section 24.0.
 *
 * The block gives a free-running 32-bit up-counter plus two 32-bit compare
 * registers that interrupt on match, so one block supplies both the cycle
 * source and the tick deadline: the counter read is a single register access,
 * which is what lets the generic timer core read it without the clock lock.
 * The Basic Timer blocks have no compare register and cannot do this.
 *
 * The compare match is on equality only (section 24.9.2), so this is a
 * COMPARE_EXACT backend: the core writes the comparator through a verify loop
 * that handles a deadline already in the past.
 *
 * Low-power behaviour, measured on hardware: with ACTIVATE, FREE_ENABLE and
 * COMPARE_ENABLE0 all set, writing the PCR Sleep Control register with
 * SLEEP_ALL = 1 pauses the block and drops its clock request, so it does not
 * hold the PLL up. Clearing SLEEP_ALL resumes counting from the free-running
 * value the block held when SLEEP_ALL was set.
 *
 * So the block needs no software disable around sleep, and it is paused rather
 * than reset: the count survives, but the sleep interval is not counted. That
 * interval is recovered from the always-on 32 KHz RTOS timer through the system
 * timer low-power companion interface and added back to the counter on wake.
 * The SoC asserts SLEEP_ALL for light sleep as well as deep sleep, so this
 * applies to every power state.
 */

#define DT_DRV_COMPAT microchip_xec_cct_timer

#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <cmsis_core.h>
#include <soc_ecia.h>
#if !defined(CONFIG_SYSTEM_TIMER_LPM_COMPANION_NONE)
#include <zephyr/drivers/timer/system_timer_lpm.h>
#endif

BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP), "XEC CCT timer does not support SMP");

#define CCT_NODE DT_DRV_INST(0)

BUILD_ASSERT(DT_NODE_HAS_STATUS(CCT_NODE, okay),
	     "microchip,xec-cct-timer devicetree node is not enabled");

#define CCT_BASE ((mm_reg_t)DT_INST_REG_ADDR(0))

#define CCT_COMPARE0_IRQ  DT_INST_IRQ_BY_NAME(0, comp0, irq)
#define CCT_COMPARE0_PRIO DT_INST_IRQ_BY_NAME(0, comp0, priority)

/* GIRQ (number, source-bit) of the Compare 0 event. The girqs pairs follow the
 * order of interrupts, and comp0 is the eighth of them, so its pair starts at
 * cell 14. Verified as {18, 27} in the generated devicetree.
 */
#define CCT_GIRQ     DT_INST_PROP_BY_IDX(0, girqs, 14)
#define CCT_GIRQ_POS DT_INST_PROP_BY_IDX(0, girqs, 15)

/* Register offsets, datasheet TABLE 24-3. All accesses must be DWORD. */
#define CCT_CTRL_OFS     0x00u
#define CCT_FREE_RUN_OFS 0x0cu
#define CCT_COMPARE0_OFS 0x28u

/* Capture and Compare Timer Control Register, datasheet section 24.10.1. */
#define CCT_CTRL_ACTIVATE        BIT(0)
#define CCT_CTRL_FREE_ENABLE     BIT(1)
#define CCT_CTRL_FREE_RESET      BIT(2)
#define CCT_CTRL_TCLK_POS        4
#define CCT_CTRL_COMPARE_ENABLE0 BIT(8)

/* Counter rate after the TCLK divider. TCLK selects a power-of-two divide of
 * 48 MHz (datasheet TABLE 24-2), so the field is the log2 of the divisor.
 */
#define CCT_INPUT_HZ 48000000u
#define CCT_RATE_HZ  DT_INST_PROP(0, clock_frequency)
#define CCT_TCLK_DIV (CCT_INPUT_HZ / CCT_RATE_HZ)

BUILD_ASSERT(CCT_RATE_HZ != 0 && (CCT_INPUT_HZ % CCT_RATE_HZ) == 0 &&
	     IS_POWER_OF_TWO(CCT_TCLK_DIV) && CCT_TCLK_DIV <= 128,
	     "clock-frequency must divide 48 MHz by a power of two, at most 128");

#define CCT_TCLK_SEL (31 - __builtin_clz(CCT_TCLK_DIV))

static ALWAYS_INLINE uint32_t cct_ctrl_read(void)
{
	return sys_read32(CCT_BASE + CCT_CTRL_OFS);
}

static ALWAYS_INLINE void cct_ctrl_write(uint32_t val)
{
	sys_write32(val, CCT_BASE + CCT_CTRL_OFS);
}

/*
 * A free-running counter paired with an equality-match comparator: a
 * COMPARE_EXACT backend. The counter is a single 32-bit register read, so it
 * is neither synthesized nor shared with the ISR and needs neither
 * TIMER_CORE_COUNTER_NONATOMIC nor a software accumulator.
 */
#define TIMER_CORE_BACKEND_COMPARE_EXACT
#define TIMER_CORE_COUNTER_WIDTH 32
#define TIMER_CORE_CYCLES_PER_SEC CCT_RATE_HZ

/*
 * A compare written too close to the counter can be passed before the write
 * lands, and the equality match is then missed for a whole counter period.
 * Two cycles covers the write reaching the counter's clock domain; the core's
 * verify loop re-reads and retries from there.
 */
#define TIMER_CORE_ALARM_LEAD_CYCLES 2u

static inline uint32_t timer_driver_cycle_get(void)
{
	return sys_read32(CCT_BASE + CCT_FREE_RUN_OFS);
}

/*
 * Arm Compare 0 at an absolute counter value. Datasheet section 24.9.2.2
 * requires the compare to be disabled while its register is updated, or the
 * matcher can emit a spurious event.
 */
static void timer_driver_set_compare(uint32_t cycles)
{
	uint32_t ctrl = cct_ctrl_read();

	cct_ctrl_write(ctrl & ~CCT_CTRL_COMPARE_ENABLE0);
	sys_write32(cycles, CCT_BASE + CCT_COMPARE0_OFS);
	cct_ctrl_write(ctrl | CCT_CTRL_COMPARE_ENABLE0);
}

/* TIMER_CORE_CYCLES_PER_SEC above declares a counter rate of the driver's own,
 * so the core leaves the cycle getters to the driver. The rate is the kernel's
 * too (see the BUILD_ASSERT below), so the count needs no scaling.
 */
#define TIMER_CORE_HAVE_CYCLE_GET_32

#include "system_timer_generic.h"

BUILD_ASSERT(CCT_RATE_HZ == CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC,
	     "clock-frequency must equal CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC");

uint32_t sys_clock_cycle_get_32(void)
{
	return timer_driver_cycle_get();
}

static void cct_timer_isr(const void *arg)
{
	ARG_UNUSED(arg);

	k_spinlock_key_t key = sys_clock_lock();

	/* The compare event has no status bit inside the block; it is latched
	 * only in the GIRQ source register. Disabling the compare drops the
	 * match condition before the latch is cleared, so the latch cannot
	 * immediately re-assert from a still-matching comparator.
	 */
	cct_ctrl_write(cct_ctrl_read() & ~CCT_CTRL_COMPARE_ENABLE0);
	soc_ecia_girq_status_clear(CCT_GIRQ, CCT_GIRQ_POS);

	timer_core_announce_from(key);
}

#ifdef CONFIG_SYSTEM_TIMER_HAS_DISABLE_SUPPORT
void sys_clock_disable(void)
{
	cct_ctrl_write(cct_ctrl_read() &
		       ~(CCT_CTRL_COMPARE_ENABLE0 | CCT_CTRL_FREE_ENABLE | CCT_CTRL_ACTIVATE));
	soc_ecia_girq_ctrl(CCT_GIRQ, CCT_GIRQ_POS, MCHP_MEC_ECIA_GIRQ_DIS);
	irq_disable(CCT_COMPARE0_IRQ);
}
#endif

#if !defined(CONFIG_SYSTEM_TIMER_LPM_COMPANION_NONE)

void sys_clock_idle_enter(uint32_t ticks)
{
	__ASSERT(sys_clock_is_locked(), "system clock lock not held");

	/* Arm the always-on companion to wake by the deadline.
	 * SYS_CLOCK_IDLE_FOREVER converts to a wakeup tens of days out, which is
	 * the intent: the companion may wake earlier, never later.
	 *
	 * Nothing else to do. The block pauses itself and drops its clock
	 * request when the SoC asserts SLEEP_ALL, so it neither has to be
	 * disabled here nor re-enabled on the way out, and the armed compare
	 * survives untouched. A compare that is already pending is left pending
	 * on purpose: it marks a deadline that really expired, and letting it
	 * abort the sleep and be serviced is correct.
	 */
	z_sys_clock_lpm_enter(k_ticks_to_us_ceil64(ticks));
}

void sys_clock_idle_exit(void)
{
	k_spinlock_key_t key = sys_clock_lock();

	/*
	 * The block was paused across the low-power state, so it did not count
	 * the sleep: the whole interval comes from the companion. It is taken in
	 * cycles because a sleep can outrun a 32-bit cycle count (89 s at
	 * 48 MHz).
	 */
	uint64_t slept_us = z_sys_clock_lpm_exit();
	uint64_t slept_cycles = k_us_to_cyc_floor64(slept_us);

	/*
	 * Add the slept cycles to the counter. The core holds its announce
	 * baseline as a 64-bit extension whose low bits must keep tracking this
	 * counter, so the counter has to advance by the span the announce below
	 * credits. The addend comes from the counter as it reads now, not from a
	 * value sampled before the sleep: the block resumed from where it
	 * paused and has been counting again since, and reusing a stale sample
	 * would discard that and could even move the counter backwards, which
	 * the core's wrap-masked deltas would read as an enormous elapse.
	 *
	 * Section 24.9.2.2 requires the compare disabled while the free-running
	 * register is written, or the matcher can emit a spurious event. The
	 * kernel re-arms the deadline right after this returns, which enables
	 * the compare again.
	 */
	uint32_t ctrl = cct_ctrl_read();

	cct_ctrl_write(ctrl & ~CCT_CTRL_COMPARE_ENABLE0);
	sys_write32(timer_driver_cycle_get() + (uint32_t)slept_cycles,
		    CCT_BASE + CCT_FREE_RUN_OFS);

	/*
	 * Announce the whole ticks slept. The sub-tick remainder stays in the
	 * counter, which advanced by the full span, so the next announce picks
	 * it up along with anything left unannounced before the sleep.
	 */
	timer_core_announce_cycles64_from(key, slept_cycles);
}

#endif /* !CONFIG_SYSTEM_TIMER_LPM_COMPANION_NONE */

/*
 * Low-power companion: the always-on 32 KHz RTOS timer.
 *
 * HOOKS mode only. The RTOS timer has no Counter API driver, and the generic
 * counter hooks in sys_lpm_hooks_counter.c only handle up-counting timers,
 * while the RTOS timer counts down.
 */

#ifdef CONFIG_SYSTEM_TIMER_LPM_COMPANION_HOOKS

BUILD_ASSERT(DT_INST_NODE_HAS_PROP(0, companion_timer),
	     "companion-timer is required in SYSTEM_TIMER_LPM_COMPANION_HOOKS mode");

#define RTOS_NODE DT_INST_PHANDLE(0, companion_timer)

#define RTOS_BASE ((mm_reg_t)DT_REG_ADDR(RTOS_NODE))
#define RTOS_IRQ  DT_IRQN(RTOS_NODE)
#define RTOS_PRIO DT_IRQ(RTOS_NODE, priority)

#define RTOS_GIRQ     DT_PROP_BY_IDX(RTOS_NODE, girqs, 0)
#define RTOS_GIRQ_POS DT_PROP_BY_IDX(RTOS_NODE, girqs, 1)

BUILD_ASSERT(DT_PROP(RTOS_NODE, clock_frequency) == 32768,
	     "the companion RTOS timer runs at a fixed 32768 Hz");

/* RTOS timer, datasheet section 26.0: a 32-bit down-counter on the always-on
 * 32 KHz domain.
 */
#define RTOS_HZ 32768u

#define RTOS_CNT_OFS  0x00u
#define RTOS_PRLD_OFS 0x04u
#define RTOS_CTRL_OFS 0x08u

#define RTOS_CTRL_ACTIVATE BIT(0)
#define RTOS_CTRL_AUTO_RLD BIT(1)
#define RTOS_CTRL_START    BIT(2)

/* Counts most recently programmed, for the elapsed calculation on wake. */
static uint32_t rtos_armed_counts;

void z_sys_clock_lpm_enter(uint64_t max_lpm_time_us)
{
	/* Longest interval the 32-bit down-counter can express. Comparing in
	 * microseconds first keeps the conversion below from overflowing on the
	 * very large value a "forever" idle passes in.
	 */
	const uint64_t max_us = ((uint64_t)UINT32_MAX * USEC_PER_SEC) / RTOS_HZ;
	uint32_t counts;

	if (max_lpm_time_us >= max_us) {
		counts = UINT32_MAX;
	} else {
		counts = (uint32_t)((max_lpm_time_us * RTOS_HZ) / USEC_PER_SEC);
		counts = MAX(counts, 1u);
	}

	/* Section 26.10: stop, load the preload, then enable and start. The
	 * load takes up to one 32 KHz clock to reach the count register.
	 */
	sys_write32(0, RTOS_BASE + RTOS_CTRL_OFS);
	sys_write32(counts, RTOS_BASE + RTOS_PRLD_OFS);
	rtos_armed_counts = counts;
	sys_write32(RTOS_CTRL_ACTIVATE, RTOS_BASE + RTOS_CTRL_OFS);
	sys_write32(RTOS_CTRL_ACTIVATE | RTOS_CTRL_START, RTOS_BASE + RTOS_CTRL_OFS);
}

uint64_t z_sys_clock_lpm_exit(void)
{
	uint32_t cnt = sys_read32(RTOS_BASE + RTOS_CNT_OFS);
	uint32_t counts;

	/* Down-counter, halted at zero with AUTO_RELOAD clear. A count above
	 * what was armed means the preload had not yet reached the count
	 * register when the wake arrived, so nothing had elapsed.
	 */
	counts = (cnt <= rtos_armed_counts) ? (rtos_armed_counts - cnt) : 0u;

	sys_write32(0, RTOS_BASE + RTOS_CTRL_OFS);
	soc_ecia_girq_status_clear(RTOS_GIRQ, RTOS_GIRQ_POS);

	return ((uint64_t)counts * USEC_PER_SEC) / RTOS_HZ;
}

/* The RTOS timer event has no status bit in the block; it is latched only in
 * the GIRQ source register. The time accounting is done by
 * z_sys_clock_lpm_exit(), so the wake only has to be acknowledged.
 */
static void rtos_timer_isr(const void *arg)
{
	ARG_UNUSED(arg);

	soc_ecia_girq_status_clear(RTOS_GIRQ, RTOS_GIRQ_POS);
}

static void rtos_companion_init(void)
{
	sys_write32(0, RTOS_BASE + RTOS_CTRL_OFS);
	soc_ecia_girq_status_clear(RTOS_GIRQ, RTOS_GIRQ_POS);
	soc_ecia_girq_ctrl(RTOS_GIRQ, RTOS_GIRQ_POS, MCHP_MEC_ECIA_GIRQ_EN);
	IRQ_CONNECT(RTOS_IRQ, RTOS_PRIO, rtos_timer_isr, NULL, 0);
	irq_enable(RTOS_IRQ);
}
#else
static void rtos_companion_init(void)
{
}
#endif /* CONFIG_SYSTEM_TIMER_LPM_COMPANION_HOOKS */

static int cct_timer_init(void)
{
	/* Reset the free-running counter to zero, then start it. The compare is
	 * left disabled; the core arms it through timer_driver_set_compare().
	 * soc_ecia_init() has already put GIRQ18 in direct mode and cleared its
	 * latches, so only this source bit has to be enabled.
	 */
	cct_ctrl_write((CCT_TCLK_SEL << CCT_CTRL_TCLK_POS) | CCT_CTRL_FREE_RESET |
		       CCT_CTRL_ACTIVATE);
	cct_ctrl_write((CCT_TCLK_SEL << CCT_CTRL_TCLK_POS) | CCT_CTRL_FREE_ENABLE |
		       CCT_CTRL_ACTIVATE);

	soc_ecia_girq_status_clear(CCT_GIRQ, CCT_GIRQ_POS);
	soc_ecia_girq_ctrl(CCT_GIRQ, CCT_GIRQ_POS, MCHP_MEC_ECIA_GIRQ_EN);

	IRQ_CONNECT(CCT_COMPARE0_IRQ, CCT_COMPARE0_PRIO, cct_timer_isr, NULL, 0);
	irq_enable(CCT_COMPARE0_IRQ);

	rtos_companion_init();

	timer_core_init();

	return 0;
}

SYS_INIT(cct_timer_init, PRE_KERNEL_2, CONFIG_SYSTEM_CLOCK_INIT_PRIORITY);
