/*
 * Copyright (c) 2026 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <soc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/pinctrl.h>

#define SLEEP_MS   3000U
#define ITERATIONS 4

/* Test-clock-out pad (GPIO060, AF2). Not owned by a device driver, so define
 * its pin-control config here and apply it manually from main().
 */
#define TST_CLK_OUT_NODE DT_NODELABEL(tst_clk_out)
PINCTRL_DT_DEFINE(TST_CLK_OUT_NODE);
static const struct pinctrl_dev_config *tst_clk_out_pcfg =
	PINCTRL_DT_DEV_CONFIG_GET(TST_CLK_OUT_NODE);

/*
 * Exercise the CCT kernel timer across tickless sleeps. Each iteration sleeps
 * long enough to let the PM policy enter a low-power state (the board's
 * suspend-to-ram residency is 2 s), which hands timekeeping to the 32 KHz RTOS
 * timer companion. If both the system timer and the LPM hooks are correct, the
 * uptime delta and the free-running cycle-counter delta should both track the
 * requested sleep time.
 */
int main(void)
{
	int ret;

	printk("XEC CCT kernel timer sample\n");
	printk("system timer HW cycles/sec: %u\n", sys_clock_hw_cycles_per_sec());
	printk("ticks/sec: %d\n", CONFIG_SYS_CLOCK_TICKS_PER_SEC);

	/* Route the test clock to GPIO060 for scope measurement. */
	ret = pinctrl_apply_state(tst_clk_out_pcfg, PINCTRL_STATE_DEFAULT);
	if (ret) {
		printk("test-clock-out pinctrl apply failed: %d\n", ret);
	} else {
		printk("test clock routed to GPIO060 (AF2)\n");
	}

	for (int i = 0; i < ITERATIONS; i++) {
		uint32_t cyc0 = k_cycle_get_32();
		int64_t up0 = k_uptime_get();

		k_msleep(SLEEP_MS);

		uint32_t cyc1 = k_cycle_get_32();
		int64_t up1 = k_uptime_get();

		/* Unsigned subtraction is wrap-correct for the 32-bit counter. */
		uint32_t dcyc = cyc1 - cyc0;

		printk("iter %d: requested %u ms, uptime +%lld ms, "
		       "cycles +%u (%u ms by cycle count)\n",
		       i, SLEEP_MS, (long long)(up1 - up0), dcyc,
		       k_cyc_to_ms_near32(dcyc));
	}

	printk("Sample complete\n");

	return 0;
}
