#include "graphics/map/URLService.h"
#include "graphics/map/MapTileSettings.h"
#include "graphics/map/TileProvider.h"
#include "lvgl.h"
#include "util/ILog.h"

#ifdef ARDUINO_ARCH_ESP32

#include "HTTPClient.h" // not available on Linux/Portduino
#include "WiFi.h"
#include "WiFiClientSecure.h"

// from ConvertPNG.c
extern "C" {
bool decodeImgGrey(const void *data, size_t size, lv_img_dsc_t **img);
bool decodeImgColor(const void *data, size_t size, lv_img_dsc_t **img);
}

// ⭐ ONE HTTPClient AND ONE TLS CONNECTION, KEPT - but only while tiles are actually coming in.
// Every tile used to build a fresh HTTPClient, whose destructor hangs up, so every tile paid for a
// whole TLS handshake on the UI task. Same host each time, so HTTPClient keeps the line open (see
// mapdlHttp in TFTView_320x240.cpp). ⛔ An open TLS line holds ~40KB of INTERNAL RAM - the 2026-09-30
// stress test watched free internal RAM fall from 73KB to 30KB - which Mail, Get Apps and the
// node-DB save need too. So it hangs up after a few idle seconds and whenever Maps closes:
// tdeck_url_tiles_hangup().
static HTTPClient s_http;
static WiFiClientSecure s_secureClient;
static WiFiClient s_plainClient;
static uint32_t s_lastUse = 0; // lv_tick of the last fetch; 0 = line closed

extern "C" void tdeck_url_tiles_hangup(bool onlyIfIdle)
{
    if (!s_lastUse)
        return;
    if (onlyIfIdle && lv_tick_get() - s_lastUse < 4000)
        return;
    s_http.end();
    s_secureClient.stop(); // frees the TLS buffers
    s_plainClient.stop();
    s_lastUse = 0;
}

URLService::URLService(Callback cb) : ITileService("HTTP:"), saveCB(cb) {}

URLService::~URLService() {}

bool URLService::load(const char *name, void *img)
{
    HTTPClient &http = s_http; // kept between tiles - see the note at the top
    WiFiClientSecure &secureClient = s_secureClient;
    WiFiClient &plainClient = s_plainClient;

    if (WiFi.status() != WL_CONNECTED) {
        ILOG_DEBUG("URLService::load skipped (WiFi not connected)");
        return false;
    }

    // Browse-fill runs on the UI task, so a fetch is a screen freeze for its whole
    // duration (TLS handshake + GET). It's a bonus feature, never worth wrecking the
    // panning feel: skip while any input is held down (mid-drag/mid-press) and allow
    // at most ~2 attempts a second. Skipped tiles show the no-tile image and
    // MapPanel's retry sweep picks them up once the user goes idle.
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        if (lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED)
            return false;
    }
    // ⛔ DO NOT KEEP ASKING FOR A ZOOM LEVEL THE SERVER DOES NOT HAVE. USGS stops at z16; past that
    // every visible square tried an HTTPS fetch - a TLS handshake ON THE UI TASK - and got 4xx,
    // over and over as you panned. That is a large part of "maps gets slow at max zoom" (Jake,
    // 2026-09-29). Three 4xx answers in a row at one zoom and that zoom is left alone for the rest
    // of the session; any success resets the count, so one missing square in a covered area does
    // not switch a real zoom level off. Memory or network failures never count.
    static uint8_t s_zoomMisses[32] = {0};
    int zoom = -1;
    if (name) { // ".../<z>/<x>/<y>.<ext>" - the third number from the end
        const char *slash[3] = {nullptr, nullptr, nullptr};
        for (const char *p = name; *p; p++)
            if (*p == '/') {
                slash[0] = slash[1];
                slash[1] = slash[2];
                slash[2] = p;
            }
        if (slash[0] && slash[0][1] >= '0' && slash[0][1] <= '9')
            zoom = atoi(slash[0] + 1);
    }
    if (zoom >= 0 && zoom < 32 && s_zoomMisses[zoom] >= 3)
        return false;

    // ⛔ AT MOST ONE FETCH PER 500ms, MEASURED FROM WHEN THE LAST ONE ENDED. Measured from when it
    // started, a fetch that took 3s left the gate open, so one redraw with a dozen missing squares
    // fetched them all back to back: the screen froze for 15-46s at a time in the 2026-09-30 stress
    // test (map left over Seattle, no tiles on the card, a slow USGS server) - half the watchdog.
    // Now one fetch, then the screen gets its turn; MapPanel's retry sweep fetches the rest.
    static uint32_t s_lastAttempt = 0;
    uint32_t now = lv_tick_get();
    if (now - s_lastAttempt < 500)
        return false;
    struct AttemptEnd {
        ~AttemptEnd() { s_lastAttempt = s_lastUse = lv_tick_get(); }
    } attemptEnd;

    struct LvFreeGuard {
        uint8_t *&ptr;
        ~LvFreeGuard() { lv_free(ptr); }
    };

    // transform filename to provider url
    std::string url = TileProvider::url(name);
    if (url.empty()) {
        ILOG_ERROR("empty URL for tile %s", name ? name : "(null)");
        return false;
    }

    http.setReuse(true);
    // hard caps: a dead server may cost at most ~2.5s of UI, not the 5s+ defaults
    http.setConnectTimeout(2500);
    http.setTimeout(2500);
    const bool tls = strncmp(url.c_str(), "https", 5) == 0;
    WiFiClient &line = tls ? (WiFiClient &)secureClient : plainClient;
    // Anything short of a whole tile hangs up, so the next reply is never read from the middle of
    // this one's leftovers.
    auto hangUp = [&]() {
        http.end();
        line.stop();
    };
    bool began;
    if (tls) {
        // https tile servers (Google, USGS): TLS without cert pinning — it's public
        // map data, and the S3 has no room for a CA bundle here.
        secureClient.setInsecure();
        secureClient.setHandshakeTimeout(6); // on the UI task: 6s, not 15 (the default 120s outlives the watchdog)
        began = http.begin(secureClient, url.c_str());
    } else {
        began = http.begin(plainClient, url.c_str());
    }
    if (!began) {
        ILOG_ERROR("ERROR begin %s", url.c_str());
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        ILOG_ERROR("ERROR GET %s : %d", url.c_str(), httpCode);
        hangUp();
        if (httpCode >= 400 && httpCode < 500 && zoom >= 0 && zoom < 32 && s_zoomMisses[zoom] < 3 &&
            ++s_zoomMisses[zoom] == 3)
            ILOG_INFO("tile server has nothing at z%d - not asking again this session", zoom);
        return false;
    }
    if (zoom >= 0 && zoom < 32)
        s_zoomMisses[zoom] = 0;

    WiFiClient *stream = http.getStreamPtr();
    int contentLen = http.getSize();
    if (contentLen <= 0) {
        ILOG_WARN("GET %s : empty", url.c_str());
        hangUp();
        return false;
    }

    size_t len = (size_t)contentLen;

    uint8_t *pngImage = (uint8_t *)lv_malloc(len);
    LvFreeGuard pngGuard{pngImage};
    if (!pngImage) {
        ILOG_ERROR("lv_malloc failed for %s (%u bytes)", url.c_str(), (unsigned int)len);
        hangUp();
        return false;
    }

    // read .png file in chunks to increase reliability (avoid readBytes())
    // Up to 1.5s of silence mid-tile. It was 15ms: a server a little slow to start the body got
    // hung up on after the handshake had already been paid for ("0 != 19052" in the stress log),
    // and the retry paid for the whole thing again.
    size_t bytesRead = 0;
    uint16_t idleSpins = 0;
    const uint16_t maxIdleSpins = 300;
    while (bytesRead < len) {
        size_t available = stream->available();
        if (available == 0) {
            if (++idleSpins > maxIdleSpins) {
                break;
            }
            delay(5);
            continue;
        }

        idleSpins = 0;
        size_t toRead = available;
        size_t remaining = len - bytesRead;
        if (toRead > remaining) {
            toRead = remaining;
        }

        int got = stream->read(pngImage + bytesRead, toRead);
        if (got <= 0) {
            break;
        }
        bytesRead += (size_t)got;
    }

    if (bytesRead != len) {
        ILOG_ERROR("http read error %s : %u != %u", url.c_str(), (unsigned int)bytesRead, (unsigned int)len);
        hangUp();
        return false;
    }
    http.end(); // the whole tile is in: the line stays open for the next one

    ILOG_DEBUG("SUCCESS(%d): GET %s (%u bytes)", (int)idleSpins, url.c_str(), (unsigned int)len);

    // save png tile to SD card
    if (saveCB && MapTileSettings::saveOK()) {
        bool result = saveCB(name, pngImage, len);
        ILOG_DEBUG("save png to SD -> %s", result ? "OK" : "failed");
    }

    // decode png via STBI library
    lv_img_dsc_t *img_dsc = nullptr;
    bool decoded = MapTileSettings::color() ? decodeImgColor(pngImage, len, &img_dsc) : decodeImgGrey(pngImage, len, &img_dsc);
    if (decoded) {
        lv_obj_t *img_obj = (lv_obj_t *)img;
        lv_image_set_src(img_obj, img_dsc);
        if (lv_image_get_src(img_obj) != img_dsc) {
            ILOG_ERROR("lv_image_set_src failed for tile %s", name);
            if (img_dsc->data && img_dsc->data_size > 0) {
                lv_free((void *)img_dsc->data);
            }
            lv_free(img_dsc);
            return false;
        }
    } else {
        ILOG_ERROR("Failed to decode tile image %s", name);
        return false;
    }

    return true;
}

#endif