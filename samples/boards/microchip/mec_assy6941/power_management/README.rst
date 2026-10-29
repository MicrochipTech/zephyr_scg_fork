.. zephyr:code-sample:: mec_assy6941_power_management
   :name: MEC ASSY6941 power management

   Enter light and deep sleep repeatedly and check the kernel timer tracked
   wall time across each one.

Overview
********

This sample exercises the Microchip MEC power management paths on
``mec_assy6941/mec1753_qlj``. It sleeps past each power state's minimum
residency so the PM policy selects that state, counts entries and exits
through a :c:struct:`pm_notifier`, and compares the uptime delta of every
sleep against the requested duration.

That last check is the point of the sample. The kernel timer cannot measure a
low-power state itself: the capture/compare block and the Basic Timers are
gated with the PLL, so timekeeping is handed to the always-on 32 KHz RTOS
timer and recovered on wake. An error in that recovery does not show up in the
entry/exit counters, only in uptime drifting away from wall time, so the
sample fails if any sleep is off by more than 20 ms.

Each run performs single-threaded and multi-threaded cycles. The
multi-threaded variant creates two periodic threads and parks them around each
sleep: deep sleep is only reachable while they are parked, since a running
thread wakes the system before the deep residency is met.

The threads park on a semaphore rather than being suspended.
:c:func:`k_thread_suspend` does not cancel a pending timeout, so a thread
suspended part-way through :c:func:`k_msleep` keeps it live and the kernel
wakes to service it, cutting the sleep under test short.

Build variants
**************

The two supported configurations reach deep sleep by different routes, and
both are worth testing.

``prj.conf`` (``CONFIG_PM=y``)
   Devices stay enabled across the low-power state. Peripherals that ignore
   the chip's ``SLP_EN`` signals would keep their clock request asserted and
   prevent the PCR from gating the PLL, so the MEC SoC layer saves, disables
   and restores the Basic Timers and the UARTs around deep sleep itself.

``pm-device.conf`` (``CONFIG_PM=y`` plus ``CONFIG_PM_DEVICE=y`` and
``CONFIG_PM_DEVICE_SYSTEM_MANAGED=y``)
   The PM subsystem suspends devices before entering the state and resumes
   them afterwards, so each driver's ``pm_action`` handles its own hardware.
   The console UART is suspended across the sleep, so console output pauses at
   the suspend and resumes on wake. This is expected, not a hang.

The sample logs which of the two it was built with on startup.

To switch the kernel timer from the RTOS timer to the capture/compare driver,
add ``-DEXTRA_DTC_OVERLAY_FILE=cct_ktimer.overlay``.

Counter alarm phase
===================

``counter_alarm.overlay`` enables a 32-bit Basic Timer as a counter device and
names it through ``chosen { zephyr,counter }``. With it, and with
``CONFIG_COUNTER=y``, the sample adds a final phase that arms a counter alarm
and then enters deep sleep with it pending:

.. code-block:: console

   west build -b mec_assy6941/mec1753_qlj -- \
       -DCONF_FILE=pm-device.conf \
       -DEXTRA_DTC_OVERLAY_FILE=counter_alarm.overlay \
       -DCONFIG_COUNTER=y

This covers the one branch of the XEC counter driver's ``pm_action`` that the
other phases never reach. A Basic Timer ignores the chip's ``SLP_EN``, so one
left running holds its ``CLK_REQ`` and the PCR will not gate the PLL. The
driver therefore stops a counter on suspend *except* when an alarm or a
periodic top callback is armed, where stopping it would drop the event the
application asked for. The sleep still happens and the CPU still enters the
low-power state; what is lost is the PLL being gated, so the part draws more
power. That is the intended trade, and it only applies while an application
has armed a counter event.

Watch two things on a scope during this phase:

* TEST_CLK_OUT (GPIO060) stays active through the sleep, where the other
  phases show it gated. That is the PLL being held up, and is expected here.
* With ``CONFIG_SOC_MEC_PM_DBG_PCR_CLK_REQ_VBM=y`` the saved ``CLK_REQ``
  registers should show this timer's bit set on sleep entry.

The phase runs once, at the end of a roughly 100 second run, so it is easy to
miss among the repeating pattern of the earlier phases. The board overlay
therefore drives GPIO004, which is pulled up on the EVB, low for the duration
of the phase: trigger on its falling edge to capture this phase alone.

Measured on MEC1753 with a 4000 ms alarm inside a 6000 ms sleep: the trigger
is low for 6.06 s, the PLL stays on for the first 4.04 s, and gates for the
remainder. The PLL holds up for exactly as long as the alarm is armed and
gates once it has fired, which is the behaviour described above.

Without the overlay the phase logs that it is skipping and returns success, so
the default build is unaffected. ``timer5`` is deliberately not used: the XEC
RTOS kernel timer borrows it for ``k_busy_wait()``.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/microchip/mec_assy6941/power_management
   :board: mec_assy6941/mec1753_qlj
   :goals: build flash
   :compact:

For the device power management variant, add
``-DCONF_FILE=pm-device.conf``.

Entry and exit markers
======================

The sample drives optional GPIO markers for light sleep, deep sleep and the
application trigger point, for measuring entry latency and residency with a
scope. They are off unless an overlay supplies them, so no jumper setup is
needed to run the sample:

.. code-block:: devicetree

   / {
           zephyr,user {
                   pm-gpios = <&gpio_000_036 10 GPIO_ACTIVE_LOW>,
                              <&gpio_000_036 11 GPIO_ACTIVE_LOW>,
                              <&gpio_000_036 12 GPIO_ACTIVE_LOW>;
           };
   };

The three entries are the light sleep, deep sleep and trigger markers, in that
order. Fewer may be given; the rest are skipped.

Sample output
=============

.. code-block:: console

   MEC ASSY6941 power management sample
   Device PM: disabled (SoC masks non-compliant peripherals)
   Kernel timer: 48000000 Hz cycle counter, 10000 Hz tick
   PM single-thread test started for cycles: 5, logging: 0
   About to enter light sleep
   Wake from Light Sleep
   About to enter deep sleep
   PM >
   PM <
   Wake from Deep Sleep
   PM state[0] entry counter 5
   PM state[0] exit counter 5
   PM state[1] entry counter 5
   PM state[1] exit counter 5
   Worst uptime error 2 ms (tolerance 20 ms)
   PM sample PASSED

The counters and timings shown will differ.
