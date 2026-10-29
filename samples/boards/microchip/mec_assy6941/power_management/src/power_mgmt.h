/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __TEST_PWRMGMT_H__
#define __TEST_PWRMGMT_H__

#include <stdbool.h>
#include <stdint.h>

/** @brief Alternates between light and deep sleep cycles.
 *
 * Sleeps in the main thread for longer than each state's minimum residency so
 * the PM policy selects that state.
 *
 * @param use_logging report progress with logging rather than printk.
 * @param cycles number of light/deep sleep pairs to run.
 * @retval 0 on success.
 * @retval -EIO if a PM state was entered a different number of times than
 *         requested, or entry and exit counts disagree.
 */
int test_pwr_mgmt_singlethread(bool use_logging, uint8_t cycles);

/** @brief Alternates between light and deep sleep cycles with threads running.
 *
 * As test_pwr_mgmt_singlethread(), with two periodic threads created and
 * suspended around each sleep. Deep sleep is only reachable while they are
 * suspended, which is what makes this distinct: a running thread keeps the
 * system out of the deeper state.
 *
 * @param use_logging report progress with logging rather than printk.
 * @param cycles number of light/deep sleep pairs to run.
 * @retval 0 on success.
 * @retval -EIO if a PM state was entered a different number of times than
 *         requested, or entry and exit counts disagree.
 */
int test_pwr_mgmt_multithread(bool use_logging, uint8_t cycles);

/** @brief Reports whether uptime tracked wall time across the sleep cycles.
 *
 * The kernel timer hands timekeeping to an always-on companion across a
 * low-power state, so a sleep the companion mismeasures shows up as uptime
 * that advanced by the wrong amount. Each sleep records the error between the
 * requested sleep and the uptime delta; this prints the worst case.
 *
 * @retval 0 if the largest error is within tolerance.
 * @retval -EIO otherwise.
 */
int test_pwr_mgmt_check_uptime(void);

/** @brief Enters deep sleep with a counter alarm armed.
 *
 * The XEC counter driver's pm_action leaves a counter running when an alarm
 * or periodic top callback is armed, because stopping it would drop the event
 * the application asked for. A running Basic Timer holds its CLK_REQ, so the
 * PCR keeps the PLL on for the sleep: the cost of an armed alarm is power, not
 * a missed event. This exercises that branch, which no other phase reaches.
 *
 * Does nothing unless a counter device is named by
 * chosen { zephyr,counter }; see counter_alarm.overlay.
 *
 * @retval 0 if the alarm fired across the sleep, or no counter is configured.
 * @retval -ENODEV if the chosen counter is not ready.
 * @retval -EIO if the alarm did not fire.
 * @retval other errno from the counter API.
 */
int test_pwr_mgmt_counter_alarm(void);

#endif /* __TEST_PWRMGMT_H__ */
