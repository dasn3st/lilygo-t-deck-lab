// -----------------------------------------------------------------------------
// T-Deck launcher: lock-screen on/off (firmware side).
//
// The launcher always boots to a PIN pad (a code is always in effect — default 1234
// until the user picks their own). Some people don't want a lock at all, so this adds
// a persistent on/off flag the Settings screen can flip.
//
// It lives in NVS — internal flash, no SPI — deliberately, NOT in the config that
// nodeDB writes over the shared filesystem. A flash write to that filesystem from the
// UI (LVGL) task is exactly what froze the device when the time-zone dropdown first
// tried it (see TDeckTimeZone.cpp). NVS via Preferences has its own lock and is
// task-safe, so the switch handler can write this directly with no deferred plumbing.
//
// Default is ON: a device nobody has touched keeps locking exactly as it did before.
// -----------------------------------------------------------------------------
#include <Preferences.h>

static int s_lockEnabled = -1; // -1 = not read from NVS yet; 0 = off; 1 = on

extern "C" int tdeck_lock_mode(void); // TDeckLockPrefs.cpp - 0 Off, 1 Swipe, 2 PIN

// Kept as the one-bit question "is there a lock at all", now answered by the three-way
// mode so there is a single source of truth. Everything that used to call this still
// works; Swipe counts as locked, because it is.
extern "C" bool tdeck_lock_enabled(void)
{
    return tdeck_lock_mode() != 0;
}

extern "C" void tdeck_lock_set_mode(int mode); // TDeckLockPrefs.cpp

// On -> PIN, off -> Off. Only here for anything still flipping a switch; the Settings
// page sets the mode directly.
extern "C" void tdeck_lock_set_enabled(bool en)
{
    tdeck_lock_set_mode(en ? 2 : 0);
}
