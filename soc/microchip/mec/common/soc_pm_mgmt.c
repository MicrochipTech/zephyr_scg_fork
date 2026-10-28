/*
 * Copyright (c) 2026 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <soc.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/pm.h>
#include <zephyr/sys/sys_io.h>

#define XEC_PCR_REG_BASE DT_REG_ADDR(DT_NODELABEL(pcr))

#define XEC_PCR_SLP_CR_OFS          0
#define XEC_PCR_SLP_CR_ALL_POS      3
#define XEC_PCR_SLP_CR_DEEP_SLP_POS 0

#define XEC_PCR_OSC_ID_OFS          0x0CU
#define XEC_PCR_OSC_ID_PLL_LOCK_POS 8

#define XEC_BASIC_TIMER_INSTANCES 6
#define XEC_BASIC_TIMER_SPACING   0x20U
#define XEC_BASIC_TIMER0_REG_BASE DT_REG_ADDR(DT_NODELABEL(timer0))
#define XEC_BASIC_TIMER_CR_OFS    0x10U
#define XEC_BASIC_TIMER_CR_EN_POS 0

#if DT_NODE_EXISTS(DT_NODELABEL(uart3))
#define MEC165XB_UART_INSTANCES 4
#elif DT_NODE_EXISTS(DT_NODELABEL(uart2))
#define MEC165XB_UART_INSTANCES 3
#else
#define MEC165XB_UART_INSTANCES 2
#endif

#define XEC_UART0_REG_BASE DT_REG_ADDR(DT_NODELABEL(uart0))

static uint8_t basic_timer_cr_save[XEC_BASIC_TIMER_INSTANCES];
static uint8_t uart_actv_save[MEC165XB_UART_INSTANCES];

static void save_basic_timers(void)
{
	mm_reg_t rb = (mm_reg_t)(XEC_BASIC_TIMER0_REG_BASE);

	for (uint32_t n = 0; n < XEC_BASIC_TIMER_INSTANCES; n++) {
		uint32_t temp = sys_read32(rb + XEC_BASIC_TIMER_CR_OFS);

		sys_write32(temp & ~BIT(XEC_BASIC_TIMER_CR_EN_POS), rb + XEC_BASIC_TIMER_CR_OFS);
		basic_timer_cr_save[n] = (uint8_t)(temp & BIT(XEC_BASIC_TIMER_CR_EN_POS));
		rb += XEC_BASIC_TIMER_SPACING;
	}
}

static void restore_basic_timers(void)
{
	mm_reg_t rb = (mm_reg_t)(XEC_BASIC_TIMER0_REG_BASE);

	for (uint32_t n = 0; n < XEC_BASIC_TIMER_INSTANCES; n++) {
		if (basic_timer_cr_save[n] != 0) {
			sys_set_bit(rb + XEC_BASIC_TIMER_CR_OFS, XEC_BASIC_TIMER_CR_EN_POS);
		}
		rb += XEC_BASIC_TIMER_SPACING;
	}
}

static void save_uarts(void)
{
	mm_reg_t rb = (mm_reg_t)(XEC_UART0_REG_BASE);

	for (uint32_t n = 0; n < MEC165XB_UART_INSTANCES; n++) {
		uart_actv_save[n] = sys_read8(rb + XEC_UART_LD_ACT_OFS);
		sys_write8(0, rb + XEC_UART_LD_ACT_OFS);
		rb += XEC_UART_SPACING;
	}
}

static void restore_uarts(void)
{
	mm_reg_t rb = (mm_reg_t)(XEC_UART0_REG_BASE);

	for (uint32_t n = 0; n < MEC165XB_UART_INSTANCES; n++) {
		sys_write8(uart_actv_save[n], rb + XEC_UART_LD_ACT_OFS);
		rb += XEC_UART_SPACING;
	}
}

/* Peripheral's that don't obey PCR sleep signals
 * Basic timers, and more ...
 */
static void soc_deep_sleep_periph_save(void)
{
	save_basic_timers();
	save_uarts();
}

static void soc_deep_sleep_periph_restore(void)
{
	restore_basic_timers();
	restore_uarts();
}

/*
 * Enable deep sleep mode in CM4 and MEC172x.
 * Enable CM4 deep sleep and sleep signals assertion on WFI.
 * Set MCHP Heavy sleep (PLL OFF when all CLK_REQ clear) and SLEEP_ALL
 * to assert SLP_EN to all peripherals on WFI.
 * WFI triggers MEC HW to enter deep sleep.
 * On wake clear SLEEP_ALL manually, de-asserting peripheral SLP_EN, since no
 * ISR runs until the kernel idle thread restores the interrupt state.
 *
 * Interrupt masking around WFI is the architecture's job:
 * arch_pm_state_set_prepare() saves BASEPRI, sets PRIMASK so no ISR is entered
 * on wake, and clears BASEPRI so any enabled interrupt can wake the CPU.
 * arch_pm_state_set_finish() restores the saved BASEPRI and clears PRIMASK. It
 * is called only after the PLL has locked and peripherals are restored: ISRs
 * must not run while the chip is still on the ring oscillator.
 */
static void z_power_soc_deep_sleep(void)
{
	mm_reg_t pcrbase = (mm_reg_t)(XEC_PCR_REG_BASE);
	uint32_t msk = BIT(XEC_PCR_SLP_CR_DEEP_SLP_POS) | BIT(XEC_PCR_SLP_CR_ALL_POS);
	uint32_t val = BIT(XEC_PCR_SLP_CR_DEEP_SLP_POS) | BIT(XEC_PCR_SLP_CR_ALL_POS);
	unsigned int key;

	soc_deep_sleep_periph_save();

	SCB->SCR |= BIT(SCB_SCR_SLEEPDEEP_Pos);

	soc_mmcr_mask_set(pcrbase + XEC_PCR_SLP_CR_OFS, val, msk);

	key = arch_pm_state_set_prepare();

	__WFI(); /* triggers sleep hardware */
	__NOP();
	__NOP();

	soc_mmcr_mask_set(pcrbase + XEC_PCR_SLP_CR_OFS, 0, msk);

	SCB->SCR &= ~BIT(SCB_SCR_SLEEPDEEP_Pos);

	/* Wait for PLL to lock. The chip runs on a +/-50% ring oscillator until
	 * it does, so nothing timing-dependent may run before this completes.
	 */
	while (sys_test_bit(pcrbase + XEC_PCR_OSC_ID_OFS, XEC_PCR_OSC_ID_PLL_LOCK_POS) == 0) {
		__NOP();
	}

	soc_deep_sleep_periph_restore();

	arch_pm_state_set_finish(key);
}

/*
 * Light sleep: clear Cortex-M deep sleep enable, set MEC PCR SLEEP_ALL to
 * assert SLP_EN to peripherals, WFI, then clear SLEEP_ALL on wake. The PLL
 * stays on, so clocks are already normal when the CPU resumes.
 *
 * Interrupt masking around WFI is handled by the architecture helpers, as in
 * z_power_soc_deep_sleep() above.
 */
static void z_power_soc_sleep(void)
{
	mm_reg_t pcrbase = (mm_reg_t)(XEC_PCR_REG_BASE);
	uint32_t msk = BIT(XEC_PCR_SLP_CR_DEEP_SLP_POS) | BIT(XEC_PCR_SLP_CR_ALL_POS);
	uint32_t val = BIT(XEC_PCR_SLP_CR_ALL_POS);
	unsigned int key;

	SCB->SCR &= ~BIT(SCB_SCR_SLEEPDEEP_Pos);

	soc_mmcr_mask_set(pcrbase + XEC_PCR_SLP_CR_OFS, val, msk);

	key = arch_pm_state_set_prepare();

	__WFI(); /* triggers sleep hardware */
	__NOP();
	__NOP();

	soc_mmcr_mask_set(pcrbase + XEC_PCR_SLP_CR_OFS, 0, msk);

	arch_pm_state_set_finish(key);
}

/*
 * CONFIG_PM=y the kernel idle thread will
 *   arch_irq_lock()
 *   tick = get next timeout expiration
 *   pm_system_suspend(ticks) in pm subsystem
 *
 * pm_system_suspend(int32_t ticks) in subsys/power.c
 * if exit latency is acceptible.
 *   lock scheduler
 *   set pm post ops flag
 *   call pm_state_set()
 *
 * For deep sleep pm_system_suspend has executed all the driver power management call backs.
 */
void pm_state_set(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(substate_id);

	switch (state) {
	case PM_STATE_SUSPEND_TO_IDLE:
		z_power_soc_sleep();
		break;
	case PM_STATE_SUSPEND_TO_RAM:
		z_power_soc_deep_sleep();
		break;
	default:
		break;
	}
}

/*
 * Hardware resume work only. Interrupt state is not touched here: the
 * architecture helpers in z_power_soc_sleep() and z_power_soc_deep_sleep()
 * own the masking around WFI, and the kernel idle thread restores the lock it
 * took before pm_system_suspend(). All MEC hardware resume work is already
 * done by the time the sleep functions return, so there is nothing left to do.
 */
void pm_state_exit_post_ops(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(state);
	ARG_UNUSED(substate_id);
}
