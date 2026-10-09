// -----------------------------------------------------------------------------
// TDeckLockPrefs - everything the lock screen remembers.
//
// Jake, 2026-09-19: "can i have either or, pin or swipe? or seems like i have to have
// both. PRefferably. choice of none, pin, or swipe" and then "give the use an option on
// the lockscreen page in settings (hint hint for putting everything into a lockscreen
// page on the settings.) ... give the option of 'Lockscreen widget': Weather (sub menu
// of update period if selected), Satellites connected, sundown/sunrise."
//
// WHY A NEW FILE. The lock on/off flag already lived in TDeckLockControl.cpp as a plain
// bool, and the PIN grace period in TDeckLockGrace.cpp. Adding two more settings to two
// more files would have been three places to look. These are one subject, so they live
// in one file and share one NVS namespace.
//
// BACKWARD COMPATIBLE ON PURPOSE. The old bool is still read: a device that has only
// ever known "lock on/off" comes back as PIN (it had a PIN) or OFF, exactly as before.
// Nobody's device changes behaviour because the setting grew a third option.
//
// NVS rather than the nodeDB config, for the reason TDeckLockControl.cpp gives: a
// filesystem write from the UI (LVGL) task is what froze this device when the time-zone
// dropdown first tried it. NVS has its own lock and is safe to write from a handler.
// -----------------------------------------------------------------------------
#include <Arduino.h>   // millis()
#include <Preferences.h>

// Lock modes. Deliberately ordered least-secure to most, so the Settings button can
// just cycle upward and wrap.
//   0 = Off    - waking goes straight to Home
//   1 = Swipe  - the glance screen, and sliding unlocks. No code, ever.
//   2 = PIN    - the glance screen, and sliding leads to the keypad.
static int s_mode = -1; // -1 = not read from NVS yet

// Lock-screen widget: what fills the slot when there is nothing to notify about.
//   0 = None, 1 = Weather, 2 = Satellites, 3 = Sun
static int s_widget = -1;

// How often the weather widget may try to refresh itself, in minutes. 0 = never (just
// keep showing the last reading, which is what Jake asked for when there is no Wi-Fi).
static int s_wxMins = -1;

static void loadAll(void)
{
    if (s_mode >= 0)
        return;
    Preferences p;
    if (!p.begin("tdecklock", true)) { // namespace not created yet: first-run defaults
        s_mode = 2;                    // PIN, matching the old default of "lock on"
        s_widget = 3;                  // Sun: the one that earns its place on a hunting radio
        s_wxMins = 60;
        return;
    }
    if (p.isKey("mode")) {
        s_mode = (int)p.getUChar("mode", 2);
    } else {
        // Migrate the old on/off bool. "On" meant a PIN was required, so that is PIN.
        s_mode = p.getBool("en", true) ? 2 : 0;
    }
    s_widget = (int)p.getUChar("widget", 3);
    s_wxMins = (int)p.getUShort("wxmins", 60);
    p.end();
    if (s_mode < 0 || s_mode > 2)
        s_mode = 2;
    if (s_widget < 0 || s_widget > 3)
        s_widget = 3;
}

extern "C" int tdeck_lock_mode(void)
{
    loadAll();
    return s_mode;
}

extern "C" void tdeck_lock_set_mode(int mode)
{
    if (mode < 0 || mode > 2)
        return;
    loadAll();
    s_mode = mode;
    Preferences p;
    if (p.begin("tdecklock", false)) {
        p.putUChar("mode", (uint8_t)mode);
        // Keep the legacy flag honest for any code still reading it.
        p.putBool("en", mode != 0);
        p.end();
    }
}

extern "C" int tdeck_lock_widget(void)
{
    loadAll();
    return s_widget;
}

extern "C" void tdeck_lock_set_widget(int w)
{
    if (w < 0 || w > 3)
        return;
    loadAll();
    s_widget = w;
    Preferences p;
    if (p.begin("tdecklock", false)) {
        p.putUChar("widget", (uint8_t)w);
        p.end();
    }
}

extern "C" uint32_t tdeck_lock_wx_minutes(void)
{
    loadAll();
    return (uint32_t)s_wxMins;
}

extern "C" void tdeck_lock_set_wx_minutes(uint32_t mins)
{
    loadAll();
    s_wxMins = (int)mins;
    Preferences p;
    if (p.begin("tdecklock", false)) {
        p.putUShort("wxmins", (uint16_t)mins);
        p.end();
    }
}

// -----------------------------------------------------------------------------
// The last position this device ever had.
//
// The lock-screen sun and satellite widgets both need a latitude, and indoors there is no
// fix - which is most of the time a lock screen is actually looked at. "Sunset is at 7:42"
// is still the answer you wanted even when the satellites cannot see you through the roof,
// so remember the last real fix and say plainly that it is the last known one.
//
// Written at most once a minute: NVS is flash, and the widgets ask on every refresh.
// -----------------------------------------------------------------------------
static int32_t s_lastLat = 0, s_lastLon = 0;
static bool s_lastLoaded = false;
static uint32_t s_lastSaveMs = 0;

extern "C" bool tdeck_lastpos_get(int32_t *lat, int32_t *lon)
{
    if (!s_lastLoaded) {
        s_lastLoaded = true;
        Preferences p;
        if (p.begin("tdecklock", true)) {
            s_lastLat = (int32_t)p.getLong("plat", 0);
            s_lastLon = (int32_t)p.getLong("plon", 0);
            p.end();
        }
    }
    if (s_lastLat == 0 && s_lastLon == 0)
        return false; // never had one. Null Island is not a place anybody stands.
    if (lat)
        *lat = s_lastLat;
    if (lon)
        *lon = s_lastLon;
    return true;
}

extern "C" void tdeck_lastpos_set(int32_t lat, int32_t lon)
{
    if (lat == 0 && lon == 0)
        return;
    tdeck_lastpos_get(nullptr, nullptr); // make sure the load has happened before we overwrite
    const bool moved = (lat != s_lastLat || lon != s_lastLon);
    s_lastLat = lat;
    s_lastLon = lon;
    s_lastLoaded = true;
    if (!moved)
        return;
    const uint32_t now = millis();
    if (s_lastSaveMs && (now - s_lastSaveMs) < 60000)
        return; // in RAM immediately; to flash at most once a minute
    s_lastSaveMs = now ? now : 1;
    Preferences p;
    if (p.begin("tdecklock", false)) {
        p.putLong("plat", lat);
        p.putLong("plon", lon);
        p.end();
    }
}

// ---- keeping the lock screen lit --------------------------------------------
// How long the glance stays visible after locking, in minutes. 0 = off (goes dark as it
// always has), 255 = always. The whole cost of this is backlight, which is why the dim level
// below matters far more than any of the rest of it.
static int s_stayOn = -1;
// How bright, 0-100%. Default 15: readable indoors, a small fraction of full power.
static int s_dimPct = -1;
// What unlocks it: 0 = slide only (as now), 1 = spacebar, 2 = Alt+W.
// ⚠️ Spacebar is a big key in the middle of the board, which is exactly what a trouser
// pocket presses - that is why jejeronimo asked for Alt+W. Both are offered; Alt+W is the
// one to recommend.
static int s_unlockKey = -1;

static void loadStay(void)
{
    if (s_stayOn >= 0)
        return;
    Preferences p;
    if (!p.begin("tdecklock", true)) {
        s_stayOn = 0;
        s_dimPct = 15;
        s_unlockKey = 0;
        return;
    }
    s_stayOn = (int)p.getUChar("stay", 0);
    s_dimPct = (int)p.getUChar("dim", 15);
    s_unlockKey = (int)p.getUChar("ukey", 0);
    p.end();
    if (s_dimPct < 0 || s_dimPct > 100)
        s_dimPct = 15;
    if (s_unlockKey < 0 || s_unlockKey > 2)
        s_unlockKey = 0;
}

extern "C" int tdeck_lock_stayon_mins(void)
{
    loadStay();
    return s_stayOn;
}

extern "C" void tdeck_lock_set_stayon_mins(int m)
{
    loadStay();
    s_stayOn = m;
    Preferences p;
    if (p.begin("tdecklock", false)) {
        p.putUChar("stay", (uint8_t)m);
        p.end();
    }
}

extern "C" int tdeck_lock_dim_pct(void)
{
    loadStay();
    return s_dimPct;
}

extern "C" void tdeck_lock_set_dim_pct(int pct)
{
    if (pct < 0 || pct > 100)
        return;
    loadStay();
    s_dimPct = pct;
    Preferences p;
    if (p.begin("tdecklock", false)) {
        p.putUChar("dim", (uint8_t)pct);
        p.end();
    }
}

extern "C" int tdeck_lock_unlock_key(void)
{
    loadStay();
    return s_unlockKey;
}

extern "C" void tdeck_lock_set_unlock_key(int k)
{
    if (k < 0 || k > 2)
        return;
    loadStay();
    s_unlockKey = k;
    Preferences p;
    if (p.begin("tdecklock", false)) {
        p.putUChar("ukey", (uint8_t)k);
        p.end();
    }
}
