// -----------------------------------------------------------------------------
// TDeckNet — the "internet door" for Lua apps (firmware side).
//
// Lets an app fetch a URL over Wi-Fi WITHOUT freezing the screen and WITHOUT the
// Wi-Fi/Bluetooth crash. The app never blocks: it calls net.fetch(url), keeps
// drawing, and checks net.status()/net.body() on later frames.
//
// THREADING: the LVGL/app task must not drive the radio (Wi-Fi/BT) or block on a
// network read — that is exactly what froze the device before. So the app-facing
// calls only RECORD intent and READ results; tdeck_net_service(), called from the
// main loop(), does the real work on the safe thread as a non-blocking state
// machine. Same deferred pattern as TDeckMeshSwitch / TDeckGpsControl / TDeckClockFormat.
//
// Wi-Fi and Bluetooth share the one 2.4GHz radio on the ESP32-S3, and bringing Wi-Fi
// up while BT is live is what caused the watchdog reboots (the open Get-Apps/BT freeze
// bug). So this shuts Bluetooth down FIRST. BT stays down until the next reboot — the
// chip can't run both well at once, and cleanly re-enabling BT afterwards is a follow-up.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include <string.h>

#if HAS_WIFI
#include "NodeDB.h" // config.network.wifi_ssid / wifi_psk
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#endif

// The launcher's on-demand Wi-Fi bridge (src/TDeckWifi.cpp) — reuse it rather than
// duplicate WiFi.begin/status/off.
extern "C" bool tdeck_wifi_connect_now(const char *ssid, const char *psk);
extern "C" void tdeck_wifi_disconnect_now(void);
extern "C" bool tdeck_wifi_connected(void);
// Shut Bluetooth down before Wi-Fi comes up (defined in src/modules/AdminModule.cpp).
// Safe to call even if BT is already down (deinit is a no-op then).
void disableBluetooth();

// -----------------------------------------------------------------------------
// The TLS memory reserve.
//
// Measured on Jake's device (2026-08-05): opening Get Apps failed with mbedTLS -32512,
// "SSL - Memory allocation failed". At that moment there were 79332 bytes of internal RAM
// free, but the largest single run of it was 16372 bytes. Not a leak and not a shortage: the
// heap is simply chopped up by the time anyone opens Get Apps, and no amount of freeing things
// afterwards puts a big enough run back together.
//
// How big a run does it need? Measured, not assumed -- the first two answers were both wrong.
// Handing it a 25588-byte run STILL failed, which rules out the single 16KB record buffer and
// points at the inbound and outbound buffers being taken together. tdeck_tls_watch_allocs()
// below now makes the allocator state the real figure rather than us inferring it again.
//
// PSRAM cannot help. mbedTLS record buffers must live in internal RAM.
//
// So we take the block at the very start of setup(), while the heap is still one clean
// stretch, and hand it back the moment a fetch needs it. Nothing else can take that run in
// between. This costs a fixed slice of internal RAM while idle, which is the price of the
// download working at all -- and it is given back for the duration of every fetch anyway.
//
// The cleaner long-term fix is CONFIG_MBEDTLS_DYNAMIC_BUFFER in sdkconfig, which frees the
// record buffers between handshake phases. That is a full ESP-IDF rebuild and a much bigger
// change than a fix for this bug warrants right now.
// -----------------------------------------------------------------------------
// ⭐ THE RESERVE IS SEVERAL BLOCKS, NOT ONE. MEASURED 2026-09-25, and this is the whole fix.
//
// The heap settles at ~27KB free with a LARGEST BLOCK OF ONLY ~16KB - not a leak, it stabilises
// there; it is simply chopped up. So the old single 36KB reserve could be taken once at boot
// while the heap was clean, and NEVER re-taken afterwards: the ladder bottomed out at 28KB and
// nothing that big exists again. One failed re-take and HTTPS was dead for the rest of the
// session - which is Jake's "Couldn't reach the app list: error -1" and Gemini failing on a
// second question.
//
// ⛔ AND ONE BIG HOLE WAS NEVER WHAT WAS NEEDED. mbedTLS allocates its inbound and outbound
// record buffers SEPARATELY, ~16.7KB each. The old comment even said so - "~16.7KB each, so
// ~34KB" - and then reserved 34KB as a single block anyway. Two 18KB holes satisfy the
// handshake exactly as well as one 36KB hole, and in a fragmented heap they are enormously
// easier to find. That is the difference between recoverable and not.
static const int kTlsBlocks = 2;
static void *s_tlsReserve[kTlsBlocks] = {nullptr, nullptr};
static size_t s_tlsReserveBytes = 0; // total currently held, for logging
static size_t kTlsBlockBytes[kTlsBlocks] = {0, 0};
// ⛔ SOMEBODY IS USING THE HOLE RIGHT NOW - DO NOT TAKE IT BACK. Set by release(), cleared by
// take(). Without this the 5s opportunistic re-take added earlier today grabbed the reserve back
// between the release and the handshake, and the handshake then failed for want of exactly the
// memory that had just been freed for it. Get Apps releases through this path but does NOT go
// through the NET_* state machine, so "is a fetch running" could not be answered by s_state.
static volatile bool s_tlsLentOut = false;

// What the heap was actually asked for when an allocation failed. Guessing the size TLS needs
// has now been wrong twice (first "16KB", then "25KB is surely enough" - both failed), so let
// the allocator report the real figure instead. The hook fires inside the failing allocation,
// so it only records; the printing happens later from a safe place.
static volatile size_t s_lastFailSize = 0;
static volatile uint32_t s_lastFailCaps = 0;
static volatile uint32_t s_failCount = 0;

static void tlsAllocFailedHook(size_t size, uint32_t caps, const char *fn)
{
    (void)fn;
    s_lastFailSize = size;
    s_lastFailCaps = caps;
    s_failCount++;
}

extern "C" void tdeck_tls_watch_allocs(void)
{
#if HAS_WIFI
    heap_caps_register_failed_alloc_callback(tlsAllocFailedHook);
#endif
}

// Print (and clear) whatever the last failed allocation was. Called after a failed fetch.
extern "C" void tdeck_tls_report_alloc_fail(void)
{
#if HAS_WIFI
    if (!s_failCount) {
        LOG_INFO("tls: no allocation failure was recorded");
        return;
    }
    LOG_INFO("tls: LAST FAILED ALLOC = %u bytes, caps=0x%08x, %u failure(s) total", (unsigned)s_lastFailSize,
             (unsigned)s_lastFailCaps, (unsigned)s_failCount);
    LOG_INFO("tls: largest internal block available was %u", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_failCount = 0;
#endif
}

// NOTE: this runs as the FIRST statement of setup(), which means the serial console does not
// exist yet. It must not log. An earlier version called LOG_INFO here and every boot died in
// RedirectablePrint with a LoadProhibited on a near-null address (EXCVADDR 0x114c) before a
// single line of output existed. Record the size and let a later caller report it.
extern "C" void tdeck_tls_reserve_init(void)
{
#if HAS_WIFI
    // Each block is taken INDEPENDENTLY, and each steps down on its own. Partial success is
    // genuinely useful here: one 18KB hole covers one of the two record buffers, which can be
    // the difference between a handshake that works and one that does not. The old all-or-
    // nothing ladder threw that away.
    static const size_t kTry[] = {18 * 1024, 17 * 1024, 16 * 1024, 12 * 1024};
    s_tlsReserveBytes = 0;
    for (int b = 0; b < kTlsBlocks; b++) {
        if (s_tlsReserve[b]) { // already held
            s_tlsReserveBytes += kTlsBlockBytes[b];
            continue;
        }
        for (size_t i = 0; i < sizeof(kTry) / sizeof(kTry[0]); i++) {
            void *p = heap_caps_malloc(kTry[i], MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (p) {
                s_tlsReserve[b] = p;
                kTlsBlockBytes[b] = kTry[i];
                s_tlsReserveBytes += kTry[i];
                break;
            }
        }
    }
#endif
}

// Hand the block back so a TLS handshake can have it. Safe to call when it's already released.
extern "C" void tdeck_tls_reserve_release(void)
{
#if HAS_WIFI
    s_tlsLentOut = true; // hands off until take() says the caller has finished
    size_t freed = 0;
    for (int b = 0; b < kTlsBlocks; b++) {
        if (!s_tlsReserve[b])
            continue;
        heap_caps_free(s_tlsReserve[b]);
        s_tlsReserve[b] = nullptr;
        freed += kTlsBlockBytes[b];
        kTlsBlockBytes[b] = 0;
    }
    s_tlsReserveBytes = 0;
    if (!freed) {
        LOG_INFO("tls reserve: nothing held to release");
        return;
    }
    // Safe to log here: this only runs from the UI, long after the console is up.
    LOG_INFO("tls reserve: released %u bytes in %d blocks, largest internal block now %u", (unsigned)freed,
             kTlsBlocks, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#endif
}

// Take it back once the download is done, so the next one has it too. Best-effort: if the
// heap is busy right now we simply go without, and try again after the following fetch.
extern "C" void tdeck_tls_reserve_take(void)
{
#if HAS_WIFI
    s_tlsLentOut = false;
    tdeck_tls_reserve_init();
#endif
}

enum NetState { NET_IDLE = 0, NET_START, NET_CONNECTING, NET_FETCH, NET_DONE, NET_ERROR };

static volatile int s_state = NET_IDLE;
static volatile bool s_pending = false; // a fresh request from the app task
static char s_url[256];
static uint32_t s_deadline = 0;
static char *s_body = nullptr; // PSRAM; holds the response body while NET_DONE
// Did WE bring wi-fi up, or was it already on? Get Apps has always checked this and reused an
// existing connection; this door did not, and tore wi-fi down and back up for EVERY fetch. With
// wi-fi enabled in Settings that meant fighting the firmware's own connection manager, two full
// reconnects per Weather refresh (it fetches twice: where are you, then what's the weather), and
// the reconnect flapping seen in the logs. Only disconnect what we opened.
static bool s_ownWifi = false;
static uint32_t s_dropAt = 0; // if we own wi-fi, when to drop it if no further fetch arrives
static volatile int s_bodyLen = 0;
static const int kNetMaxBody = 8192; // app responses are small (weather JSON ~1-2KB)

// ---- app-facing (called on the LVGL/app task) --------------------------------

// net.fetch(url) -> bool. Starts a fetch. false if one's already running, the URL is
// empty, or no Wi-Fi network is saved (the app should then tell the user to set Wi-Fi up).
extern "C" bool tdeck_net_fetch(const char *url); // defined below; the POST wrapper queues through it

// A POST body, when one has been set. Owned here and freed after the request, because the
// caller (Gemini) builds it on the UI task and the request runs on the net task.
static char *s_postBody = nullptr;
static size_t s_postLen = 0;
// Multipart uploads need the full boundary in Content-Type. 48 bytes truncated
// the voice boundary and left the transcription server waiting for a delimiter
// that never arrived.
static char s_postType[128] = {0};
// Optional bearer token for provider APIs such as Ollama Cloud.  It is kept in RAM only;
// callers load it from the SD card at runtime and never compile it into the firmware.
static char s_authHeader[160] = {0};
static int s_httpCode = 0; // last HTTP status, so a caller can tell 404 from 429

extern "C" void tdeck_net_set_auth(const char *value)
{
    snprintf(s_authHeader, sizeof(s_authHeader), "%s", value ? value : "");
}

extern "C" void tdeck_net_clear_auth(void)
{
    memset(s_authHeader, 0, sizeof(s_authHeader));
}

// Queue a POST instead of a GET. Same state machine, same TLS reserve, same client - see the
// note in the fetch below about why there must not be a second TLS path on this device.
extern "C" bool tdeck_net_post_bytes(const char *url, const uint8_t *body, size_t bodyLen, const char *contentType)
{
    if (!url || !body || !bodyLen) {
        LOG_INFO("net: POST rejected, empty URL/body");
        return false;
    }
    if (s_postBody) {
        free(s_postBody);
        s_postBody = nullptr;
        s_postLen = 0;
    }
    s_postBody = (char *)heap_caps_malloc(bodyLen, MALLOC_CAP_SPIRAM);
    if (!s_postBody)
        return false;
    memcpy(s_postBody, body, bodyLen);
    s_postLen = bodyLen;
    snprintf(s_postType, sizeof(s_postType), "%s", contentType ? contentType : "application/json");
    if (!tdeck_net_fetch(url)) {
        LOG_INFO("net: POST rejected, state=%d pending=%d wifi_ssid=%s", (int)s_state, (int)s_pending,
                 config.network.wifi_ssid[0] ? "set" : "missing");
        free(s_postBody);
        s_postBody = nullptr;
        s_postLen = 0;
        return false;
    }
    LOG_INFO("net: POST queued, bytes=%u", (unsigned)bodyLen);
    return true;
}

// Queue a POST by transferring ownership of an already PSRAM-backed body. Used for long
// microphone recordings so the complete WAV does not need another 3.8 MB copy.
extern "C" bool tdeck_net_post_bytes_owned(const char *url, uint8_t *body, size_t bodyLen,
                                            const char *contentType)
{
    if (!url || !body || !bodyLen || s_postBody) {
        LOG_INFO("net: owned POST rejected, empty or busy");
        return false;
    }
    s_postBody = (char *)body;
    s_postLen = bodyLen;
    snprintf(s_postType, sizeof(s_postType), "%s", contentType ? contentType : "application/json");
    if (!tdeck_net_fetch(url)) {
        s_postBody = nullptr;
        s_postLen = 0;
        return false; // caller still owns body on rejection
    }
    LOG_INFO("net: owned POST queued, bytes=%u", (unsigned)bodyLen);
    return true; // network task owns body now
}

extern "C" bool tdeck_net_post(const char *url, const char *body, const char *contentType)
{
    if (!body)
        return false;
    return tdeck_net_post_bytes(url, (const uint8_t *)body, strlen(body), contentType);
}

extern "C" bool tdeck_net_fetch(const char *url)
{
#if HAS_WIFI
    if (!url || !url[0])
        return false;
    if (s_pending || s_state == NET_START || s_state == NET_CONNECTING || s_state == NET_FETCH)
        return false; // busy
    if (config.network.wifi_ssid[0] == 0)
        return false; // no saved network
    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = 0;
    s_pending = true; // handed to the main-loop service
    return true;
#else
    (void)url;
    return false;
#endif
}

// 0 idle, 1 working, 2 done, 3 error.
extern "C" int tdeck_net_poll(void)
{
    switch (s_state) {
    case NET_START:
    case NET_CONNECTING:
    case NET_FETCH:
        return 1;
    case NET_DONE:
        return 2;
    case NET_ERROR:
        return 3;
    default:
        return 0;
    }
}

// The RAW state, for callers that want to tell "joining wi-fi" from "waiting on the server".
// tdeck_net_poll() deliberately collapses those into "working", which is the right default -
// but a UI that shows one word for twenty seconds reads as a hang, so it can ask for more.
// Returns the NetState values: 0 idle, 1 start, 2 connecting, 3 fetch, 4 done, 5 error.
extern "C" int tdeck_net_state(void)
{
    return s_state;
}

// Copy the body when done; returns bytes copied (0 if not done). Reading it returns the
// door to idle so the next fetch can start. Only safe to read once NET_DONE, which the
// service sets AFTER the body is fully written — so no torn reads.
extern "C" int tdeck_net_result(char *buf, int cap)
{
    if (s_state != NET_DONE || !buf || cap <= 0)
        return 0;
    int n = s_bodyLen;
    if (n > cap - 1)
        n = cap - 1;
#if HAS_WIFI
    if (s_body && n > 0)
        memcpy(buf, s_body, n);
#endif
    buf[n] = 0;
#if HAS_WIFI
    if (s_body) {
        heap_caps_free(s_body);
        s_body = nullptr;
    }
#endif
    // The bearer token is needed only while the HTTP request is active. Do not leave it in the
    // shared network door after the response has been consumed.
    memset(s_authHeader, 0, sizeof(s_authHeader));
    s_bodyLen = 0;
    s_state = NET_IDLE;
    return n;
}

// Clear an error/done state without reading (so the app can retry).
extern "C" int tdeck_net_http_code(void)
{
    return s_httpCode;
}

extern "C" void tdeck_net_reset(void)
{
#if HAS_WIFI
    if (s_state == NET_DONE || s_state == NET_ERROR) {
        if (s_body) {
            heap_caps_free(s_body);
            s_body = nullptr;
        }
        s_bodyLen = 0;
        s_state = NET_IDLE;
    }
    memset(s_authHeader, 0, sizeof(s_authHeader));
#endif
}

// ---- main-loop service (the safe thread) -------------------------------------
#if HAS_WIFI
static WiFiClientSecure *s_client = nullptr;

// Blocking HTTPS GET into a fresh PSRAM buffer, run ONLY once Wi-Fi is already up and
// with short timeouts, so it can't wedge the loop for long. Mirrors getappsGet.
static bool netHttpGet(const char *url)
{
    LOG_INFO("net: request begin %s", url ? url : "(null)");
    // Hand the TLS handshake the contiguous block reserved at boot. This is the SAME mbedTLS
    // -32512 failure that broke Get Apps, and it bit here too: the reserve was added for the
    // Get Apps download path in TFTView and this second, separate download path -- the one every
    // Lua app uses -- was left without it. Get Apps worked, the Weather app starved.
    // If a fetch path does TLS, it releases the reserve first. There are only these two.
    tdeck_tls_reserve_release();
    if (!s_client) {
        s_client = new WiFiClientSecure();
        s_client->setInsecure(); // public, read-only APIs; no room for a CA bundle
    // ⛔ 15-SECOND HANDSHAKE CAP. The library default is 120 SECONDS, and the task watchdog fires
    // at ~90: a TLS handshake that stalls on a weak wi-fi link was rebooting the device before the
    // library gave up. Caught 2026-09-29 - the log went silent at "net: GET api.open-meteo.com"
    // (the lock-screen weather refresh) and the watchdog reset the device 77 seconds later.
        s_client->setHandshakeTimeout(15);
    }
    HTTPClient http;
    http.setConnectTimeout(6000);
    http.setTimeout(6000);
    http.setUserAgent("t-ui-tdeck"); // some APIs reject a blank/odd user-agent
    {
        // ⛔ NEVER LOG A KEY. Gemini carries the user's API key in the query string, and this line
        // printed the whole URL to the USB log - found 2026-09-29 reading a capture. Everything
        // from "key=" to the next '&' is masked; the rest of the URL is still worth seeing.
        char shown[160];
        snprintf(shown, sizeof(shown), "%s", url);
        char *k = strstr(shown, "key=");
        if (k) {
            char *v = k + 4;
            char *amp = strchr(v, '&');
            const char *tail = amp ? amp : "";
            char rest[64];
            snprintf(rest, sizeof(rest), "%s", tail);
            snprintf(v, sizeof(shown) - (v - shown), "***%s", rest);
        }
        LOG_INFO("net: GET %s", shown);
    }
    if (!http.begin(*s_client, url)) {
        LOG_INFO("net: http.begin() failed");
        if (s_postBody) {
            free(s_postBody);
            s_postBody = nullptr;
            s_postLen = 0;
        }
        return false;
    }
    if (s_authHeader[0])
        http.addHeader("Authorization", s_authHeader);
    int code;
    if (s_postBody) {
        http.addHeader("Content-Type", s_postType);
        http.setTimeout(20000); // Gemini takes a few seconds to think; 6s would time out mid-answer
        LOG_INFO("net: POST %d bytes", (int)s_postLen);
        code = http.POST((uint8_t *)s_postBody, s_postLen);
        free(s_postBody);
        s_postBody = nullptr;
        s_postLen = 0;
    } else {
        code = http.GET();
    }
    if (code != HTTP_CODE_OK) {
        // The BODY of a failed request is worth keeping: Gemini explains a rejected key or a
        // retired model in it, and "it failed" alone would leave that unfixable from the device.
        String err = http.getString();
        LOG_INFO("net: request failed, code=%d body=%.120s", code, err.c_str());
        s_httpCode = code;
        http.end();
        return false;
    }
    s_httpCode = code;
    // Read the body. Open-Meteo (and many APIs) send it CHUNKED with no Content-Length header,
    // so http.getSize() returns -1 — that's WHY the first version failed, it treated -1 as empty.
    // getString() reads and de-chunks the whole body cleanly; our JSON responses are small.
    (void)http.getSize();
    String body = http.getString();
    http.end();
    int got = (int)body.length();
    LOG_INFO("net: read %d bytes", got);
    if (got <= 0 || got > kNetMaxBody)
        return false;
    char *buf = (char *)heap_caps_malloc(got + 1, MALLOC_CAP_SPIRAM);
    if (!buf)
        return false;
    memcpy(buf, body.c_str(), (size_t)got);
    buf[got] = 0;
    if (s_body)
        heap_caps_free(s_body);
    s_body = buf;
    s_bodyLen = got;
    return true;
}
#endif

extern "C" void tdeck_net_service(void)
{
#if HAS_WIFI
    // Drop a connection WE opened once nothing has needed it for a while. Never touches a
    // connection someone else owns.
    if (s_dropAt && (int32_t)(millis() - s_dropAt) > 0 && s_state != NET_START && s_state != NET_CONNECTING &&
        s_state != NET_FETCH) {
        s_dropAt = 0;
        if (s_ownWifi) {
            LOG_INFO("net: dropping the wi-fi we brought up");
            tdeck_wifi_disconnect_now();
            s_ownWifi = false;
        }
    }

    // ⭐ KEEP TRYING TO GET THE RESERVE BACK, not just once at the end of a fetch.
    //
    // MEASURED 2026-09-25: the first HTTPS request after boot works, and every one after it fails
    // with code=-1. The reserve is taken at boot when internal RAM is clean (36,864 bytes), handed
    // to the handshake, and then re-taken the moment the TLS client is deleted - which is the
    // right place and is not enough. By then the user has opened Maps or chess or Gemini, the
    // largest internal block has fallen from 34,804 to 27,636, the whole {36,32,28}KB ladder
    // misses, and nothing ever tries again. One HTTPS request per boot, silently.
    //
    // A handshake needs ~34KB contiguous, so a smaller reserve would not help; what helps is
    // ASKING AGAIN LATER. Closing Maps or chess frees exactly the kind of block this wants, and
    // this grabs it within a few seconds of it appearing. Costs one failed malloc every 5s in the
    // worst case, and nothing at all once it succeeds.
    {
        static uint32_t s_nextTake = 0;
        const bool idle = (s_state != NET_START && s_state != NET_CONNECTING && s_state != NET_FETCH);
        if (idle && !s_tlsLentOut && (!s_tlsReserve[0] || !s_tlsReserve[1]) && (int32_t)(millis() - s_nextTake) > 0) {
            s_nextTake = millis() + 5000;
            const size_t before = s_tlsReserveBytes;
            tdeck_tls_reserve_init();
            if (s_tlsReserveBytes != before)
                LOG_INFO("tls reserve: now holding %u bytes in %d/%d blocks (largest free %u)",
                         (unsigned)s_tlsReserveBytes, (s_tlsReserve[0] ? 1 : 0) + (s_tlsReserve[1] ? 1 : 0),
                         kTlsBlocks, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
    }

    if (s_pending && (s_state == NET_IDLE || s_state == NET_DONE || s_state == NET_ERROR)) {
        s_pending = false;
        s_state = NET_START;
    }

    switch (s_state) {
    case NET_START: {
        // Wi-Fi and BT can't share the radio here — drop BT first, but only ONCE. Calling
        // deinit() over and over on an already-down stack is needless and a place double-frees
        // could hide, so latch it. BT stays down until the next reboot regardless.
        static bool s_btDown = false;
        if (!s_btDown) {
            disableBluetooth();
            s_btDown = true;
        }
        // Already connected? Use it. This is the common case -- wi-fi set up in Settings means
        // the device joins at boot -- and it turns a ~10s round trip into an immediate one.
        // Deliberately does NOT clear s_ownWifi: if this is our own connection still inside its
        // grace period, we must stay responsible for closing it.
        if (tdeck_wifi_connected()) {
            LOG_INFO("net: wi-fi already up, reusing it");
            s_dropAt = 0; // another fetch arrived; cancel any pending shutdown
            s_state = NET_FETCH;
            break;
        }
        LOG_INFO("net: START, firmware wifi ssid='%s'", config.network.wifi_ssid);
        if (!tdeck_wifi_connect_now(config.network.wifi_ssid, config.network.wifi_psk)) {
            LOG_INFO("net: connect_now failed (no ssid on the firmware side?)");
            s_state = NET_ERROR;
            break;
        }
        s_ownWifi = true;              // we opened it, so we close it
        s_deadline = millis() + 15000; // give the join up to 15s
        s_state = NET_CONNECTING;
        break;
    }
    case NET_CONNECTING: {
        if (tdeck_wifi_connected()) {
            LOG_INFO("net: wifi connected");
            delay(500); // let DHCP/DNS settle before the first TLS connect
            s_state = NET_FETCH;
        } else if ((int32_t)(millis() - s_deadline) > 0) {
            LOG_INFO("net: wifi connect TIMED OUT after 15s");
            tdeck_wifi_disconnect_now();
            s_state = NET_ERROR;
        }
        break;
    }
    case NET_FETCH: {
        bool ok = netHttpGet(s_url);
        // Only close what we opened, and not immediately: an app that fetches twice in a row
        // (Weather asks where you are, then what the weather is) would otherwise pay for a full
        // reconnect in between. Hold it briefly instead; the grace period below drops it.
        if (s_ownWifi)
            s_dropAt = millis() + 20000;
        s_state = ok ? NET_DONE : NET_ERROR;
        LOG_INFO("net: result = %s", ok ? "DONE" : "FAILED");
        if (!ok)
            tdeck_tls_report_alloc_fail(); // say WHICH allocation failed, not just that it did
        // Drop the secure client and reclaim the reserve for the next fetch. Deleting it first
        // matters: the handshake buffers are held BY the client, so re-reserving before it's gone
        // would just fail and leave the following fetch to starve the same way.
        if (s_client) {
            delete s_client;
            s_client = nullptr;
        }
        tdeck_tls_reserve_take();
        break;
    }
    default:
        break;
    }
#endif
}
