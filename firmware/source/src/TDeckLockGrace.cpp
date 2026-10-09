// -----------------------------------------------------------------------------
// TDeckLockGrace - how long after locking the device before the PIN is asked for again.
//
// Jake, 2026-09-18: "lock screen. setting for amount of time aftwe lock/sleep for it
// to require a pin unlock."
//
// The glance screen (slide to unlock) always appears. This only decides whether sliding
// takes you straight in or on to the keypad. Inside the window, the device has not left
// your hand; outside it, the code is the code.
//
// 0 = always ask, and that is the DEFAULT: a device nobody has touched behaves exactly
// as it did before this existed.
//
// Same reasoning as TDeckLockControl.cpp for where it lives: NVS via Preferences, not the
// nodeDB config on the shared filesystem. A filesystem write from the UI (LVGL) task is
// what froze the device when the time-zone dropdown first tried it. NVS has its own lock
// and is task-safe, so the Settings handler can write this directly.
// -----------------------------------------------------------------------------
#include <Preferences.h>

static int32_t s_graceSecs = -1; // -1 = not read from NVS yet

extern "C" uint32_t tdeck_lock_grace_secs(void)
{
    if (s_graceSecs < 0) {
        Preferences p;
        if (p.begin("tdecklock", true)) { // read-only; shares the namespace with the on/off flag
            s_graceSecs = (int32_t)p.getUInt("grace", 0);
            p.end();
        } else {
            s_graceSecs = 0; // namespace not created yet -> always ask
        }
    }
    return (uint32_t)s_graceSecs;
}

extern "C" void tdeck_lock_set_grace_secs(uint32_t secs)
{
    s_graceSecs = (int32_t)secs;
    Preferences p;
    if (p.begin("tdecklock", false)) {
        p.putUInt("grace", secs);
        p.end();
    }
}
