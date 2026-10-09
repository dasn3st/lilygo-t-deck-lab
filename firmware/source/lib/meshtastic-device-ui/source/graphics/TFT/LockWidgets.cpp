// -----------------------------------------------------------------------------
// LockWidgets - the three things that can fill the middle of the lock screen.
//
// Jake, 2026-09-19: "give the option of 'Lockscreen widget': Weather (sub menu of update
// period if selected) (if not able to update... no wifi.. just keeps the previous reading).
// Satellites connected, sundown/sunrise."
//
// All three answer the same question in the same shape, so they share one struct and one
// entry point: a big headline, a word beside it, and two quiet lines underneath. The lock
// screen does not need to know which one it is drawing.
//
// WHY THIS FILE AND NOT THE VIEW. TFTView_320x240.cpp is already 16,000 lines, and none of
// this is view code - it is arithmetic and one file read. Keeping it out means the glance
// can be read in one screenful, and this can be checked on its own.
//
// THREADING. Everything here runs on the "tft" task, from inside an LVGL callback, which
// already holds spiLock for the whole of task_handler(). That is why the SD read below takes
// no lock - exactly like LuaApp.cpp's store.read, which runs on the same task. Do NOT call
// these from the main loop or from a timer on another task.
// -----------------------------------------------------------------------------
#include "graphics/view/TFT/LockWidgets.h"

#include "graphics/common/SdCard.h" // SDFs (shared SdFat instance)
#include "lvgl.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_heap_caps.h>

extern "C" bool tdeck_gps_position(int32_t *lat, int32_t *lon);
extern "C" uint32_t tdeck_gps_num_sats(void);
extern "C" bool tdeck_gps_has_lock(void);
extern "C" uint32_t tdeck_gps_dop(void); // dilution of precision, x100
extern "C" bool tdeck_wall_clock(int *year, int *mon, int *day, int *hour, int *min, int *sec, int *offset);
// Last position we ever had, kept in NVS (src/TDeckLockPrefs.cpp). Indoors there is no fix,
// and "sunset is at 7:42" is still the answer you wanted - the Sundown app made the same call.
extern "C" bool tdeck_lastpos_get(int32_t *lat, int32_t *lon);
extern "C" void tdeck_lastpos_set(int32_t lat, int32_t lon);
extern volatile const char *tdeck_tft_where; // freeze breadcrumb
// A freshly fetched forecast waiting to be written to the card (src/TDeckWeatherAuto.cpp).
// Handed over once; we own the buffer and must free it. See tdeck_wx_auto_service() for
// why the fetch happens there and the file is written here.
extern "C" char *tdeck_wx_auto_take(int *len);

namespace
{

// ---------------------------------------------------------------- sun

// Trig in degrees, which is how the almanac formula is written.
const double kRad = 3.14159265358979323846 / 180.0;
double dsin(double d) { return sin(d * kRad); }
double dcos(double d) { return cos(d * kRad); }
double dtan(double d) { return tan(d * kRad); }
double dasin(double x) { return asin(x) / kRad; }
double dacos(double x) { return acos(x) / kRad; }
double datan(double x) { return atan(x) / kRad; }

double norm(double v, double m)
{
    v = fmod(v, m);
    if (v < 0)
        v += m;
    return v;
}

int dayOfYear(int y, int m, int d)
{
    const int n1 = 275 * m / 9;
    const int n2 = (m + 9) / 12;
    const int n3 = 1 + (y - 4 * (y / 4) + 2) / 3;
    return n1 - (n2 * n3) + d - 30;
}

// The US Naval Observatory sunrise equation, ported straight from the Sundown app already on
// Jake's card (sd-apps/sun/main.lua) - same constants, same quadrant correction. Accurate to
// about a minute, which is far better than you can judge by looking.
//   zenith 90.833 = official sunrise/sunset (refraction, and the disc's EDGE not its middle)
//   zenith 96     = civil twilight: the first and last usable light, i.e. "last light".
// Returns false when the sun does not do that today, which really happens far enough north.
bool solarEvent(int y, int mo, int d, double lat, double lon, double zenith, bool rising, double offH, double *out)
{
    const int N = dayOfYear(y, mo, d);
    const double lngHour = lon / 15.0;
    const double t = rising ? (N + ((6.0 - lngHour) / 24.0)) : (N + ((18.0 - lngHour) / 24.0));

    const double M = (0.9856 * t) - 3.289;
    const double L = norm(M + (1.916 * dsin(M)) + (0.020 * dsin(2 * M)) + 282.634, 360.0);

    double RA = norm(datan(0.91764 * dtan(L)), 360.0);
    // Right ascension has to land in the same quadrant as the sun's true longitude; the
    // arctangent above throws that away, so put it back.
    RA = RA + ((floor(L / 90.0) * 90.0) - (floor(RA / 90.0) * 90.0));
    RA = RA / 15.0;

    const double sinDec = 0.39782 * dsin(L);
    const double cosDec = dcos(dasin(sinDec));

    const double cosH = (dcos(zenith) - (sinDec * dsin(lat))) / (cosDec * dcos(lat));
    if (cosH > 1.0 || cosH < -1.0)
        return false; // never rises / never sets today

    const double H = (rising ? (360.0 - dacos(cosH)) : dacos(cosH)) / 15.0;
    const double T = H + RA - (0.06571 * t) - 6.622;
    *out = norm(norm(T - lngHour, 24.0) + offH, 24.0);
    return true;
}

// Decimal hours -> "7:14 PM" or "19:14".
void hhmm(double h, bool ampm, char *out, size_t n)
{
    const int m = (int)floor(h * 60.0 + 0.5);
    int hh = (m / 60) % 24;
    const int mm = m % 60;
    if (!ampm) {
        snprintf(out, n, "%d:%02d", hh, mm);
        return;
    }
    const char *suf = (hh < 12) ? "AM" : "PM";
    hh = hh % 12;
    if (hh == 0)
        hh = 12;
    snprintf(out, n, "%d:%02d %s", hh, mm, suf);
}

void spanText(double hours, char *out, size_t n)
{
    if (hours < 0)
        hours += 24.0;
    const int m = (int)floor(hours * 60.0 + 0.5);
    snprintf(out, n, "%dh %02dm", m / 60, m % 60);
}

// Position for the sun: the live fix if there is one, otherwise the last one we saw.
// Returns 0 = never had one, 1 = live fix, 2 = remembered.
int positionNow(double *lat, double *lon)
{
    int32_t la = 0, lo = 0;
    if (tdeck_gps_position(&la, &lo)) {
        tdeck_lastpos_set(la, lo); // free: we are here anyway, and indoors this is all we get
        *lat = la * 1e-7;
        *lon = lo * 1e-7;
        return 1;
    }
    if (tdeck_lastpos_get(&la, &lo)) {
        *lat = la * 1e-7;
        *lon = lo * 1e-7;
        return 2;
    }
    return 0;
}

bool fillSun(LockWidgetText *o, bool ampm)
{
    int y, mo, d, hh, mi, ss, off;
    if (!tdeck_wall_clock(&y, &mo, &d, &hh, &mi, &ss, &off)) {
        snprintf(o->big, sizeof(o->big), "--:--");
        snprintf(o->tag, sizeof(o->tag), "no clock");
        snprintf(o->line1, sizeof(o->line1), "Waiting for the time");
        snprintf(o->line2, sizeof(o->line2), "The GPS satellites set the clock");
        return true;
    }

    double lat = 0, lon = 0;
    const int fixKind = positionNow(&lat, &lon);
    if (fixKind == 0) {
        snprintf(o->big, sizeof(o->big), "--:--");
        snprintf(o->tag, sizeof(o->tag), "no position");
        snprintf(o->line1, sizeof(o->line1), "Looking for satellites");
        snprintf(o->line2, sizeof(o->line2), "Step outside with a view of the sky");
        return true;
    }

    const double offH = off / 3600.0;
    double rise = 0, set = 0, dusk = 0;
    const bool haveRise = solarEvent(y, mo, d, lat, lon, 90.833, true, offH, &rise);
    const bool haveSet = solarEvent(y, mo, d, lat, lon, 90.833, false, offH, &set);
    const bool haveDusk = solarEvent(y, mo, d, lat, lon, 96.0, false, offH, &dusk);

    if (!haveRise || !haveSet) {
        snprintf(o->big, sizeof(o->big), "--:--");
        snprintf(o->tag, sizeof(o->tag), "polar day");
        o->tagColor = 0xff9f0a;
        snprintf(o->line1, sizeof(o->line1), "The sun does not set here today");
        return true;
    }

    // Far enough north the sun sets AFTER midnight, so "sunset" is a smaller number than
    // "sunrise" and daylight spans the date boundary. Work out whether it is day FIRST, then
    // look ahead - comparing the two in clock order gets Reykjavik in June badly wrong.
    const double now = hh + mi / 60.0 + ss / 3600.0;
    const bool isDay = (set > rise) ? (now >= rise && now < set) : (now >= rise || now < set);

    double nextAt = isDay ? set : rise;
    if (nextAt <= now)
        nextAt += 24.0;

    hhmm(isDay ? set : rise, ampm, o->big, sizeof(o->big));
    snprintf(o->tag, sizeof(o->tag), isDay ? "sunset" : "sunrise");
    o->tagColor = 0xff9f0a;

    char span[16];
    spanText(nextAt - now, span, sizeof(span));
    if (isDay)
        snprintf(o->line1, sizeof(o->line1), "%s of daylight left", span);
    else
        snprintf(o->line1, sizeof(o->line1), "sunrise in %s", span);

    char a[16], b[16];
    hhmm(isDay ? rise : set, ampm, a, sizeof(a));
    if (haveDusk) {
        hhmm(dusk, ampm, b, sizeof(b));
        snprintf(o->line2, sizeof(o->line2), "%s %s  -  last light %s", isDay ? "sunrise" : "sunset", a, b);
    } else {
        snprintf(o->line2, sizeof(o->line2), "%s %s", isDay ? "sunrise" : "sunset", a);
    }
    if (fixKind == 2)
        snprintf(o->right, sizeof(o->right), "last known");
    return true;
}

// ---------------------------------------------------------------- satellites

bool fillSats(LockWidgetText *o)
{
    const uint32_t sats = tdeck_gps_num_sats();
    const bool haveLock = tdeck_gps_has_lock();
    const uint32_t dop = tdeck_gps_dop(); // x100

    snprintf(o->big, sizeof(o->big), "%u sat%s", (unsigned)sats, sats == 1 ? "" : "s");

    if (!haveLock) {
        snprintf(o->tag, sizeof(o->tag), sats ? "searching" : "no fix");
    } else if (sats < 4 || dop > 500) {
        // A 3-satellite "lock", or bad geometry, is a marginal 2D fix that can sit blocks
        // away. The map already calls that amber; say the same thing here.
        snprintf(o->tag, sizeof(o->tag), "weak");
        o->tagColor = 0xff9f0a;
    } else {
        snprintf(o->tag, sizeof(o->tag), "locked");
        o->tagColor = 0x30d158;
    }

    int32_t la = 0, lo = 0;
    if (tdeck_gps_position(&la, &lo)) {
        tdeck_lastpos_set(la, lo);
        snprintf(o->line1, sizeof(o->line1), "%.5f, %.5f", la * 1e-7, lo * 1e-7);
    } else if (tdeck_lastpos_get(&la, &lo)) {
        snprintf(o->line1, sizeof(o->line1), "%.5f, %.5f", la * 1e-7, lo * 1e-7);
        snprintf(o->right, sizeof(o->right), "last known");
    } else {
        snprintf(o->line1, sizeof(o->line1), "no position yet");
    }

    // DOP, not a made-up metre figure. The receiver does not report an accuracy in metres,
    // and inventing one that looks authoritative would be worse than saying nothing.
    if (haveLock && dop)
        snprintf(o->line2, sizeof(o->line2), "%s geometry  (DOP %u.%u)",
                 dop <= 200 ? "good" : (dop <= 500 ? "fair" : "poor"), (unsigned)(dop / 100),
                 (unsigned)((dop / 10) % 10));
    else
        snprintf(o->line2, sizeof(o->line2), "GPS needs a clear view of the sky");
    return true;
}

// ---------------------------------------------------------------- weather

// What a WMO code means, in the fewest words that are still true. Same mapping the weather
// app uses, so the widget and the app never disagree.
const char *codeDesc(int c, uint32_t *col)
{
    *col = 0x8e8e93;
    if (c == 0) {
        *col = 0xffd60a;
        return "Clear";
    }
    if (c <= 3) {
        *col = 0xd8d8dd;
        return "Partly cloudy";
    }
    if (c == 45 || c == 48)
        return "Fog";
    if (c >= 51 && c <= 57) {
        *col = 0x5ac8fa;
        return "Drizzle";
    }
    if (c >= 61 && c <= 67) {
        *col = 0x5ac8fa;
        return "Rain";
    }
    if (c >= 71 && c <= 77) {
        *col = 0xffffff;
        return "Snow";
    }
    if (c >= 80 && c <= 82) {
        *col = 0x5ac8fa;
        return "Rain showers";
    }
    if (c >= 85 && c <= 86) {
        *col = 0xffffff;
        return "Snow showers";
    }
    if (c >= 95) {
        *col = 0xff9f0a;
        return "Thunderstorm";
    }
    return "";
}

// The cache the weather app already writes, read straight off the card. Nothing here ever
// goes online: Jake asked for exactly this - "if not able to update... no wifi.. just keeps
// the previous reading" - and the previous reading is this file.
//
// Format (apps/weather/main.lua, CACHE_VER 4):
//   1  "4"
//   2  temp|code|wind                  (temp always Fahrenheit)
//   3  place|coords|fromGps[|stamp]    (stamp added 2026-09-19; older caches have no 4th field)
//   4+ date|code|hi|lo                 (one per day; line 4 is the day it was fetched)
//
// Throttled to once a minute: the glance ticks twice a second, and hitting the SD card at that
// rate would fight the radio for the SPI bus for no benefit - the file changes at most hourly.
struct WxCache {
    uint32_t readAtMs;
    bool valid;
    int code;
    char temp[8];
    char hi[8];
    char lo[8];
    char place[40];
    char stamp[20]; // "YYYY-MM-DD HH:MM" local, or empty on an older cache
    char date[12];  // the first forecast day, i.e. the day it was fetched
};
WxCache wx;

// Pull one pipe-separated field out of a line. Returns false past the end.
bool field(const char *line, int idx, char *out, size_t n)
{
    const char *p = line;
    for (int i = 0; i < idx; i++) {
        p = strchr(p, '|');
        if (!p)
            return false;
        p++;
    }
    const char *e = strchr(p, '|');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len >= n)
        len = n - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return true;
}

// ---------------------------------------------------------------- refreshing the cache

// Find "key": in a JSON body. wantArray picks between the scalar and the array when the same
// name appears as both - which Open-Meteo does for weather_code (once under `current`, once
// under `daily`) and for time. Returns a pointer just past the colon (or past the '[').
const char *jsonFind(const char *body, const char *key, bool wantArray)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const size_t plen = strlen(pat);
    for (const char *p = strstr(body, pat); p; p = strstr(p + 1, pat)) {
        const char *v = p + plen;
        if (wantArray) {
            if (*v == '[')
                return v + 1;
            continue;
        }
        // ⚠️ KEEP LOOKING UNTIL THE VALUE IS A NUMBER. Open-Meteo sends a "current_units"
        // block BEFORE "current", carrying the same names with their units as strings -
        // "temperature_2m":"°F", "weather_code":"wmo code". Taking the first match got
        // the unit, not the reading. (Lua's pattern skipped these for free; C has to be told.)
        if (*v == '-' || *v == '.' || (*v >= '0' && *v <= '9'))
            return v;
    }
    return nullptr;
}

// Copy one plain scalar (number) into out.
bool jsonScalar(const char *body, const char *key, char *out, size_t n)
{
    const char *v = jsonFind(body, key, false);
    if (!v)
        return false;
    size_t i = 0;
    while (*v && (*v == '-' || *v == '.' || (*v >= '0' && *v <= '9')) && i < n - 1)
        out[i++] = *v++;
    out[i] = 0;
    return i > 0;
}

// Copy element `idx` of an array, stripping quotes. Elements are numbers or ISO dates.
bool jsonElem(const char *arr, int idx, char *out, size_t n)
{
    const char *p = arr;
    for (int k = 0; k < idx; k++) {
        p = strchr(p, ',');
        if (!p)
            return false;
        p++;
    }
    while (*p == ' ' || *p == '"')
        p++;
    size_t i = 0;
    while (*p && *p != ',' && *p != ']' && *p != '"' && i < n - 1)
        out[i++] = *p++;
    out[i] = 0;
    return i > 0;
}

// Merge a freshly fetched forecast into the file the Weather app owns, keeping the place name
// it looked up and stamping the time so the widget can say how old the reading is.
//
// It only ever UPDATES: with no existing cache there is no place name, and inventing one would
// be worse than saying "open the Weather app once", which is what the widget already does.
void mergeFetched(char *body)
{
    char *old = (char *)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    char *out = (char *)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (!old || !out) {
        if (old)
            heap_caps_free(old);
        if (out)
            heap_caps_free(out);
        return;
    }
    int n = -1;
    tdeck_tft_where = "sd-weather";
    FsFile f = SDFs.open("/apps/weather/forecast.txt", O_RDONLY);
    if (f) {
        n = f.read((uint8_t *)old, 1023);
        f.close();
    }
    if (n <= 0) {
        heap_caps_free(old);
        heap_caps_free(out);
        return; // the app has never saved one; not ours to create
    }
    old[n] = 0;

    // line 3 carries the place, which we keep exactly as it was
    char keep[96] = "";
    {
        char *l1 = strchr(old, '\n');
        char *l2 = l1 ? strchr(l1 + 1, '\n') : nullptr;
        char *l3 = l2 ? strchr(l2 + 1, '\n') : nullptr;
        if (l2) {
            const size_t len = (size_t)((l3 ? l3 : old + n) - (l2 + 1));
            const size_t cap = len < sizeof(keep) - 1 ? len : sizeof(keep) - 1;
            memcpy(keep, l2 + 1, cap);
            keep[cap] = 0;
        }
    }
    if (!keep[0]) {
        heap_caps_free(old);
        heap_caps_free(out);
        return;
    }
    // drop any stamp already on the end, then add the current one
    {
        int bars = 0;
        for (char *p = keep; *p; p++) {
            if (*p == '|' && ++bars == 3) {
                *p = 0;
                break;
            }
        }
    }
    char stamp[24] = "";
    int y, mo, d, hh, mi, ss, off;
    if (tdeck_wall_clock(&y, &mo, &d, &hh, &mi, &ss, &off))
        snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d", y, mo, d, hh, mi);

    char temp[12] = "", code[8] = "", wind[12] = "";
    if (!jsonScalar(body, "temperature_2m", temp, sizeof(temp))) {
        heap_caps_free(old);
        heap_caps_free(out);
        return; // not a forecast we understand; leave the good cache alone
    }
    jsonScalar(body, "weather_code", code, sizeof(code));
    jsonScalar(body, "wind_speed_10m", wind, sizeof(wind));

    int len = snprintf(out, 1024, "4\n%s|%s|%s\n%s|%s\n", temp, code, wind, keep, stamp);

    const char *aDate = jsonFind(body, "time", true);
    const char *aCode = jsonFind(body, "weather_code", true);
    const char *aHi = jsonFind(body, "temperature_2m_max", true);
    const char *aLo = jsonFind(body, "temperature_2m_min", true);
    if (aDate && aCode && aHi && aLo) {
        for (int i = 0; i < 7 && len < 900; i++) {
            char dt[16], cd[8], hi[12], lo[12];
            if (!jsonElem(aDate, i, dt, sizeof(dt)) || !jsonElem(aCode, i, cd, sizeof(cd)) ||
                !jsonElem(aHi, i, hi, sizeof(hi)) || !jsonElem(aLo, i, lo, sizeof(lo)))
                break;
            len += snprintf(out + len, (size_t)(1024 - len), "%s|%s|%s|%s\n", dt, cd, hi, lo);
        }
    }

    FsFile w = SDFs.open("/apps/weather/forecast.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (w) {
        w.write((const uint8_t *)out, (size_t)len);
        w.close();
        wx.readAtMs = 0; // force the next read to pick the new file up
    }
    heap_caps_free(old);
    heap_caps_free(out);
}

void loadWeatherCache(void)
{
    // A background refresh may have landed since last time. Writing the file is done HERE,
    // on the tft task, because this is where SDFs lives and where the SPI lock is already
    // held - the fetch itself ran on the main loop and never touched the card.
    if (char *fresh = tdeck_wx_auto_take(nullptr)) {
        mergeFetched(fresh);
        heap_caps_free(fresh);
    }

    const uint32_t now = lv_tick_get();
    if (wx.readAtMs && (now - wx.readAtMs) < 60000)
        return;
    wx.readAtMs = now ? now : 1;

    // PSRAM, not the stack and not a static. The tft task has a 16KB stack so 1KB WOULD fit -
    // measured, not assumed, via the stack figures @@mem now reports - but a static would hold
    // scarce internal RAM permanently for a read that happens once a minute, and the stack is
    // shared with LVGL's own draw recursion. PSRAM costs nothing here.
    char *buf = (char *)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (!buf)
        return;
    int n = -1;
    FsFile f = SDFs.open("/apps/weather/forecast.txt", O_RDONLY);
    if (f) {
        n = f.read((uint8_t *)buf, 1023);
        f.close();
    }
    if (n <= 0) {
        heap_caps_free(buf);
        return; // no cache yet, or no card: keep whatever we had
    }
    buf[n] = 0;

    // Split off the first four lines; that is everything the widget needs.
    char *lines[4] = {nullptr, nullptr, nullptr, nullptr};
    int ln = 0;
    for (char *p = buf; *p && ln < 4;) {
        lines[ln++] = p;
        char *nl = strchr(p, '\n');
        if (!nl)
            break;
        *nl = 0;
        p = nl + 1;
    }
    if (ln < 3 || strcmp(lines[0], "4") != 0) {
        heap_caps_free(buf);
        return; // a version we do not know how to read; leave the old values alone
    }

    char codeBuf[8] = "";
    field(lines[1], 0, wx.temp, sizeof(wx.temp));
    field(lines[1], 1, codeBuf, sizeof(codeBuf));
    wx.code = codeBuf[0] ? atoi(codeBuf) : -1;
    field(lines[2], 0, wx.place, sizeof(wx.place));
    if (!wx.place[0])
        field(lines[2], 1, wx.place, sizeof(wx.place)); // no town name: the coordinates will do
    if (!field(lines[2], 3, wx.stamp, sizeof(wx.stamp)))
        wx.stamp[0] = 0;
    if (lines[3]) {
        field(lines[3], 0, wx.date, sizeof(wx.date));
        field(lines[3], 2, wx.hi, sizeof(wx.hi));
        field(lines[3], 3, wx.lo, sizeof(wx.lo));
    }
    wx.valid = wx.temp[0] != 0;
    heap_caps_free(buf);
}

// Fahrenheit in the file, whatever the user asked for on screen.
int toDisplay(const char *f, bool celsius)
{
    const double v = atof(f);
    return (int)floor((celsius ? (v - 32.0) * 5.0 / 9.0 : v) + 0.5);
}

// How old is the reading? Exact when the cache carries a stamp, and to the day when it does
// not (a cache written before the stamp existed). Never guesses.
void ageText(char *out, size_t n)
{
    int y, mo, d, hh, mi, ss, off;
    if (!tdeck_wall_clock(&y, &mo, &d, &hh, &mi, &ss, &off)) {
        snprintf(out, n, "saved reading");
        return;
    }
    int sy, smo, sd, shh, smi;
    if (wx.stamp[0] && sscanf(wx.stamp, "%d-%d-%d %d:%d", &sy, &smo, &sd, &shh, &smi) == 5) {
        // Days apart via a day number, so a month boundary does not read as "-27 days".
        const long a = (long)y * 372 + (long)mo * 31 + d;
        const long b = (long)sy * 372 + (long)smo * 31 + sd;
        long mins = (a - b) * 1440L + ((long)hh * 60 + mi) - ((long)shh * 60 + smi);
        if (mins < 0)
            mins = 0; // the clock can jump backwards on a first GPS fix; do not print nonsense
        if (mins < 90)
            snprintf(out, n, "updated %ldm ago", mins);
        else if (mins < 2880)
            snprintf(out, n, "updated %ldh ago", mins / 60);
        else
            snprintf(out, n, "updated %ld days ago", mins / 1440);
        return;
    }
    int fy, fmo, fd;
    if (wx.date[0] && sscanf(wx.date, "%d-%d-%d", &fy, &fmo, &fd) == 3) {
        const long days = ((long)y * 372 + (long)mo * 31 + d) - ((long)fy * 372 + (long)fmo * 31 + fd);
        if (days <= 0)
            snprintf(out, n, "updated today");
        else if (days == 1)
            snprintf(out, n, "updated yesterday");
        else
            snprintf(out, n, "%ld days old", days);
        return;
    }
    snprintf(out, n, "saved reading");
}

bool fillWeather(LockWidgetText *o, bool celsius)
{
    loadWeatherCache();
    if (!wx.valid) {
        snprintf(o->big, sizeof(o->big), "--");
        snprintf(o->tag, sizeof(o->tag), "no reading");
        snprintf(o->line1, sizeof(o->line1), "Open the Weather app once");
        snprintf(o->line2, sizeof(o->line2), "It saves the forecast for here");
        return true;
    }

    snprintf(o->big, sizeof(o->big), "%d%c", toDisplay(wx.temp, celsius), celsius ? 'C' : 'F');
    uint32_t col = 0x8e8e93;
    snprintf(o->tag, sizeof(o->tag), "%s", codeDesc(wx.code, &col));
    o->tagColor = col;
    snprintf(o->line1, sizeof(o->line1), "%s", wx.place[0] ? wx.place : "location unknown");
    ageText(o->line2, sizeof(o->line2));
    if (wx.hi[0] && wx.lo[0])
        snprintf(o->right, sizeof(o->right), "H %d  L %d", toDisplay(wx.hi, celsius), toDisplay(wx.lo, celsius));
    return true;
}

} // namespace

bool lockwidget_fill(int which, bool celsius, bool ampm, LockWidgetText *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    out->tagColor = 0x8e8e93;
    switch (which) {
    case 1:
        return fillWeather(out, celsius);
    case 2:
        return fillSats(out);
    case 3:
        return fillSun(out, ampm);
    default:
        return false;
    }
}
