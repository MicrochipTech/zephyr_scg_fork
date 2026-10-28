/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "power_mgmt.h"

LOG_MODULE_DECLARE(mec_assy6941_pm, LOG_LEVEL_INF);

/* suspend-to-idle and suspend-to-ram */
#define SLP_STATES_SUPPORTED 2U

#define TASK_STACK_SIZE 1024U
#define PRIORITY        K_PRIO_COOP(5)

/* Thread periods, both below the suspend-to-idle residency so the threads keep
 * the system out of a low-power state while they run.
 */
#define THREAD_A_SLEEP_TIME 100U
#define THREAD_B_SLEEP_TIME 1000U

/*
 * Entry latency budget, in milliseconds.
 *
 * Latency here is measured from the application's trigger marker to the PM
 * core entering the state, so it includes however long the system stays busy
 * first. The logging thread runs every CONFIG_LOG_PROCESS_THREAD_SLEEP_MS and
 * each of its wakeups defers entry, so the budget has to clear that period or
 * it flags ordinary behaviour. One period plus a margin for the work itself.
 */
#define MAX_EXPECTED_MS_LATENCY (CONFIG_LOG_PROCESS_THREAD_SLEEP_MS + 500)

/* Slept past each state's minimum residency so the policy selects it. */
#define DP_EXTRA_SLP_TIME 1100U
#define LT_EXTRA_SLP_TIME 500U

/*
 * Uptime tolerance per sleep, in milliseconds.
 *
 * Across a low-power state the kernel timer recovers elapsed time from the
 * always-on 32 KHz companion, whose resolution is 30.5 us, and a deep sleep
 * adds up to 3 ms of PLL relock. k_msleep() also rounds up to a tick. 20 ms
 * covers all of that with room to spare while still catching a companion that
 * mismeasures an interval, which is the failure this is here to find.
 */
#define MAX_UPTIME_ERROR_MS 20

#define SEC_TO_MSEC 1000U

K_THREAD_STACK_DEFINE(stack_a, TASK_STACK_SIZE);
K_THREAD_STACK_DEFINE(stack_b, TASK_STACK_SIZE);

static struct k_thread thread_a_id;
static struct k_thread thread_b_id;

/*
 * Pause handshake for the worker threads.
 *
 * k_thread_suspend() does not cancel a pending timeout: halt_thread() aborts
 * one only for _THREAD_DEAD (kernel/sched.c). A thread suspended part-way
 * through k_msleep() keeps that timeout live, so the kernel wakes to service
 * it and the sleep under test ends early or is never entered at the depth
 * requested. That is what limited this sample to 3 of 5 light and 2 of 5 deep
 * sleeps.
 *
 * Instead each worker parks itself on a semaphore with K_FOREVER, which
 * registers no timeout at all (z_add_timeout() returns immediately for
 * K_FOREVER), and the caller waits on a second semaphore until both have
 * parked. k_wakeup() cuts short a worker that is mid-k_msleep(), and unlike
 * suspend it does abort the timeout.
 */
static bool run_threads;
static struct k_sem parked_sem;
static struct k_sem resume_sem;

struct pm_counter {
	uint8_t entry_cnt;
	uint8_t exit_cnt;
};

static int64_t trigger_time;
static bool checks_enabled;
static struct pm_counter pm_counters[SLP_STATES_SUPPORTED];

/* Worst uptime error seen over all sleeps, in milliseconds. */
static int64_t worst_uptime_error_ms;

static const struct pm_state_info residency_info[] =
	PM_STATE_INFO_LIST_FROM_DT_CPU(DT_NODELABEL(cpu0));
static const size_t residency_info_len = DT_NUM_CPU_POWER_STATES(DT_NODELABEL(cpu0));

BUILD_ASSERT(ARRAY_SIZE(residency_info) >= 2,
	     "the board must define a light and a deep power state");

/*
 * Optional entry/exit markers.
 *
 * The MEC15xx version of this sample wrote GPIO control registers directly,
 * which does not build on MEC5. Here the pins are devicetree-supplied and
 * entirely optional: define pm-markers in an overlay to drive them, otherwise
 * the sample runs without.
 */
#define PM_MARKERS_NODE DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(PM_MARKERS_NODE, pm_gpios)
#define PM_MARKER_COUNT DT_PROP_LEN(PM_MARKERS_NODE, pm_gpios)
static const struct gpio_dt_spec pm_markers[] = {
	DT_FOREACH_PROP_ELEM_SEP(PM_MARKERS_NODE, pm_gpios, GPIO_DT_SPEC_GET_BY_IDX, (,))};
#else
#define PM_MARKER_COUNT 0
static const struct gpio_dt_spec pm_markers[] = {};
#endif

/* Marker indices: light sleep, deep sleep, application trigger. */
#define PM_MARKER_LIGHT   0
#define PM_MARKER_DEEP    1
#define PM_MARKER_TRIGGER 2

static void pm_marker_init(void)
{
	for (size_t i = 0; i < PM_MARKER_COUNT; i++) {
		int ret = gpio_pin_configure_dt(&pm_markers[i], GPIO_OUTPUT_INACTIVE);

		if (ret != 0) {
			LOG_WRN("Marker %u unavailable: %d", (unsigned int)i, ret);
		}
	}
}

/* Markers are diagnostics, so a failed write is not worth failing the test
 * over and the return value is deliberately discarded.
 */
static void pm_marker_set(uint8_t idx, bool active)
{
	if (idx < PM_MARKER_COUNT) {
		(void)gpio_pin_set_dt(&pm_markers[idx], active ? 1 : 0);
	}
}

static void pm_latency_check(void)
{
	int64_t latency = k_uptime_delta(&trigger_time);

	LOG_INF("PM sleep entry latency %d.%03d seconds", (int)(latency / SEC_TO_MSEC),
		(int)(latency % SEC_TO_MSEC));

	if (latency > (int64_t)MAX_EXPECTED_MS_LATENCY) {
		LOG_WRN("Sleep entry latency is higher than expected (budget %d ms)",
			(int)MAX_EXPECTED_MS_LATENCY);
	}
}

static void notify_pm_state_entry(enum pm_state state)
{
	if (!checks_enabled) {
		return;
	}

	switch (state) {
	case PM_STATE_SUSPEND_TO_IDLE:
		pm_marker_set(PM_MARKER_LIGHT, true);
		pm_counters[0].entry_cnt++;
		break;
	case PM_STATE_SUSPEND_TO_RAM:
		pm_marker_set(PM_MARKER_DEEP, true);
		pm_counters[1].entry_cnt++;
		pm_latency_check();
		break;
	default:
		break;
	}
}

static void notify_pm_state_exit(enum pm_state state)
{
	if (!checks_enabled) {
		return;
	}

	switch (state) {
	case PM_STATE_SUSPEND_TO_IDLE:
		pm_marker_set(PM_MARKER_LIGHT, false);
		pm_counters[0].exit_cnt++;
		break;
	case PM_STATE_SUSPEND_TO_RAM:
		pm_marker_set(PM_MARKER_DEEP, false);
		pm_counters[1].exit_cnt++;
		break;
	default:
		break;
	}
}

static int pm_check_counters(uint8_t cycles)
{
	int ret = 0;

	for (int i = 0; i < SLP_STATES_SUPPORTED; i++) {
		LOG_INF("PM state[%d] entry counter %d", i, pm_counters[i].entry_cnt);
		LOG_INF("PM state[%d] exit counter %d", i, pm_counters[i].exit_cnt);

		if (pm_counters[i].entry_cnt != pm_counters[i].exit_cnt) {
			LOG_ERR("PM state[%d] entry/exit mismatch", i);
			ret = -EIO;
		}

		if (pm_counters[i].entry_cnt != cycles) {
			LOG_ERR("PM state[%d] counter %d, expected %d", i,
				pm_counters[i].entry_cnt, cycles);
			ret = -EIO;
		}

		pm_counters[i].entry_cnt = 0;
		pm_counters[i].exit_cnt = 0;
	}

	return ret;
}

static void pm_reset_counters(void)
{
	for (int i = 0; i < SLP_STATES_SUPPORTED; i++) {
		pm_counters[i].entry_cnt = 0;
		pm_counters[i].exit_cnt = 0;
	}

	checks_enabled = false;
	pm_marker_set(PM_MARKER_TRIGGER, false);
}

static void pm_trigger_marker(void)
{
	trigger_time = k_uptime_get();
	pm_marker_set(PM_MARKER_TRIGGER, true);
	printk("PM >\n");
}

static void pm_exit_marker(void)
{
	pm_marker_set(PM_MARKER_TRIGGER, false);
	printk("PM <\n");

	if (trigger_time > 0) {
		int64_t residency_delta = k_uptime_delta(&trigger_time);

		LOG_INF("PM sleep residency %d.%03d seconds",
			(int)(residency_delta / SEC_TO_MSEC),
			(int)(residency_delta % SEC_TO_MSEC));
	}
}

/*
 * Sleep for @p sleep_ms and record how far uptime advanced against it.
 *
 * The kernel timer cannot measure a low-power state itself; it recovers the
 * interval from the always-on companion. An error here means that recovery is
 * wrong, which is invisible to the entry/exit counters.
 */
static void pm_sleep_and_measure(uint32_t sleep_ms, const char *what)
{
	int64_t before = k_uptime_get();
	int64_t slept;
	int64_t error;

	k_msleep(sleep_ms);

	slept = k_uptime_get() - before;
	error = slept - (int64_t)sleep_ms;
	if (error < 0) {
		error = -error;
	}

	if (error > worst_uptime_error_ms) {
		worst_uptime_error_ms = error;
	}

	if (error > MAX_UPTIME_ERROR_MS) {
		LOG_ERR("%s: slept %lld ms, uptime advanced %lld ms (error %lld ms)", what,
			(long long)sleep_ms, (long long)slept, (long long)error);
	} else {
		LOG_DBG("%s: uptime error %lld ms", what, (long long)error);
	}

	/* Exercises the cycle counter, which k_busy_wait() spins on. */
	k_busy_wait(100);
}

int test_pwr_mgmt_check_uptime(void)
{
	LOG_INF("Worst uptime error %lld ms (tolerance %d ms)", (long long)worst_uptime_error_ms,
		MAX_UPTIME_ERROR_MS);

	if (worst_uptime_error_ms > MAX_UPTIME_ERROR_MS) {
		LOG_ERR("Uptime did not track wall time across sleep");
		return -EIO;
	}

	return 0;
}

static void task_thread(uint32_t period_ms, char marker)
{
	while (true) {
		if (!run_threads) {
			/* Park holding no timeout: K_FOREVER adds none, so
			 * nothing of this thread's can wake the system while
			 * the sleep under test runs.
			 */
			k_sem_give(&parked_sem);
			(void)k_sem_take(&resume_sem, K_FOREVER);
			continue;
		}

		k_msleep(period_ms);

		if (run_threads) {
			printk("%c", marker);
		}
	}
}

static void task_a_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	task_thread(THREAD_A_SLEEP_TIME, 'A');
}

static void task_b_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	task_thread(THREAD_B_SLEEP_TIME, 'B');
}

static void create_tasks(void)
{
	k_sem_init(&parked_sem, 0, 2);
	k_sem_init(&resume_sem, 0, 2);
	run_threads = true;

	k_thread_create(&thread_a_id, stack_a, TASK_STACK_SIZE, task_a_thread, NULL, NULL, NULL,
			PRIORITY, K_INHERIT_PERMS, K_FOREVER);
	k_thread_create(&thread_b_id, stack_b, TASK_STACK_SIZE, task_b_thread, NULL, NULL, NULL,
			PRIORITY, K_INHERIT_PERMS, K_FOREVER);

	k_thread_start(&thread_a_id);
	k_thread_start(&thread_b_id);
}

static void destroy_tasks(void)
{
	run_threads = false;

	/* Release anything parked so the aborts below are not racing a pend. */
	k_sem_give(&resume_sem);
	k_sem_give(&resume_sem);

	k_thread_abort(&thread_a_id);
	k_thread_abort(&thread_b_id);

	k_thread_join(&thread_a_id, K_FOREVER);
	k_thread_join(&thread_b_id, K_FOREVER);
}

static void park_all_tasks(void)
{
	run_threads = false;

	k_wakeup(&thread_a_id);
	k_wakeup(&thread_b_id);

	/* Both threads park themselves; wait for them so no timeout outlives
	 * this call.
	 */
	k_sem_take(&parked_sem, K_FOREVER);
	k_sem_take(&parked_sem, K_FOREVER);
}

static void unpark_all_tasks(void)
{
	run_threads = true;

	k_sem_give(&resume_sem);
	k_sem_give(&resume_sem);
}

static struct pm_notifier notifier = {
	.state_entry = notify_pm_state_entry,
	.state_exit = notify_pm_state_exit,
};

static uint32_t light_sleep_ms(void)
{
	return (residency_info[0].min_residency_us / 1000U) + LT_EXTRA_SLP_TIME;
}

static uint32_t deep_sleep_ms(void)
{
	return (residency_info[residency_info_len - 1].min_residency_us / 1000U) +
	       DP_EXTRA_SLP_TIME;
}

static void report_wake(bool use_logging, const char *msg)
{
	if (use_logging) {
		LOG_INF("%s", msg);
	} else {
		printk("%s\n", msg);
	}
}

int test_pwr_mgmt_multithread(bool use_logging, uint8_t cycles)
{
	uint8_t iterations = cycles;

	/* Deep sleep is only reachable with the threads suspended: a running
	 * thread wakes the system before the deep residency is met. Covers
	 * zephyrproject-rtos/zephyr#20033.
	 */
	pm_notifier_register(&notifier);
	create_tasks();

	LOG_WRN("PM multi-thread test started for cycles: %d, logging: %d", cycles, use_logging);

	checks_enabled = true;
	while (iterations-- > 0) {
		LOG_INF("Park threads...");
		park_all_tasks();
		LOG_INF("About to enter light sleep");
		pm_sleep_and_measure(light_sleep_ms(), "light sleep");
		report_wake(use_logging, "Wake from Light Sleep");

		LOG_INF("Resume");
		unpark_all_tasks();

		LOG_INF("Park threads...");
		park_all_tasks();
		LOG_INF("About to enter deep sleep");

		pm_trigger_marker();
		pm_sleep_and_measure(deep_sleep_ms(), "deep sleep");
		report_wake(use_logging, "Wake from Deep Sleep");

		pm_exit_marker();
		LOG_INF("Resume");
		unpark_all_tasks();
	}

	destroy_tasks();

	LOG_INF("PM multi-thread completed");

	int ret = pm_check_counters(cycles);

	pm_reset_counters();
	pm_notifier_unregister(&notifier);

	return ret;
}

int test_pwr_mgmt_singlethread(bool use_logging, uint8_t cycles)
{
	uint8_t iterations = cycles;

	LOG_WRN("PM single-thread test started for cycles: %d, logging: %d", cycles, use_logging);

	pm_marker_init();
	pm_notifier_register(&notifier);
	checks_enabled = true;
	while (iterations-- > 0) {
		/* Light sleep: the 48 MHz PLL stays on. */
		LOG_INF("About to enter light sleep");
		pm_sleep_and_measure(light_sleep_ms(), "light sleep");
		report_wake(use_logging, "Wake from Light Sleep");

		/* Deep sleep: the 48 MHz PLL is gated. */
		LOG_INF("About to enter deep sleep");

		pm_trigger_marker();
		pm_sleep_and_measure(deep_sleep_ms(), "deep sleep");
		report_wake(use_logging, "Wake from Deep Sleep");

		pm_exit_marker();
	}

	LOG_INF("PM single-thread completed");

	int ret = pm_check_counters(cycles);

	pm_reset_counters();
	pm_notifier_unregister(&notifier);

	return ret;
}
