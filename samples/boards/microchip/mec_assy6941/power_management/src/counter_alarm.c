/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Deep sleep with a counter alarm armed.
 *
 * A Basic Timer ignores the chip's SLP_EN signal, so one left running keeps
 * its CLK_REQ asserted and the PCR will not gate the PLL. The XEC counter
 * driver's pm_action therefore stops a counter on suspend, except when an
 * alarm or a periodic top callback is armed: stopping it would drop the event
 * the application asked for, so the block is left running and the PLL stays
 * on for the duration of the sleep.
 *
 * This exercises that branch, which nothing else in the sample reaches. Two
 * things are worth watching on a scope while it runs:
 *
 *   - TEST_CLK_OUT (GPIO060) should stay active through the sleep, where the
 *     other phases show it gated. That is the PLL being held up, which is the
 *     expected cost rather than a fault.
 *   - With CONFIG_SOC_MEC_PM_DBG_PCR_CLK_REQ_VBM the saved CLK_REQ registers
 *     should show this timer's bit set on entry.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/logging/log.h>

#include "power_mgmt.h"

LOG_MODULE_DECLARE(mec_assy6941_pm, LOG_LEVEL_INF);

#if DT_HAS_CHOSEN(zephyr_counter)

#define ALARM_COUNTER_NODE DT_CHOSEN(zephyr_counter)

/* Long enough that the alarm cannot plausibly fire before the sleep is
 * entered, short enough to stay inside a 32-bit tick count at 48 MHz
 * (89 s there) and to keep the sample's runtime reasonable.
 */
#define ALARM_DELAY_MS 4000U

/* Slept past the deep-sleep residency, and past the alarm, so the alarm is
 * what ends the sleep rather than the sleep ending first.
 */
#define ALARM_SLEEP_MS 6000U

static const struct device *const counter_dev = DEVICE_DT_GET(ALARM_COUNTER_NODE);

static struct k_sem alarm_fired;

/*
 * The ticks argument is not recorded. This driver's ISR passes counter->CNT,
 * and by then the one-shot has expired and the block has reloaded from PRLD,
 * so the value read is the preload rather than the count at the alarm: on a
 * 32-bit timer it is always UINT32_MAX. The uptime delta around the sleep is
 * the meaningful measure, and the semaphore is what says the alarm arrived.
 */
static void alarm_handler(const struct device *dev, uint8_t chan_id, uint32_t ticks,
			  void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan_id);
	ARG_UNUSED(ticks);
	ARG_UNUSED(user_data);

	k_sem_give(&alarm_fired);
}

int test_pwr_mgmt_counter_alarm(void)
{
	struct counter_alarm_cfg cfg = {
		.callback = alarm_handler,
		.user_data = NULL,
		.flags = 0,
	};
	int64_t before;
	int64_t elapsed;
	int ret;

	if (!device_is_ready(counter_dev)) {
		LOG_ERR("Counter %s not ready", counter_dev->name);
		return -ENODEV;
	}

	LOG_WRN("Deep sleep with a counter alarm armed on %s", counter_dev->name);

	k_sem_init(&alarm_fired, 0, 1);

	ret = counter_start(counter_dev);
	if ((ret != 0) && (ret != -EALREADY)) {
		LOG_ERR("counter_start: %d", ret);
		return ret;
	}

	cfg.ticks = counter_us_to_ticks(counter_dev, (uint64_t)ALARM_DELAY_MS * USEC_PER_MSEC);

	ret = counter_set_channel_alarm(counter_dev, 0, &cfg);
	if (ret != 0) {
		LOG_ERR("counter_set_channel_alarm: %d", ret);
		(void)counter_stop(counter_dev);
		return ret;
	}

	LOG_INF("Alarm armed for %u ms (%u ticks); PLL is expected to stay on",
		ALARM_DELAY_MS, cfg.ticks);

	/* Sleep past the deep-sleep residency so the PM policy picks that
	 * state with the counter still running.
	 */
	before = k_uptime_get();
	k_msleep(ALARM_SLEEP_MS);
	elapsed = k_uptime_get() - before;

	if (k_sem_take(&alarm_fired, K_NO_WAIT) != 0) {
		LOG_ERR("Alarm did not fire across the sleep");
		(void)counter_cancel_channel_alarm(counter_dev, 0);
		(void)counter_stop(counter_dev);
		return -EIO;
	}

	LOG_INF("Alarm fired across deep sleep; uptime advanced %lld ms of %u ms requested",
		(long long)elapsed, ALARM_SLEEP_MS);

	/* The driver's pm_action left the block alone, so the counter is still
	 * running here. Stop it so the following phases can gate the PLL.
	 */
	ret = counter_stop(counter_dev);
	if (ret != 0) {
		LOG_ERR("counter_stop: %d", ret);
		return ret;
	}

	return 0;
}

#else /* !DT_HAS_CHOSEN(zephyr_counter) */

int test_pwr_mgmt_counter_alarm(void)
{
	LOG_INF("No chosen zephyr,counter; skipping the counter alarm phase");

	return 0;
}

#endif /* DT_HAS_CHOSEN(zephyr_counter) */
