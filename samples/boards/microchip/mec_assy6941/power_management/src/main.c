/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/pm/device.h>
#include <zephyr/logging/log.h>

#include "power_mgmt.h"

LOG_MODULE_REGISTER(mec_assy6941_pm, LOG_LEVEL_INF);

#define MAX_CYCLES 5U

BUILD_ASSERT(IS_ENABLED(CONFIG_PM), "this sample requires CONFIG_PM");

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

/* GPIO004 on the EVB, pulled up, used purely as a scope trigger: it is driven
 * low across the counter alarm phase so the PLL behaviour during that phase can
 * be captured on its own. See the README.
 */
static const struct gpio_dt_spec marker_gpio = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, gpios);

/*
 * CONFIG_PM alone leaves every device enabled across a low-power state, so the
 * peripherals that ignore the chip's SLP_EN signals keep their clock request
 * asserted and the PCR will not gate the PLL. The MEC SoC layer works around
 * that for the Basic Timers and the UARTs in its deep-sleep path, which is why
 * deep sleep works without CONFIG_PM_DEVICE.
 *
 * With CONFIG_PM_DEVICE and CONFIG_PM_DEVICE_SYSTEM_MANAGED the PM subsystem
 * suspends devices before entering the state and resumes them after, so a
 * driver's own pm_action handles its hardware instead. The two paths reach
 * deep sleep differently and both need testing, so report which one is built
 * rather than silently testing only one.
 *
 * Note the UART is the console here: with system-managed device PM it is
 * suspended across the sleep, so output stops at the suspend and resumes
 * afterwards. That is expected, not a hang.
 */
static void report_build_config(void)
{
	LOG_INF("MEC ASSY6941 power management sample");

	if (IS_ENABLED(CONFIG_PM_DEVICE)) {
		if (IS_ENABLED(CONFIG_PM_DEVICE_SYSTEM_MANAGED)) {
			LOG_INF("Device PM: system managed (drivers suspended by the PM core)");
		} else if (IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME)) {
			LOG_INF("Device PM: runtime only (drivers not suspended by the PM core)");
		} else {
			LOG_INF("Device PM: enabled, neither system managed nor runtime");
		}
	} else {
		LOG_INF("Device PM: disabled (SoC masks non-compliant peripherals)");
	}

	LOG_INF("Kernel timer: %u Hz cycle counter, %u Hz tick",
		(unsigned int)sys_clock_hw_cycles_per_sec(),
		(unsigned int)CONFIG_SYS_CLOCK_TICKS_PER_SEC);
}

#if defined(CONFIG_PM_DEVICE) && defined(CONFIG_PM_DEVICE_SYSTEM_MANAGED)
/*
 * With system-managed device PM the console UART is suspended while the system
 * sleeps. Give its driver a chance to drain the FIFO before the suspend, so the
 * preceding log lines are not lost mid-transmission.
 */
static void flush_console(void)
{
	k_msleep(50);
}
#else
static void flush_console(void)
{
}
#endif

int main(void)
{
	int ret;
	int result = 0;

	report_build_config();

	ret = gpio_pin_configure_dt(&marker_gpio, GPIO_OUTPUT_HIGH);
	if (ret != 0) {
		LOG_ERR("Could not configure the GPIO004 scope marker (%d)", ret);
	}

	flush_console();

	ret = test_pwr_mgmt_singlethread(false, MAX_CYCLES);
	if (ret != 0) {
		LOG_ERR("Single-thread test failed: %d", ret);
		result = ret;
	}
	flush_console();

	ret = test_pwr_mgmt_singlethread(true, MAX_CYCLES);
	if (ret != 0) {
		LOG_ERR("Single-thread test (logging) failed: %d", ret);
		result = ret;
	}
	flush_console();

	ret = test_pwr_mgmt_multithread(false, MAX_CYCLES);
	if (ret != 0) {
		LOG_ERR("Multi-thread test failed: %d", ret);
		result = ret;
	}
	flush_console();

	ret = test_pwr_mgmt_multithread(true, MAX_CYCLES);
	if (ret != 0) {
		LOG_ERR("Multi-thread test (logging) failed: %d", ret);
		result = ret;
	}
	flush_console();

	/* Scope trigger: the pin idles high and is driven low across the phase. */
	gpio_pin_set_raw(marker_gpio.port, marker_gpio.pin, 0);
	ret = test_pwr_mgmt_counter_alarm();
	gpio_pin_set_raw(marker_gpio.port, marker_gpio.pin, 1);

	if (ret != 0) {
		LOG_ERR("Counter alarm test failed: %d", ret);
		result = ret;
	}
	flush_console();

	ret = test_pwr_mgmt_check_uptime();
	if (ret != 0) {
		result = ret;
	}

	if (result != 0) {
		LOG_ERR("PM sample failed: %d", result);
		printk("PM sample FAILED\n");
		return result;
	}

	LOG_INF("PM sample completed");
	printk("PM sample PASSED\n");

	return 0;
}
