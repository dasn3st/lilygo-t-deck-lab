// -----------------------------------------------------------------------------
// T-Deck launcher: a wake-up alarm (firmware side).
//
// jejeronimo, t-ui#9 "Wake up app": "Yes, it is an alarm clock to wake me up in the morning."
// Jake's call on where it goes: "#9 we can add to the timer app and rename it clock."
//
// ⭐ WHY THIS IS FIRMWARE-SIDE AND NOT IN THE APP. The obvious place is StopwatchApp.cpp, next
// to the countdown timer it sits beside - and that would be wrong. An alarm that only runs
// while its own screen is open is not an alarm; the entire point is that it fires at 6am with
// the device face-down, locked and dark. So the clock lives here, is checked from loop(), and
// does not care what is on screen or whether anything is on screen at all. The app is only a
// way to set it.
//
// THREADING: the same deferred pattern as TDeckClockFormat.cpp and TDeckUnits.cpp. The UI runs
// on its own FreeRTOS task and writing settings to flash from there is what froze the device
// when the time-zone dropdown first tried it (see TDeckTimeZone.cpp). The UI-facing setter
// records intent; tdeck_alarm_service() does the write on the safe thread.
//
// TIME: localtime() is correct here because TDeckTimeZone.cpp calls tzset() at boot, and the
// epoch comes from getValidTime() - the same source the message log and the lock screen use.
// With no fix and no RTC there is no valid time, and an alarm that does not know what time it
// is must stay silent rather than guess.
// -----------------------------------------------------------------------------
#include "main.h"
#include "gps/RTC.h" // getValidTime + RTCQuality
#include "mesh/NodeDB.h"
#include <Preferences.h>
#include <time.h>

// Waking the screen. Declared in the UI layer (TFTView_320x240.cpp); the UI poll timer
// consumes it, exactly as it does for a keypress on a dark screen.
extern volatile bool tdeck_wake_request;

// The buzzer, via the firmware's own helpers (buzz.cpp) - the same ones the countdown timer
// uses, so the alarm sounds like the rest of the device.
void playLongBeep();
extern "C" void tdeck_beep_gain(float g);
extern "C" void tdeck_beep_restore_gain(void);

static int s_hour = 7;
static int s_min = 0;
static bool s_on = false;
static bool s_loaded = false;
static volatile bool s_pendingSave = false;

// Ringing state. Kept apart from the setting: the alarm going off is not a preference.
static bool s_ringing = false;
static uint32_t s_ringStartMs = 0;
static uint32_t s_lastBeepMs = 0;
// Day-of-year this alarm last rang. ⭐ PERSISTED, not just in RAM: with the catch-up window
// below, a crash a few minutes after you dismissed the alarm would otherwise set it off again
// on the way back up. Dismissing has to be final for that day.
static int s_firedYday = -1;
static volatile bool s_pendingFiredSave = false;

// Ring for at most this long if nobody dismisses it. Long enough to wake someone, short enough
// that a forgotten alarm in a bag does not flatten the battery.
static const uint32_t kRingMaxMs = 120000;
static const uint32_t kBeepGapMs = 2500;

// How late the alarm may still ring. Covers a reboot spanning the alarm AND the time it takes
// a device with no RTC to learn the clock again after one.
static const int kCatchUpMins = 15;

static void load()
{
    if (s_loaded)
        return;
    s_loaded = true;
    Preferences p;
    if (p.begin("tdeckalarm", true)) {
        s_hour = (int)p.getUChar("h", 7);
        s_min = (int)p.getUChar("m", 0);
        s_on = p.getBool("on", false);
        s_firedYday = (int)p.getShort("fired", -1);
        p.end();
    }
    if (s_hour < 0 || s_hour > 23)
        s_hour = 7;
    if (s_min < 0 || s_min > 59)
        s_min = 0;
}

extern "C" void tdeck_alarm_get(int *h, int *m, bool *on)
{
    load();
    if (h)
        *h = s_hour;
    if (m)
        *m = s_min;
    if (on)
        *on = s_on;
}

// Called from the UI (LVGL) task - records intent only.
extern "C" void tdeck_alarm_set(int h, int m, bool on)
{
    load();
    s_hour = (h < 0 || h > 23) ? 0 : h;
    s_min = (m < 0 || m > 59) ? 0 : m;
    s_on = on;
    // Arming or re-arming clears "already rang today": setting the alarm to a time that has
    // just passed would otherwise leave it silently disabled until tomorrow.
    s_firedYday = -1;
    s_pendingSave = true;
    s_pendingFiredSave = true;
}

extern "C" bool tdeck_alarm_ringing(void)
{
    return s_ringing;
}

extern "C" void tdeck_alarm_dismiss(void)
{
    if (!s_ringing)
        return;
    s_ringing = false;
    tdeck_beep_restore_gain(); // restore the user's saved speaker volume
    LOG_INFO("[ALARM] dismissed");
}

// Called from the main Meshtastic loop(). Cheap when idle.
extern "C" void tdeck_alarm_service(void)
{
    load();

    if (s_pendingFiredSave && !s_pendingSave) {
        // Record "rang today" on its own, without rewriting the whole setting.
        s_pendingFiredSave = false;
        Preferences p;
        if (p.begin("tdeckalarm", false)) {
            p.putShort("fired", (int16_t)s_firedYday);
            p.end();
        }
    }
    if (s_pendingSave) {
        s_pendingSave = false;
        s_pendingFiredSave = false;
        Preferences p;
        if (p.begin("tdeckalarm", false)) {
            p.putUChar("h", (uint8_t)s_hour);
            p.putUChar("m", (uint8_t)s_min);
            p.putBool("on", s_on);
            p.putShort("fired", (int16_t)s_firedYday);
            p.end();
        }
        LOG_INFO("[ALARM] set %02d:%02d %s", s_hour, s_min, s_on ? "on" : "off");
    }

    const uint32_t now = millis();

    if (s_ringing) {
        // Stop on its own eventually: a forgotten alarm must not beep until the battery dies.
        if (now - s_ringStartMs > kRingMaxMs) {
            tdeck_alarm_dismiss();
            return;
        }
        if (now - s_lastBeepMs >= kBeepGapMs) {
            s_lastBeepMs = now;
            tdeck_beep_gain(1.0f); // as loud as the countdown timer goes
            playLongBeep();
            tdeck_wake_request = true; // keep the screen up while it rings
        }
        return;
    }

    if (!s_on)
        return;

    // Checked once a second. The window test below does the deciding; s_firedYday is what
    // stops it ringing twice for the same day.
    static uint32_t lastCheck = 0;
    if (now - lastCheck < 1000)
        return;
    lastCheck = now;

    const uint32_t epoch = getValidTime(RTCQuality::RTCQualityDevice, true);
    if (!epoch)
        return; // ⛔ no fix, no RTC: an alarm that does not know the time stays silent

    const time_t t = (time_t)epoch;
    struct tm lt;
    localtime_r(&t, &lt); // tzset() was done at boot by TDeckTimeZone.cpp

    // ⭐ A WINDOW, NOT AN INSTANT. Jake: "if the device for whatever reason crashes and then
    // reboots. It needs to hold that alarm still." Firing only during the exact minute meant a
    // crash at 06:59 that took 40 s to come back landed at 07:00:40 and never rang at all.
    //
    // It also covers the other half of a reboot: this device has NO RTC, so after a restart it
    // does not know the time until GPS or the mesh supplies one (~45 s from a warm start). A
    // one-minute window would expire before the clock could even be read.
    //
    // Bounded at 15 minutes so an alarm missed by hours - device off overnight - does not go
    // off at lunchtime.
    const int nowMins = lt.tm_hour * 60 + lt.tm_min;
    const int setMins = s_hour * 60 + s_min;
    int since = nowMins - setMins;
    if (since < 0)
        since += 24 * 60; // just after midnight, for an alarm set late yesterday
    if (since > kCatchUpMins)
        return; // not due, or missed by too much to be welcome

    if (s_firedYday == lt.tm_yday)
        return; // already rang today, and that survives a reboot

    s_firedYday = lt.tm_yday;
    s_pendingFiredSave = true; // write it NOW, so a crash cannot un-dismiss the alarm
    s_ringing = true;
    s_ringStartMs = now;
    s_lastBeepMs = now - kBeepGapMs; // beep immediately rather than after the first gap
    tdeck_wake_request = true;
    LOG_INFO("[ALARM] ringing at %02d:%02d", lt.tm_hour, lt.tm_min);
}
