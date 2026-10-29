/*
 * Copyright (c) 2019 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT microchip_xec_timer

/**
 * @file
 * @brief Microchip XEC Counter driver
 *
 * This is the driver for the 16/32-bit counters on the Microchip SoCs.
 *
 * Notes:
 * - The counters are running in down counting mode.
 * - Interrupts are triggered (if enabled) when the counter
 *   reaches zero.
 * - These are not free running counters where there are separate
 *   compare values for interrupts. When setting single shot alarms,
 *   the counter values are changed so that interrupts are triggered
 *   when the counters reach zero.
 */

#include <zephyr/irq.h>
#include <zephyr/dt-bindings/interrupt-controller/mchp-xec-ecia.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(counter_mchp_xec, CONFIG_COUNTER_LOG_LEVEL);

#include <zephyr/drivers/counter.h>
#include <zephyr/pm/device.h>
#include <soc.h>
#include <errno.h>
#include <stdbool.h>

struct counter_xec_config {
	struct counter_config_info info;
	void (*config_func)(void);
	uint32_t base_address;
	uint16_t prescaler;
	uint8_t girq_id;
	uint8_t girq_bit;
	uint8_t enc_pcr;
};

struct counter_xec_data {
	counter_alarm_callback_t alarm_cb;
	counter_top_callback_t top_cb;
	void *user_data;
#ifdef CONFIG_PM_DEVICE
	/* Control register as of the last suspend, and whether the block was
	 * counting then. The count itself is not saved: it holds its value
	 * across the enable being cleared, and a consumer that depends on the
	 * count holds an alarm or top callback, which makes the device busy and
	 * so not suspended at all.
	 */
	uint32_t ctrl_save;
	bool resume_running;
	/* Whether the last suspend actually stopped the block. False when it
	 * was left running for an armed event, so the resume knows there is
	 * nothing to put back.
	 */
	bool suspended;
#endif
};

#define COUNTER_XEC_REG_BASE(_dev)                                                                 \
	((struct btmr_regs *)((const struct counter_xec_config *const)_dev->config)->base_address)

#define COUNTER_XEC_CONFIG(_dev) (((const struct counter_xec_config *const)_dev->config))

#define COUNTER_XEC_DATA(_dev) ((struct counter_xec_data *)dev->data)

static int counter_xec_start(const struct device *dev)
{
	struct btmr_regs *counter = COUNTER_XEC_REG_BASE(dev);

	if (counter->CTRL & MCHP_BTMR_CTRL_ENABLE) {
		return -EALREADY;
	}

	counter->CTRL |= (MCHP_BTMR_CTRL_ENABLE | MCHP_BTMR_CTRL_START);

	LOG_DBG("%p Counter started", dev);

	return 0;
}

static int counter_xec_stop(const struct device *dev)
{
	struct btmr_regs *counter = COUNTER_XEC_REG_BASE(dev);
	uint32_t reg;

	if (!(counter->CTRL & MCHP_BTMR_CTRL_ENABLE)) {
		/* Already stopped, nothing to do */
		return 0;
	}

	reg = counter->CTRL;
	reg &= ~MCHP_BTMR_CTRL_ENABLE;
	reg &= ~MCHP_BTMR_CTRL_START;
	reg &= ~MCHP_BTMR_CTRL_HALT;
	reg &= ~MCHP_BTMR_CTRL_RELOAD;
	reg &= ~MCHP_BTMR_CTRL_AUTO_RESTART;
	counter->CTRL = reg;

	counter->IEN = MCHP_BTMR_INTDIS;
	counter->CNT = counter->PRLD;

	LOG_DBG("%p Counter stopped", dev);

	return 0;
}

static int counter_xec_get_value(const struct device *dev, uint32_t *ticks)
{
	struct btmr_regs *counter = COUNTER_XEC_REG_BASE(dev);

	*ticks = counter->CNT;
	return 0;
}

static int counter_xec_set_alarm(const struct device *dev, uint8_t chan_id,
				 const struct counter_alarm_cfg *alarm_cfg)
{
	struct btmr_regs *counter = COUNTER_XEC_REG_BASE(dev);
	struct counter_xec_data *data = COUNTER_XEC_DATA(dev);

	if (chan_id != 0) {
		LOG_ERR("Invalid channel id %u", chan_id);
		return -ENOTSUP;
	}

	/* Interrupts are only triggered when the counter reaches 0.
	 * So only relative alarms are supported.
	 */
	if (alarm_cfg->flags & COUNTER_ALARM_CFG_ABSOLUTE) {
		return -ENOTSUP;
	}

	if (data->alarm_cb != NULL) {
		return -EBUSY;
	}

	if (!alarm_cfg->callback) {
		return -EINVAL;
	}

	if (alarm_cfg->ticks > counter->PRLD) {
		return -EINVAL;
	}

	counter->CNT = alarm_cfg->ticks;

	data->alarm_cb = alarm_cfg->callback;
	data->user_data = alarm_cfg->user_data;

	counter->IEN = MCHP_BTMR_INTEN;

	LOG_DBG("%p Counter alarm set to %u ticks", dev, alarm_cfg->ticks);

	counter->CTRL |= MCHP_BTMR_CTRL_START;

	return 0;
}

static int counter_xec_cancel_alarm(const struct device *dev, uint8_t chan_id)
{
	struct btmr_regs *counter = COUNTER_XEC_REG_BASE(dev);
	struct counter_xec_data *data = COUNTER_XEC_DATA(dev);

	if (chan_id != 0) {
		LOG_ERR("Invalid channel id %u", chan_id);
		return -ENOTSUP;
	}

	counter->CTRL &= ~MCHP_BTMR_CTRL_START;
	counter->IEN = MCHP_BTMR_INTDIS;

	data->alarm_cb = NULL;
	data->user_data = NULL;

	LOG_DBG("%p Counter alarm canceled", dev);

	return 0;
}

static uint32_t counter_xec_get_pending_int(const struct device *dev)
{
	struct btmr_regs *counter = COUNTER_XEC_REG_BASE(dev);

	return counter->STS;
}

static uint32_t counter_xec_get_top_value(const struct device *dev)
{
	struct btmr_regs *counter = COUNTER_XEC_REG_BASE(dev);

	return counter->PRLD;
}

static int counter_xec_set_top_value(const struct device *dev, const struct counter_top_cfg *cfg)
{
	struct btmr_regs *counter = COUNTER_XEC_REG_BASE(dev);
	const struct counter_xec_config *counter_cfg = COUNTER_XEC_CONFIG(dev);
	struct counter_xec_data *data = COUNTER_XEC_DATA(dev);
	int ret = 0;
	bool restart;

	if (data->alarm_cb) {
		return -EBUSY;
	}

	if (cfg->ticks > counter_cfg->info.max_top_value) {
		return -EINVAL;
	}

	restart = ((counter->CTRL & MCHP_BTMR_CTRL_START) != 0U);

	counter->CTRL &= ~MCHP_BTMR_CTRL_START;

	if (cfg->flags & COUNTER_TOP_CFG_DONT_RESET) {
		if (counter->CNT > cfg->ticks) {
			ret = -ETIME;

			if (cfg->flags & COUNTER_TOP_CFG_RESET_WHEN_LATE) {
				counter->CNT = cfg->ticks;
			}
		}
	} else {
		counter->CNT = cfg->ticks;
	}

	counter->PRLD = cfg->ticks;

	data->top_cb = cfg->callback;
	data->user_data = cfg->user_data;

	if (data->top_cb) {
		counter->IEN = MCHP_BTMR_INTEN;
		counter->CTRL |= MCHP_BTMR_CTRL_AUTO_RESTART;
	} else {
		counter->IEN = MCHP_BTMR_INTDIS;
		counter->CTRL &= ~MCHP_BTMR_CTRL_AUTO_RESTART;
	}

	LOG_DBG("%p Counter top value was set to %u", dev, cfg->ticks);

	if (restart) {
		counter->CTRL |= MCHP_BTMR_CTRL_START;
	}

	return ret;
}

static void counter_xec_isr(const struct device *dev)
{
	struct btmr_regs *counter = COUNTER_XEC_REG_BASE(dev);
	const struct counter_xec_config *counter_cfg = COUNTER_XEC_CONFIG(dev);
	struct counter_xec_data *data = COUNTER_XEC_DATA(dev);
	counter_alarm_callback_t alarm_cb;
	void *user_data;

	counter->STS = MCHP_BTMR_STS_ACTIVE;

	soc_ecia_girq_status_clear(counter_cfg->girq_id, counter_cfg->girq_bit);

	LOG_DBG("%p Counter ISR", dev);

	if (data->alarm_cb) {
		/* Alarm is one-shot, so disable interrupt and callback */
		counter->IEN = MCHP_BTMR_INTDIS;

		alarm_cb = data->alarm_cb;
		data->alarm_cb = NULL;
		user_data = data->user_data;

		alarm_cb(dev, 0, counter->CNT, user_data);
	} else if (data->top_cb) {
		data->top_cb(dev, data->user_data);
	}
}

#ifdef CONFIG_PM_DEVICE
/*
 * Basic Timers ignore the chip's SLP_EN signal: a running one keeps its
 * CLK_REQ asserted and the PCR will not gate the PLL while any CLK_REQ is set.
 * Clearing the block enable is what drops CLK_REQ.
 *
 * A counter with a pending alarm or a periodic top callback must keep running,
 * or the event the application asked for never arrives. Such a counter is left
 * alone on suspend. The consequence is confined to power: the PM sequence still
 * runs and the CPU still enters the low-power state, but the PCR sees this
 * block's CLK_REQ and keeps the PLL on, so the part draws more than it would
 * in full deep sleep. That cost is the application's choice, taken only when it
 * has actually armed a counter event.
 *
 * Suspend deliberately reports success in that case rather than an error.
 * pm_suspend_devices() treats any error other than -ENOSYS, -ENOTSUP or
 * -EALREADY as a failure to suspend the system, which aborts the whole
 * transition and resumes every device already suspended. Refusing here would
 * therefore cost the sleep entirely, not just the PLL.
 *
 * A counter that is merely running with nothing armed has no event to miss, so
 * it is stopped and deep sleep proceeds. The PM core separately skips a device
 * that is busy, has runtime PM enabled, or is a wake-up source, so marking a
 * device busy is still the way to keep it untouched for any other reason.
 */
static int counter_xec_pm_action(const struct device *dev, enum pm_device_action action)
{
	struct btmr_regs *counter = COUNTER_XEC_REG_BASE(dev);
	const struct counter_xec_config *counter_cfg = COUNTER_XEC_CONFIG(dev);
	struct counter_xec_data *data = COUNTER_XEC_DATA(dev);

	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		/* An armed alarm or a periodic top callback needs the block to
		 * keep counting, so leave it running and let the PLL stay on.
		 * Nothing is saved, and the matching resume has nothing to do.
		 */
		if ((data->alarm_cb != NULL) || (data->top_cb != NULL)) {
			data->suspended = false;
			break;
		}

		data->ctrl_save = counter->CTRL;
		data->resume_running = (data->ctrl_save & MCHP_BTMR_CTRL_ENABLE) != 0U;
		data->suspended = true;

		/* Clear START as well as ENABLE so the block cannot resume
		 * counting on its own when the enable comes back; the resume
		 * decides that from resume_running.
		 */
		counter->CTRL &= ~(MCHP_BTMR_CTRL_ENABLE | MCHP_BTMR_CTRL_START);
		soc_xec_pcr_sleep_en_set(counter_cfg->enc_pcr);
		break;
	case PM_DEVICE_ACTION_RESUME:
		if (!data->suspended) {
			break;
		}

		soc_xec_pcr_sleep_en_clear(counter_cfg->enc_pcr);

		/* Restore the configuration (prescale, count direction,
		 * auto-restart) without starting the block, then start it only
		 * if it was counting when suspended.
		 */
		counter->CTRL = data->ctrl_save &
				~(MCHP_BTMR_CTRL_ENABLE | MCHP_BTMR_CTRL_START);

		if (data->resume_running) {
			counter->CTRL |= (MCHP_BTMR_CTRL_ENABLE | MCHP_BTMR_CTRL_START);
		}
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}
#endif /* CONFIG_PM_DEVICE */

static DEVICE_API(counter, counter_xec_api) = {
	.start = counter_xec_start,
	.stop = counter_xec_stop,
	.get_value = counter_xec_get_value,
	.set_alarm = counter_xec_set_alarm,
	.cancel_alarm = counter_xec_cancel_alarm,
	.set_top_value = counter_xec_set_top_value,
	.get_pending_int = counter_xec_get_pending_int,
	.get_top_value = counter_xec_get_top_value,
};

static int counter_xec_init(const struct device *dev)
{
	struct btmr_regs *counter = COUNTER_XEC_REG_BASE(dev);
	const struct counter_xec_config *counter_cfg = COUNTER_XEC_CONFIG(dev);

	soc_xec_pcr_sleep_en_clear(counter_cfg->enc_pcr);

	counter_xec_stop(dev);

	counter->CTRL &= ~MCHP_BTMR_CTRL_COUNT_UP;
	counter->CTRL |= (counter_cfg->prescaler << MCHP_BTMR_CTRL_PRESCALE_POS) &
			 MCHP_BTMR_CTRL_PRESCALE_MASK;

	/* Set preload and actually pre-load the counter */
	counter->PRLD = counter_cfg->info.max_top_value;
	counter->CNT = counter_cfg->info.max_top_value;

	soc_ecia_girq_ctrl(counter_cfg->girq_id, counter_cfg->girq_bit, 1);

	counter_cfg->config_func();

	return 0;
}

#define DEV_CFG_GIRQ(inst, idx)     MCHP_XEC_ECIA_GIRQ(DT_INST_PROP_BY_IDX(inst, girqs, idx))
#define DEV_CFG_GIRQ_POS(inst, idx) MCHP_XEC_ECIA_GIRQ_POS(DT_INST_PROP_BY_IDX(inst, girqs, idx))

#define COUNTER_XEC_INIT(inst)                                                                     \
	static void counter_xec_irq_config_##inst(void);                                           \
                                                                                                   \
	static struct counter_xec_data counter_xec_dev_data_##inst;                                \
                                                                                                   \
	static struct counter_xec_config counter_xec_dev_config_##inst = {                         \
		.info =                                                                            \
			{                                                                          \
				.max_top_value = DT_INST_PROP(inst, max_value),                    \
				.freq = DT_INST_PROP(inst, clock_frequency) /                      \
					(DT_INST_PROP(inst, prescaler) + 1),                       \
				.flags = 0,                                                        \
				.channels = 1,                                                     \
			},                                                                         \
                                                                                                   \
		.config_func = counter_xec_irq_config_##inst,                                      \
		.base_address = DT_INST_REG_ADDR(inst),                                            \
		.prescaler = DT_INST_PROP(inst, prescaler),                                        \
		.girq_id = DEV_CFG_GIRQ(inst, 0),                                                  \
		.girq_bit = DEV_CFG_GIRQ_POS(inst, 0),                                             \
		.enc_pcr = DT_INST_PROP(inst, pcr_src),                                            \
	};                                                                                         \
                                                                                                   \
	PM_DEVICE_DT_INST_DEFINE(inst, counter_xec_pm_action);                                     \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, counter_xec_init, PM_DEVICE_DT_INST_GET(inst),                 \
			      &counter_xec_dev_data_##inst, &counter_xec_dev_config_##inst,        \
			      POST_KERNEL, CONFIG_COUNTER_INIT_PRIORITY, &counter_xec_api);        \
                                                                                                   \
	static void counter_xec_irq_config_##inst(void)                                            \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority), counter_xec_isr,      \
			    DEVICE_DT_INST_GET(inst), 0);                                          \
		irq_enable(DT_INST_IRQN(inst));                                                    \
	}

DT_INST_FOREACH_STATUS_OKAY(COUNTER_XEC_INIT)
