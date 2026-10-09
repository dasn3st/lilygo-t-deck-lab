// -----------------------------------------------------------------------------
// T-Deck launcher: WiFi status bridge (firmware side)
//
// Same pattern as TDeckGpsBridge.cpp / TDeckMeshSwitch.cpp: device-ui can't
// include firmware/Arduino headers, so these extern "C" free functions expose
// the live WiFi state to our Settings screen — enough to show "Connecting…" vs
// "Connected <ip>". The network config itself (enable, SSID, password) is set
// from the UI via the normal admin config path; this file is read-only status.
//
// THREADING: pure reads of the Arduino WiFi singleton + the config struct. A
// value that's stale by one poll is harmless for a status readout, so no locking.
// -----------------------------------------------------------------------------
#include "configuration.h"

#if HAS_WIFI
#include "NodeDB.h" // the global `config`
#include <WiFi.h>
#include <esp_wifi.h> // esp_wifi_set_ps
#endif

// 0 = off / not configured, 1 = enabled but not yet connected, 2 = connected
extern "C" int tdeck_wifi_state(void)
{
#if HAS_WIFI
    if (!config.network.wifi_enabled || config.network.wifi_ssid[0] == 0)
        return 0;
    return (WiFi.status() == WL_CONNECTED) ? 2 : 1;
#else
    return 0;
#endif
}

// Fills buf with the device's IP once connected, else an empty string.
extern "C" void tdeck_wifi_ip(char *buf, int len)
{
    if (!buf || len <= 0)
        return;
    buf[0] = 0;
#if HAS_WIFI
    if (WiFi.status() == WL_CONNECTED)
        snprintf(buf, len, "%s", WiFi.localIP().toString().c_str());
#endif
}

// --- Network scan (so the UI can offer a pick-from-a-list instead of typing) ---
// Async so the UI never blocks. If scanning fails on this build (WiFi/BT share the
// radio), the UI just falls back to manual entry — nothing gets stuck.

extern "C" void tdeck_wifi_scan_start(void)
{
#if HAS_WIFI
    WiFi.mode(WIFI_STA);            // scanning needs station mode; harmless if already there
    WiFi.scanDelete();             // drop any previous results
    WiFi.scanNetworks(true, false); // async = true, show_hidden = false
#endif
}

// -2 = failed / not started, -1 = still running, >= 0 = number of networks found
extern "C" int tdeck_wifi_scan_done(void)
{
#if HAS_WIFI
    return WiFi.scanComplete();
#else
    return -2;
#endif
}

// Read one scan result. secure = 1 for password-protected networks, 0 for open.
extern "C" void tdeck_wifi_scan_get(int i, char *ssid, int len, int *rssi, int *secure)
{
    if (ssid && len > 0)
        ssid[0] = 0;
#if HAS_WIFI
    if (i < 0)
        return;
    if (ssid && len > 0)
        snprintf(ssid, len, "%s", WiFi.SSID(i).c_str());
    if (rssi)
        *rssi = WiFi.RSSI(i);
    if (secure)
        *secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? 1 : 0;
#endif
}

extern "C" void tdeck_wifi_scan_free(void)
{
#if HAS_WIFI
    WiFi.scanDelete();
#endif
}

// --- On-demand connect (for the File Share / FTP screen) ------------------------------------
// Brings WiFi up using the SSID/password the user already saved, WITHOUT touching Meshtastic's
// network config — so there's no reboot and WiFi isn't draining RAM the rest of the time (Jake's
// "WiFi on-demand" decision). Assumes WiFi is normally OFF in config; if the user has WiFi
// enabled in Settings, Meshtastic manages it and these shouldn't be used.
// The SSID/password are passed in from the UI (db.config), which is where the user typed them —
// that copy is authoritative even if they never ran the reboot-to-apply toggle. Returns false if
// no network name was given.
extern "C" bool tdeck_wifi_connect_now(const char *ssid, const char *psk)
{
#if HAS_WIFI
    if (!ssid || ssid[0] == 0)
        return false;
    WiFi.mode(WIFI_STA);
    // Radio power saving OFF while we are on, as Meshtastic's own Wi-Fi path does ("needed to
    // improve performance", WiFiAPClient.cpp). Left on, the radio dozes between beacons and every
    // reply waits for it: place names came down at ~4KB/s (measured 2026-09-30). Wi-Fi here is only
    // ever on for a download, and tdeck_wifi_disconnect_now() turns the radio off afterwards. ESP-IDF
    // refuses this while Bluetooth is on (coexistence needs modem sleep), hence the log line.
    const esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    LOG_INFO("wifi on demand: power save off -> %s", esp_err_to_name(ps));
    WiFi.begin(ssid, psk ? psk : "");
    return true;
#else
    return false;
#endif
}

// Drop the connection and power the WiFi radio back down (frees the internal RAM it was using).
extern "C" void tdeck_wifi_disconnect_now(void)
{
#if HAS_WIFI
    WiFi.disconnect(true); // true = also turn the radio off
    WiFi.mode(WIFI_OFF);
#endif
}

// Simple "are we online yet" check that works regardless of the config flag (the on-demand path
// above deliberately leaves config.network.wifi_enabled alone, so tdeck_wifi_state() can't be
// used for this).
extern "C" bool tdeck_wifi_connected(void)
{
#if HAS_WIFI
    return WiFi.status() == WL_CONNECTED;
#else
    return false;
#endif
}
