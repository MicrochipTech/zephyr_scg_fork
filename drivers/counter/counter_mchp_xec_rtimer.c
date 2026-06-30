/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT microchip,xec-rtimer-counter

/**
 * @file
 * @brief Microchip XEC RTOS timer counter driver
 *
 * The RTOS timer is a 32-bit down counter using a fixed 32 KHz input clock.
 * When the timer count reaches 0 it signals an interrupt if enabled and
 * if enabled can reload the counter from the preload register.
 */


#include <soc.h>
#include <errno.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include <device_mec5.h>
#include <mec_rtimer_api.h>

LOG_MODULE_REGISTER(counter_xec_rtimer, CONFIG_COUNTER_LOG_LEVEL);

#define XEC_RT_MAIN_CLK_FREQ 32768U

#define XEC_RT_CNT_OFS 0    /* 32-bit R/W down counter */
#define XEC_RT_PLD_OFS 0x4U /* 32-bit R/W */
#define XEC_RT_CR_OFS  0x8U /* 32-bit R/W */
#define XEC_RT_SI_OFS  0xCU /* 32-bit WO */

/* control register */
#define XEC_RT_CR_ACTV_POS     0 /* block activate */
#define XEC_RT_CR_AUTO_RLD_POS 1 /* count 1 -> 0 causes HW to load count with preload */
#define XEC_RT_CR_START_POS    2
#define XEC_RT_CR_EXT_HALT_POS 3 /* halt during debugger active, break points, etc. */
#define XEC_RT_CR_FW_HALT_POS  4 /* halt timer, when cleared resumes counting down */

/* NOTE: struct counter_config_info must be first member */
struct counter_xec_rt_devcfg {
	struct counter_config_info info;
	mm_reg_t base;
	void (*irq_cfg_func)(void);
	uint8_t girq;
	uint8_t girq_pos;
};

struct counter_xec_rt_dev_data {
	uint32_t top_ticks;
	counter_alarm_callback_t alarm_cb;
	void *alarm_cb_ud;
	counter_top_callback_t top_cb;
	void *top_cb_ud;
};

static void xec_rt_restart(const struct device *dev, uint32_t new_count, uint8_t restart)
{
	const struct counter_xec_rt_devcfg *xcfg = dev->config;
	mm_reg_t base = xcfg->base;
	uint32_t cr = sys_read32(base + XEC_RT_CR_OFS);

	cr &= (uint32_t)~BIT(XEC_RT_CR_FW_HALT_POS);
	cr |= BIT(XEC_RT_CR_ACTV_POS);
	if (restart != 0) {
		cr |= BIT(XEC_RT_CR_START_POS);
	}

	sys_write32(0, base + XEC_RT_CR_OFS);
	sys_write32(new_count, base + XEC_RT_PLD_OFS);
	sys_write32(cr, base + XEC_RT_CR_OFS);
}

/* API - Start counter device in free running mode
 * RTOS timer implements a 32-bit count down counter.
 * On start, it loads the value in the preload register into
 * it count register and begins counting down. Once the count
 * register reaches 0 it stops counting and asserts its interrupt
 * signal. If auto-reload is enabled it will load count from preload
 * and begin counting down again.
 */
static void counter_xec_rt_start(const struct device *dev)
{
	const struct counter_xec_rt_devcfg *xcfg = dev->config;

	sys_set_bit(xcfg->base + XEC_RT_CR_OFS, XEC_RT_CR_START_POS);
}

/* API - Stop the counter
 * Clears the RTOS timer start bit.
 * Clear any pending interrupt after stopping
 */
static void counter_xec_rt_stop(const struct device *dev)
{
	const struct counter_xec_rt_devcfg *xcfg = dev->config;

	sys_clear_bit(xcfg->base + XEC_RT_CR_OFS, XEC_RT_CR_START_POS);
	soc_ecia_girq_status_clear(xcfg->girq, xcfg->girq_pos);
}

/* API - Get current counter value */
static int counter_xec_rt_get_value(const struct device *dev, uint32_t *ticks)
{
	const struct counter_xec_rt_devcfg *xcfg = dev->config;

	if (ticks == NULL) {
		return -EINVAL;
	}

	*ticks = sys_read32(xcfg->base + XEC_RT_CNT_OFS);

	return 0;
}

/* API - Set a single-shot alarm
 * RTOS timer only signals an event when it reaches terminal count (0).
 * Setting an alarm means changing the current count value while it may
 * be running.
 * If the basic timer is running
 *    Halt timer, write new value to count, and unhalt
 * Else basic timer is not running
 *    Write alarm value to preload and do not start
 *
 * Notes:
 * Alarm callback is mandatory.
 * Absolute alarm is not supported because basic timer interrupt is only
 * triggered when the counter reaches its terminal value.
 */
static int counter_xec_rt_set_alarm(const struct device *dev, uint8_t chan_id,
				    const struct counter_alarm_cfg *alarm_cfg)
{
	const struct counter_xec_rt_devcfg *xcfg = dev->config;
	struct counter_xec_rt_dev_data *const xdat = dev->data;
	mm_reg_t base = xcfg->base;

	if (chan_id != 0) {
		LOG_ERR("Invalid channel id %u", chan_id);
		return -ENOTSUP;
	}

	if (xdat->alarm_cb != NULL) {
		return -EBUSY;
	}

	if (alarm_cfg == NULL) {
		LOG_ERR("Invalid alarm config");
		return -EINVAL;
	}

	if (alarm_cfg->callback == NULL) {
		LOG_ERR("Alarm callback function cannot be null");
		return -EINVAL;
	}

	if ((alarm_cfg->flags & COUNTER_ALARM_CFG_ABSOLUTE) != 0) {
		LOG_ERR("Absolute alarm is not supported");
		return -ENOTSUP;
	}

	if (alarm_cfg->ticks > xdat->top_ticks) {
		LOG_DBG("Request alarm ticks %u > %u current top",
			alarm_cfg->ticks, xdat->top_ticks);
		return -EINVAL;
	}

	soc_ecia_girq_ctrl(xcfg->girq, xcfg->girq_pos, 0);

	xdat->alarm_cb = alarm_cfg->callback;
	xdat->alarm_cb_ud = alarm_cfg->user_data;

	xec_rt_restart(dev, alarm_cfg->ticks, 1);
	soc_ecia_girq_ctrl(xcfg->girq, xcfg->girq_pos, 1);

	return 0;
}

/* Cancels an alarm if previously configured.
 * Do not disable interrupt if a top callback is installed.
 */
static int counter_xec_rt_cancel_alarm(const struct device *dev, uint8_t chan_id)
{
	const struct counter_xec_rt_devcfg *xcfg = dev->config;
	struct counter_xec_rt_dev_data *const xdat = dev->data;
	mm_reg_t base = xcfg->base;

	if (chan_id != 0) {
		LOG_ERR("Invalid channel id %u", chan_id);
		return -ENOTSUP;
	}

	soc_ecia_girq_ctrl(xcfg->girq, xcfg->girq_pos, 0);

	xdat->alarm_cb = NULL;
	xdat->alarm_cb_ud = NULL;

	if (xdat->top_cb != NULL) {
		soc_ecia_girq_ctrl(xcfg->girq, xcfg->girq_pos, 1);
	}

	LOG_DBG("%p Counter alarm canceled", dev);

	return 0;
}

static uint32_t counter_xec_rt_get_pending_int(const struct device *dev)
{
	const struct counter_xec_rt_devcfg *xcfg = dev->config;
	uint32_t status = 0;

	soc_ecia_girq_status(xcfg->girq, &status);

	return ((status >> xcfg->girq_pos) & BIT(0));
}

/* API - Return the current count top value.
 * We return the current top value set by driver init or successful
 * call to the set top value API.
 */
static uint32_t counter_xec_rt_get_top_value(const struct device *dev)
{
	struct counter_xec_rt_dev_data *const xdat = dev->data;

	return xdat->top_ticks;
}

/* API - Set a new top value and optional callback.
 * cfg->flags
 * COUNTER_TOP_CFG_DONT_RESET - Allow counter to free run while setting new top
 * COUNTER_TOP_CFG_RESET_WHEN_LATE - Reset counter if new top value will go out of bounds
 * NOTES:
 * Basic timer COUNT register should not be written while it is running.
 * Preload can be written while timer is running but there is a race condition
 * if the write is issues when the timer is about to reach its terminal count.
 * Hardware does not implement a free running counter therefore we can't support
 * COUNTER_TOP_CFG_DONT_RESET.
 *
 */
static int counter_xec_rt_set_top_value(const struct device *dev,
					const struct counter_top_cfg *cfg)
{
	const struct counter_xec_rt_devcfg *xcfg = dev->config;
	struct counter_xec_rt_dev_data *const xdat = dev->data;
	const struct counter_config_info *info = &xcfg->info;
	mm_reg_t base = xcfg->base;
	uint32_t ticks = 0, restart = 0;
	int ret = 0;

	if (xdat->alarm_cb) {
		LOG_ERR("Changing top while an alarm is active is not allowed");
		return -EBUSY;
	}

	if (cfg == NULL) {
		LOG_ERR("Invalid top config");
		return -EINVAL;
	}

	if (cfg->ticks > info->max_top_value) {
		LOG_ERR("New top exceeds max top value");
		return -EINVAL;
	}

	if ((cfg->flags & COUNTER_TOP_CFG_DONT_RESET) != 0) {
		LOG_ERR("Updating top value without reset is not supported");
		return -ENOTSUP;
	}

	ticks = cfg->ticks;

	soc_ecia_girq_ctrl(xcfg->girq, xcfg->girq_pos, 0);

	xdat->top_ticks = ticks;
	xdat->top_cb = cfg->callback;
	xdat->top_cb_ud = cfg->user_data;

	/* TODO HW race condition at 0->1 */
	if (sys_read32(base + XEC_RT_CNT_OFS) != 0) {
		restart = 1u;
	}

	xec_rt_restart(dev, ticks, restart);

	if (xdat->top_cb != NULL) {
		sys_set_bit(base + XEC_RT_CR_OFS, XEC_RT_CR_AUTO_RLD_POS);
		soc_ecia_girq_ctrl(xcfg->girq, xcfg->girq_pos, 1);
	} else {
		sys_clear_bit(base + XEC_RT_CR_OFS, XEC_RT_CR_AUTO_RLD_POS);
	}

	return ret;
}

static uint32_t counter_xec_rt_get_freq(const struct device *dev)
{
	return (uint32_t)(XEC_RT_MAIN_CLK_FREQ);
}

static void counter_xec_rt_isr(const struct device *dev)
{
	const struct counter_xec_rt_devcfg *xcfg = dev->config;
	struct counter_xec_rt_dev_data *const xdat = dev->data;
	mm_reg_t base = xcfg->base;
	counter_alarm_callback_t alarm_cb = NULL;
	void *user_data = NULL;
	uint32_t status = soc_ecia_girq_is_result(xcfg->girq, BIT(xcfg->girq_pos));
	uint32_t cnt = sys_read32(base + XEC_RT_CNT_OFS);

	soc_ecia_girq_status_clear(xcfg->girq, xcfg->girq_pos);

	LOG_DBG("%p Counter ISR", dev);

	/* Was interrupt from an alarm? */
	if (data->alarm_cb != NULL) {
		soc_ecia_girq_ctrl(xcfg->girq, xcfg->girq_pos, 0);
		alarm_cb = xdat->alarm_cb;
		user_data = xdat->alarm_cb_ud;
		data->alarm_cb = NULL;
		data->alarm_cb_ud = NULL;
		alarm_cb(dev, 0, cnt, user_data);
	} else if (data->top_cb != NULL) {
		soc_ecia_girq_ctrl(xcfg->girq, xcfg->girq_pos, 1);
		xdat->top_cb(dev, data->top_cb_ud);
	}
}

static const struct counter_driver_api counter_xec_rt_api = {
		.start = counter_xec_rt_start,
		.stop = counter_xec_rt_stop,
		.get_value = counter_xec_rt_get_value,
		.set_alarm = counter_xec_rt_set_alarm,
		.cancel_alarm = counter_xec_rt_cancel_alarm,
		.set_top_value = counter_xec_rt_set_top_value,
		.get_pending_int = counter_xec_rt_get_pending_int,
		.get_top_value = counter_xec_rt_get_top_value,
		.get_freq = counter_xec_rt_get_freq,
};

static int counter_xec_rt_dev_init(const struct device *dev)
{
	const struct counter_xec_rt_devcfg *xcfg = dev->config;
	struct counter_xec_rt_dev_data *const xdat = dev->data;
	const struct counter_config_info *info = &xcfg->info;
	mm_reg_t base = xcfg->base;
	uint32_t rtmr_cfg = BIT(XEC_RT_CR_ACTV_POS);
	int ret = 0;

	if (IS_ENABLED(CONFIG_SOC_MEC_DEBUG_AND_TRACING)) {
		rtmr_cfg |= BIT(XEC_RT_CR_EXT_HALT_POS);
	}

	xdat->top_ticks = info->max_top_value;

	if ((info->flags & COUNTER_CONFIG_INFO_COUNT_UP) != 0) {
		LOG_ERR("Count up not supported");
		return -ENOTSUP;
	}

	sys_write32(0, base + XEC_RT_CR_OFS);
	soc_ecia_girq_status_clear(xcfg->girq, xcfg->girq_pos);

	sys_write32(info->max_top_value, base + XEC_RT_PLD_OFS);
	sys_write32(rtmr_cfg, base + XEC_RT_CR_OFS);

	if (xcfg->irq_cfg_func) {
		xcfg->irq_cfg_func();
		soc_ecia_girq_ctrl(xcfg->girq, xcfg->girq_pos, 1U);
	}

	return 0;
}

#define XEC_RT_GIRQ_ENC(inst) DT_INST_PROP_BY_IDX(inst, girq, 0)
#define XEC_RT_GIRQ(inst)     MCHP_XEC_ECIA_GIRQ(XEC_RT_GIRQ_ENC(inst))
#define XEC_RT_GIRQ_POS(inst) MCHP_XEC_ECIA_GIRQ_POS(XEC_RT_GIRQ_ENC(inst))

#define COUNTER_XEC_RT_INIT(inst)						\
	static void counter_xec_rt_irq_config_##inst(void)			\
	{									\
		IRQ_CONNECT(DT_INST_IRQN(inst),					\
			    DT_INST_IRQ(inst, priority),			\
			    counter_xec_rt_isr,					\
			    DEVICE_DT_INST_GET(inst), 0);			\
		irq_enable(DT_INST_IRQN(inst));					\
	}
										\
	static struct counter_xec_rt_dev_data counter_xec_rt_xdat_##inst;	\
										\
	static struct counter_xec_rt_devcfg counter_xec_rt_xcfg_##inst = {	\
		.info = {							\
			.max_top_value = DT_INST_PROP(inst, max_value),		\
			.freq = MEC_RTIMER_MAIN_CLK_FREQ,			\
			.flags = 0,						\
			.channels = 1,						\
		},								\
		.base = (mm_reg_t)DT_INST_REG_ADDR(inst),			\
		.irq_cfg_func = counter_xec_rt_irq_config_##inst,		\
		.girq = XEC_RT_GIRQ(inst),					\
		.girq_pos = XEC_RT_GIRQ_POS(inst),				\
	};									\
										\
	DEVICE_DT_INST_DEFINE(inst,						\
			      counter_xec_rt_dev_init,				\
			      NULL,						\
			      &counter_xec_rt_xdat_##inst,			\
			      &counter_xec_rt_xcfg_##inst,			\
			      POST_KERNEL,					\
			      CONFIG_COUNTER_INIT_PRIORITY,			\
			      &counter_xec_rt_api);

DT_INST_FOREACH_STATUS_OKAY(COUNTER_XEC_RT_INIT)
