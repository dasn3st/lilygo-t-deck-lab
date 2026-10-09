// -----------------------------------------------------------------------------
// T-Deck launcher: GPS on/off control bridge (firmware side)
//
// Companion to TDeckGpsBridge.cpp (which only *reads* GPS status). This one lets
// the device-ui settings screen turn the GPS on or off and — when on — keep it
// searching continuously, applied LIVE with no reboot.
//
// Why a bridge + deferred service (same story as TDeckMeshSwitch.cpp): the LVGL
// UI runs on its own FreeRTOS task and must not drive the GPS driver directly (it
// sends UART commands and shares state with the GPS OSThread). So the UI-facing
// setter only records intent; tdeck_gps_control_service(), called from the main
// loop(), applies it on the same cooperative thread the GPS OSThread and
// AdminModule use — exactly where gps->enable()/disable() are already known-safe.
//
// "Continuous" = a gps_update_interval of 10s. At or below
// GPS_UPDATE_ALWAYS_ON_THRESHOLD_MS the GPS driver never powers the chip down
// between fixes (see GPS::down()), so it is always looking for satellites. The
// stock default is 120s, which is why the GPS otherwise sleeps in ~2-minute
// stretches and appears to "not look" until something happens to catch it awake.
// Longer intervals nap the chip between checks and save battery.
// -----------------------------------------------------------------------------
#if !MESHTASTIC_EXCLUDE_GPS
#include "DebugConfiguration.h" // LOG_INFO
#include "gps/GPS.h"
#include "mesh/NodeDB.h"

// How often the GPS looks for a fix when it's "on". At or below the driver's always-on
// threshold the chip never powers down between fixes, so it is continuously searching.
#define TDECK_GPS_CONTINUOUS_SECS 10

static volatile bool s_tdeckGpsPending = false;          // an on/off change is waiting
static volatile bool s_tdeckGpsWantOn = true;            // desired state (written by the UI task)
static volatile uint32_t s_tdeckGpsPendingInterval = 0;  // desired check interval, seconds (0 = no change)
static volatile bool s_tdeckGpsKick = false;             // re-arm the search (wake); no config write
static bool s_tdeckGpsBootKicked = false;                // boot re-arm done once the driver is up

// Re-arm the GPS search. Called on wake, because nothing else does.
//
// Observed on-device (2026-07-18): from a cold boot the GPS could sit for 25+ minutes,
// including driving, without a lock - then flipping the Settings switch off and on gave
// satellites almost immediately. After the device slept it died again and only the toggle
// revived it. Both gaps are the same root cause: the ONLY thing that ever called
// gps->enable() with the continuous interval was that switch. At boot the stored (possibly
// long) interval left the chip napping between searches, and after a sleep nothing re-armed
// it at all. This does automatically what Jake was doing by hand.
extern "C" void tdeck_gps_kick(void)
{
    s_tdeckGpsKick = true;
}

// Called from the UI (LVGL) task. Only records intent — never touches the GPS here.
extern "C" void tdeck_gps_set_enabled(bool on)
{
    s_tdeckGpsWantOn = on;
    s_tdeckGpsPending = true;
}

// How often the GPS looks for a fix, in seconds. <= 10 keeps the chip always on
// (continuous); longer intervals let it nap between checks to save battery.
extern "C" void tdeck_gps_set_interval(uint32_t secs)
{
    if (secs)
        s_tdeckGpsPendingInterval = secs;
}

// Live truth: is the GPS currently configured on?
extern "C" bool tdeck_gps_get_enabled(void)
{
    return config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED;
}

// Called from the main Meshtastic loop() — the safe GPS context. Applies pending
// requests; retries next loop if the GPS driver isn't up yet. Cheap when idle.
extern "C" void tdeck_gps_control_service(void)
{
    if (!s_tdeckGpsPending && !s_tdeckGpsPendingInterval && !s_tdeckGpsKick && s_tdeckGpsBootKicked)
        return;

    if (!gps)
        return; // GPS driver not ready yet — keep the request pending, retry next loop

    // Once, as soon as the driver exists: if the GPS is meant to be on, put it into
    // continuous search. Without this a stored long interval means the chip naps between
    // fixes from boot, which reads as "the GPS just never locks".
    if (!s_tdeckGpsBootKicked) {
        s_tdeckGpsBootKicked = true;
        if (config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED) {
            config.position.gps_update_interval = TDECK_GPS_CONTINUOUS_SECS;
            gps->enable();
        }
    }

    // Wake re-arm. Deliberately does NOT write config to flash — this can happen often, and
    // the stored settings aren't changing, only the driver's scheduling.
    if (s_tdeckGpsKick) {
        s_tdeckGpsKick = false;
        if (config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED) {
            config.position.gps_update_interval = TDECK_GPS_CONTINUOUS_SECS;
            gps->enable();
        }
    }

    // Watchdog. The wake re-arm above only fires on the trackball double-click / keyboard
    // wake path, so waking any other way (or the power state machine dropping the device to
    // DARK on battery and coming back) could still leave the receiver asleep with no way
    // back except toggling the Settings switch by hand.
    //
    // ACTIVE and IDLE are both fine — IDLE is the normal resting state between fixes when
    // continuous search is on. Only the genuinely powered-down states are wrong here, so
    // this can't disturb a search that's legitimately in progress.
    static uint32_t lastWatchdogMs = 0;
    uint32_t nowMs = millis();
    if (nowMs - lastWatchdogMs > 20000) {
        lastWatchdogMs = nowMs;
        if (config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED) {
            GPSPowerState ps = gps->getPowerState();
            if (ps == GPS_SOFTSLEEP || ps == GPS_HARDSLEEP || ps == GPS_OFF) {
                LOG_INFO("T-Deck GPS watchdog: receiver asleep (state %d) but GPS is on — re-arming", (int)ps);
                config.position.gps_update_interval = TDECK_GPS_CONTINUOUS_SECS;
                gps->enable();
            }
        }
    }

    if (!s_tdeckGpsPending && !s_tdeckGpsPendingInterval)
        return;

    // Apply a new check interval first so an accompanying "on" enables with it.
    uint32_t interval = s_tdeckGpsPendingInterval;
    if (interval) {
        config.position.gps_update_interval = interval;
        s_tdeckGpsPendingInterval = 0;
        // Already on and staying on? Re-enable so the driver reschedules with the
        // new interval now instead of after its current (possibly long) nap.
        if (!s_tdeckGpsPending && config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED)
            gps->enable();
    }

    if (s_tdeckGpsPending) {
        if (s_tdeckGpsWantOn) {
            config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_ENABLED;
            gps->enable(); // resets scheduling and starts a search immediately
        } else {
            config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_DISABLED;
            gps->disable();
        }
        s_tdeckGpsPending = false;
    }

    // Persist so the choice survives a reboot. Unlike the admin config path, this
    // does NOT force a device restart.
    if (nodeDB)
        nodeDB->saveToDisk(SEGMENT_CONFIG);
}

#else // GPS excluded from this build — provide no-op stubs so callers still link.
extern "C" void tdeck_gps_set_enabled(bool) {}
extern "C" void tdeck_gps_set_interval(uint32_t) {}
extern "C" void tdeck_gps_kick(void) {}
extern "C" bool tdeck_gps_get_enabled(void)
{
    return false;
}
extern "C" void tdeck_gps_control_service(void) {}
#endif // !MESHTASTIC_EXCLUDE_GPS
