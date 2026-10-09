// -----------------------------------------------------------------------------
// TDeckWeatherAuto - keep the lock screen's weather widget current, without ever
// surprising anybody.
//
// Jake, 2026-09-19: "'Lockscreen widget': Weather (sub menu of update period if selected)
// (if not able to update... no wifi.. just keeps the previous reading)."
//
// ⛔ THE RULE THAT SHAPES ALL OF THIS: IT NEVER BRINGS WI-FI UP.
// TDeckNet's NET_START calls disableBluetooth(), and Bluetooth stays down until the next
// reboot. A background timer that quietly killed the link to the phone app would be a
// genuinely nasty thing for a device to do to you. So this refreshes ONLY when Wi-Fi is
// already connected - at which point Bluetooth is already down and nothing new is lost.
// With Wi-Fi off, nothing happens and the widget keeps showing the last reading, which is
// exactly what was asked for.
//
// IT ALSO NEVER CREATES A FORECAST FROM NOTHING. It updates an existing cache in place:
// the numbers and the timestamp change, the place name the Weather app looked up is left
// alone. If the app has never run there is no cache, and the widget says so rather than
// this inventing a location it cannot name.
//
// WHERE THE WORK HAPPENS. The fetch runs on the main loop (TDeckNet's own service). The
// FILE is written on the "tft" task, from LockWidgets.cpp, because that is where SDFs lives
// and where task_handler() already holds the SPI lock. Nothing here touches the card.
// -----------------------------------------------------------------------------
#include "configuration.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>

#if HAS_WIFI
#include <WiFi.h>
#endif

extern "C" int tdeck_lock_widget(void);          // 1 = Weather
extern "C" uint32_t tdeck_lock_wx_minutes(void); // 0 = never
extern "C" bool tdeck_lastpos_get(int32_t *lat, int32_t *lon);
extern "C" bool tdeck_net_fetch(const char *url);
extern "C" int tdeck_net_poll(void); // 0 idle, 1 working, 2 done, 3 error
extern "C" int tdeck_net_result(char *buf, int cap);
extern "C" void tdeck_net_reset(void);

namespace
{
enum State { IDLE, WAITING };
State s_state = IDLE;
uint32_t s_lastTryMs = 0;   // when we last STARTED one, success or not
bool s_everTried = false;

// The body, handed across to the tft task exactly once. PSRAM: it is ~2KB and only alive
// between the fetch finishing and the widget picking it up.
char *s_body = nullptr;
int s_bodyLen = 0;

const int kMaxBody = 4096;

bool wifiUp(void)
{
#if HAS_WIFI
    return WiFi.isConnected();
#else
    return false;
#endif
}
} // namespace

// Called by LockWidgets.cpp on the tft task. Hands over the body ONCE and clears it; the
// caller owns the buffer from then on and must heap_caps_free it.
extern "C" char *tdeck_wx_auto_take(int *len)
{
    if (!s_body)
        return nullptr;
    char *b = s_body;
    if (len)
        *len = s_bodyLen;
    s_body = nullptr;
    s_bodyLen = 0;
    return b;
}

extern "C" void tdeck_wx_auto_service(void)
{
    const uint32_t period = tdeck_lock_wx_minutes();

    if (s_state == IDLE) {
        if (tdeck_lock_widget() != 1 || period == 0)
            return; // not the chosen widget, or set to Never
        if (s_body)
            return; // last one is still waiting to be picked up
        if (!wifiUp())
            return; // the whole point: never bring it up, just use it if it is there
        const uint32_t now = millis();
        // A first refresh two minutes after boot, so the node sync and the Wi-Fi handshake
        // are out of the way first, then on the period. Both in one comparison.
        const uint32_t due = s_everTried ? (period * 60000UL) : 120000UL;
        if (s_everTried && (now - s_lastTryMs) < due)
            return;
        if (!s_everTried && now < due)
            return;

        int32_t lat = 0, lon = 0;
        if (!tdeck_lastpos_get(&lat, &lon))
            return; // we have never known where we are; nothing to ask about

        char url[288];
        snprintf(url, sizeof(url),
                 "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                 "&current=temperature_2m,weather_code,wind_speed_10m"
                 "&daily=weather_code,temperature_2m_max,temperature_2m_min"
                 "&temperature_unit=fahrenheit&wind_speed_unit=mph&timezone=auto",
                 lat * 1e-7, lon * 1e-7);

        s_lastTryMs = now ? now : 1;
        s_everTried = true;
        if (!tdeck_net_fetch(url))
            return; // busy, or no saved network: try again next period, quietly
        s_state = WAITING;
        LOG_INFO("[WeatherAuto] refreshing the lock-screen forecast");
        return;
    }

    // WAITING
    const int st = tdeck_net_poll();
    if (st == 1)
        return;
    if (st == 2) {
        char *buf = (char *)heap_caps_malloc(kMaxBody, MALLOC_CAP_SPIRAM);
        if (buf) {
            const int n = tdeck_net_result(buf, kMaxBody);
            if (n > 0) {
                s_body = buf;
                s_bodyLen = n;
            } else {
                heap_caps_free(buf);
            }
        } else {
            tdeck_net_result(nullptr, 0); // still drain it, so the door goes back to idle
        }
        s_state = IDLE;
        return;
    }
    if (st == 3) {
        LOG_INFO("[WeatherAuto] fetch failed; keeping the previous reading");
        tdeck_net_reset();
        s_state = IDLE;
        return;
    }
    // st == 0: somebody else reset the door under us. Nothing to collect.
    s_state = IDLE;
}
