/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Private interface between the Microchip XEC RTOS timer kernel timer driver
 * and the MEC SoC power management code. Not public API.
 *
 * The driver borrows a 32-bit Basic Timer as a free-running 1 MHz counter for
 * arch_busy_wait() when CONFIG_ARCH_HAS_CUSTOM_BUSY_WAIT is enabled. Basic
 * Timers do not respect the chip's SLP_EN signals: a running one keeps its
 * CLK_REQ asserted, and the PCR will not gate the PLL while any CLK_REQ is
 * set. Deep sleep therefore has to stop that timer, which only the driver
 * knows how to address.
 *
 * Only deep sleep needs this. Light sleep leaves the PLL running, so a Basic
 * Timer holding CLK_REQ there costs nothing.
 */

#ifndef ZEPHYR_DRIVERS_TIMER_MCHP_XEC_RTOS_TIMER_H_
#define ZEPHYR_DRIVERS_TIMER_MCHP_XEC_RTOS_TIMER_H_

/**
 * @brief Stop the busy-wait Basic Timer before deep sleep entry.
 *
 * Call from the SoC deep-sleep path before asserting SLEEP_ALL, so the timer
 * drops its CLK_REQ and the PCR can gate the PLL. Pairs with
 * mchp_xec_rtos_timer_dsleep_restore().
 *
 * The SoC PM code carries weak no-op definitions of both. The driver
 * overrides them only when it is the kernel timer and
 * CONFIG_ARCH_HAS_CUSTOM_BUSY_WAIT gave it a Basic Timer to own; any other
 * configuration gets the no-op.
 */
void mchp_xec_rtos_timer_dsleep_save(void);

/**
 * @brief Restart the busy-wait Basic Timer after deep sleep exit.
 *
 * Call from the SoC deep-sleep path once the PLL has locked, before anything
 * that may call k_busy_wait(). Restores only what
 * mchp_xec_rtos_timer_dsleep_save() stopped; a timer that was already stopped
 * stays stopped.
 */
void mchp_xec_rtos_timer_dsleep_restore(void);

#endif /* ZEPHYR_DRIVERS_TIMER_MCHP_XEC_RTOS_TIMER_H_ */
