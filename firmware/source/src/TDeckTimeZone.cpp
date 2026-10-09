// -----------------------------------------------------------------------------
// T-Deck launcher: time zone bridge (firmware side)
//
// Same deferred pattern as TDeckGpsControl.cpp, and for the same reason: the LVGL
// UI runs on its own FreeRTOS task and must NOT write settings to flash from
// there. The first version of this file called nodeDB->saveToDisk() straight from
// the dropdown's event callback and froze the device when Jake changed zone —
// flash writes from the UI task collide with the main loop's own filesystem use.
// So the UI-facing setter now only records intent, and tdeck_tz_service(), called
// from loop(), does the real work on the safe thread.
//
// Why this exists at all: the device gets accurate UTC from the GPS satellites,
// but with no zone configured main.cpp falls back to "GMT0", so every clock reads
// UTC. The strings below are POSIX TZ rules including the daylight-saving
// changeover dates, so the clock follows DST by itself.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include "mesh/NodeDB.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Kept in step with the dropdown in the launcher's Settings screen (same order).
static const char *kTdeckZones[] = {
    "PST8PDT,M3.2.0,M11.1.0",     // Pacific
    "MST7MDT,M3.2.0,M11.1.0",     // Mountain
    "MST7",                       // Arizona (no DST)
    "CST6CDT,M3.2.0,M11.1.0",     // Central
    "EST5EDT,M3.2.0,M11.1.0",     // Eastern
    "AKST9AKDT,M3.2.0,M11.1.0",   // Alaska
    "HST10",                      // Hawaii
    "GMT0",                       // UTC
    "GMT0BST,M3.5.0/1,M10.5.0",   // UK
    "CET-1CEST,M3.5.0,M10.5.0/3", // Central Europe
};
static const int kTdeckZoneCount = sizeof(kTdeckZones) / sizeof(kTdeckZones[0]);

static volatile int s_tdeckTzPending = -1; // index the UI asked for; -1 = nothing waiting

// Called from the UI (LVGL) task. Records intent ONLY — no flash, no tzset here.
extern "C" void tdeck_tz_set(int idx)
{
    if (idx >= 0 && idx < kTdeckZoneCount)
        s_tdeckTzPending = idx;
}

// Which entry is currently in effect, or -1 if the device has no zone we recognise
// (including the common case of none set at all, where the firmware silently runs on
// GMT). The UI must show that honestly rather than displaying the first entry, which
// looked like "Pacific" was already chosen while the clock was really on UTC.
extern "C" int tdeck_tz_get(void)
{
    if (!config.device.tzdef[0])
        return -1;
    for (int i = 0; i < kTdeckZoneCount; i++)
        if (strcmp(config.device.tzdef, kTdeckZones[i]) == 0)
            return i;
    return -1;
}

// Called from the main Meshtastic loop() — the safe context for touching flash.
extern "C" void tdeck_tz_service(void)
{
    int idx = s_tdeckTzPending;
    if (idx < 0)
        return;
    s_tdeckTzPending = -1;

    strncpy(config.device.tzdef, kTdeckZones[idx], sizeof(config.device.tzdef) - 1);
    config.device.tzdef[sizeof(config.device.tzdef) - 1] = 0;
    setenv("TZ", config.device.tzdef, 1);
    tzset(); // localtime() is correct from here on, everywhere
    if (nodeDB)
        nodeDB->saveToDisk(SEGMENT_CONFIG);
    LOG_INFO("T-Deck time zone set to %s", config.device.tzdef);
}
