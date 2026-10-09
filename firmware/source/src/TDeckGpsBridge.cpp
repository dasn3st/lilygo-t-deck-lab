// -----------------------------------------------------------------------------
// T-Deck launcher: GPS status bridge (firmware side)
//
// Same idea as TDeckMeshSwitch.cpp: device-ui can't include firmware headers,
// so these extern "C" free functions expose the GPS driver's live state to the
// UI — satellites in view and lock/fix straight from gpsStatus, available while
// the GPS is still acquiring, before any position packet reaches the UI feed.
//
// THREADING: these are pure reads of small scalar fields on the long-lived
// gpsStatus singleton (updated by the GPS OSThread). Word-sized reads on the
// ESP32-S3 don't tear, and a stale-by-one-update value is harmless for a UI
// readout, so no locking is needed.
// -----------------------------------------------------------------------------
#include "GPSStatus.h"
#include "gps/RTC.h"
#include <time.h>

extern "C" uint32_t tdeck_gps_num_sats(void)
{
    return gpsStatus ? gpsStatus->getNumSatellites() : 0;
}

extern "C" bool tdeck_gps_has_lock(void)
{
    return gpsStatus && gpsStatus->getHasLock();
}

// Position dilution of precision x100 (e.g. 250 = PDOP 2.5); 0 = unknown. Lets the
// UI tell a solid fix from a marginal one (3 sats "locked" can be blocks off).
extern "C" uint32_t tdeck_gps_dop(void)
{
    return gpsStatus ? gpsStatus->getDOP() : 0;
}

// Current fix (1e-7 degrees, same scale the UI already uses). Returns false
// until there's a usable position. getLatitude()/getLongitude() also cover the
// fixed-position config case, where hasLock may stay false.
extern "C" bool tdeck_gps_position(int32_t *lat, int32_t *lon)
{
    if (!gpsStatus)
        return false;
    if (!gpsStatus->getHasLock() && !config.position.fixed_position)
        return false;
    int32_t la = gpsStatus->getLatitude();
    int32_t lo = gpsStatus->getLongitude();
    if (la == 0 && lo == 0)
        return false;
    *lat = la;
    *lon = lo;
    return true;
}

// Height above sea level in metres, for the Maps app (Jake, 2026-09-30: "how do we see elevation").
// Only from a real 3D fix - four satellites or more; with three the receiver solves a flat 2D
// position and its altitude is a guess. A typed-in fixed position carries whatever was typed.
extern "C" bool tdeck_gps_altitude(int32_t *meters)
{
    if (!gpsStatus || !meters)
        return false;
    if (config.position.fixed_position) {
        *meters = gpsStatus->getAltitude();
        return *meters != 0;
    }
    if (!gpsStatus->getHasLock() || gpsStatus->getNumSatellites() < 4)
        return false;
    *meters = gpsStatus->getAltitude();
    return true;
}

// Wall-clock date and time for Lua apps (device.clock()). The device has no battery-backed
// clock: it learns the time from the GPS satellites, so before a fix there is genuinely no
// date to report and we say so rather than handing back 1970.
//
// getValidTime(..., true) returns seconds already shifted into the configured time zone, and
// the zone strings carry their daylight-saving rules (see TDeckTimeZone.cpp), so the local
// fields below follow DST without any extra work here. Asking for the same instant twice -
// once local, once UTC - gives the offset to hand back, which is what an app needs to turn a
// UTC calculation (a sunrise, say) into a time on the user's own clock.
extern "C" bool tdeck_wall_clock(int *year, int *mon, int *day, int *hour, int *min, int *sec, int *offset)
{
    uint32_t utc = getValidTime(RTCQuality::RTCQualityDevice, false);
    if (!utc)
        return false; // no fix yet -> no honest answer
    uint32_t local = getValidTime(RTCQuality::RTCQualityDevice, true);

    // `local` is already offset, so read its fields with gmtime: applying the zone twice
    // would put the clock out by the offset again.
    time_t t = (time_t)local;
    struct tm tmv;
    gmtime_r(&t, &tmv);

    *year = tmv.tm_year + 1900;
    *mon = tmv.tm_mon + 1;
    *day = tmv.tm_mday;
    *hour = tmv.tm_hour;
    *min = tmv.tm_min;
    *sec = tmv.tm_sec;
    *offset = (int)((int64_t)local - (int64_t)utc);
    return true;
}
