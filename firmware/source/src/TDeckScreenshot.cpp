// -----------------------------------------------------------------------------
// TDeckScreenshot - save what is on the screen to the SD card.
//
// Jake, 2026-09-18: "debug: screen shot thar we can both trigger. saves to sd".
// The point is that he can show me exactly what he is looking at, instead of
// describing it, and that a bug report can carry a picture.
//
// HOW IT CAPTURES. Not by reading pixels back off the panel: the ST7789 shares its
// SPI bus with the SD card and the radio, readback there is unreliable, and holding
// that bus is what has been stalling the radio and rebooting the device. Instead the
// LVGL flush callback - which already has the finished pixels in its hand on their way
// to the glass - copies them into a PSRAM buffer. That also means the shot contains
// EXACTLY what is displayed, overlays and pop-ups included, which a per-screen snapshot
// would miss.
//
// HOW IT WRITES. Not from the flush callback. That runs on the "tft" task, mid-refresh, and
// tftSetup.cpp holds the SPI LOCK across that whole task, so doing anything slow there holds
// the bus the radio is waiting for. The callback only fills the buffer and sets a flag;
// tdeck_shot_service() on the MAIN loop does the writing.
//
// WHERE IT WRITES - Jake, 2026-09-19: "could we have the screenshot save to the internal
// memory instead so i dont need to plug int he card everytime?"
//
// So: INTERNAL FLASH (LittleFS / FSCom), not the SD card. Two happy consequences beyond the
// obvious one. It works with no card in the device at all. And internal flash is NOT on the
// shared SPI bus that the display, the card and the radio fight over - it has its own flash
// controller - so this no longer needs spiLock and can never stall the radio.
//
// The partition is 0x360000 (3.5 MB), shared with Meshtastic's own /prefs (config plus a node
// database that runs to six figures of bytes with 248 nodes). A 24-bit 320x240 BMP is 230 KB,
// so this keeps the newest kKeep and deletes the rest, and refuses outright if free space
// looks tight. Losing somebody's node database to make room for a screenshot would be a
// genuinely bad trade.
//
// Getting them off: `esptool read_flash 0xc90000 0x360000 fs.bin`, then read the image with
// littlefs-python (already installed in the penv). No Wi-Fi, no card, no new protocol.
//
// 24-bit BMP is bigger than it needs to be, but every computer and phone opens it without
// thinking, which is the whole point of a screenshot you intend to send to somebody.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include <Arduino.h> // millis()
#include <cstdio>
#include <cstring>

#if !defined(ARCH_PORTDUINO)
#include "FSCommon.h" // FSCom = LittleFS on internal flash
#include <esp_heap_caps.h>
#define SHOT_HAVE_FS 1
#else
#define SHOT_HAVE_FS 0
#endif

// Keep this many. Four 230 KB screenshots is under a megabyte of a 3.5 MB partition, which
// leaves plenty for the node database, and four is more than any bug report needs.
static const int kKeep = 4;
// Refuse below this much free space, so a screenshot can never be the thing that stops
// Meshtastic saving its nodes.
static const uint32_t kMinFreeBytes = 600u * 1024u;

static const int kShotW = 320;
static const int kShotH = 240;

static uint16_t *s_buf = nullptr;      // kShotW * kShotH RGB565, PSRAM
static volatile bool s_capturing = false;
static uint32_t s_armedAtMs = 0; // for the watchdog below
static uint32_t s_covered = 0;   // pixels written since arming, for the coverage test
static volatile bool s_ready = false;  // buffer full, waiting to be written
static volatile bool s_failed = false; // could not allocate, or could not write
static char s_lastPath[32] = {0};

extern "C" bool tdeck_shot_capturing(void)
{
    return s_capturing;
}

extern "C" const char *tdeck_shot_last_path(void)
{
    return s_lastPath;
}

extern "C" bool tdeck_shot_failed(void)
{
    return s_failed;
}

// Called from the UI once the countdown expires. Allocates on first use and keeps the
// buffer: 150 KB of PSRAM held for the life of the session is a fair price for not
// risking an allocation failure at the moment somebody is trying to capture a bug.
// ⛔ REFUSE A SCREENSHOT WHILE A SECRET IS ON SCREEN.
// @@shot streams the framebuffer over USB. The Mail setup form holds a Gmail app password, and
// I have been screenshotting this device all session to verify work - one taken at the wrong
// moment would put the password in a PNG on a PC and in a conversation log. Relying on
// remembering is not a control; refusing is. Set while the form is up, cleared when it closes.
static volatile bool s_shotBlocked = false;

extern "C" void tdeck_shot_block(bool on)
{
    s_shotBlocked = on;
}

// Returns whether a capture was actually armed. ⛔ THE CALLER MUST REPORT THIS. It used to
// return void and the remote handler replied "shot armed" unconditionally, so a REFUSED
// screenshot - including one refused because a password is on screen - looked like a success,
// and only surfaced later as "no capture in memory". A guard that reports success when it
// blocks something is worse than no guard: it teaches you to trust the wrong message.
extern "C" bool tdeck_shot_begin(void)
{
    if (s_shotBlocked) {
        LOG_INFO("@@err shot refused - a password field is on screen");
        return false;
    }
#if SHOT_HAVE_FS
    if (s_capturing || s_ready)
        return false;

    // ⚠️ REFUSE WHEN INTERNAL RAM IS TIGHT. 2026-09-19: Jake turned Wi-Fi on, and a
    // screenshot stream while Wi-Fi was negotiating took the device down -
    // "last restart=CRASH | prev run low: fast=0k". The capture buffer itself is PSRAM,
    // but Wi-Fi is a heavy user of INTERNAL heap and a screenshot is never worth a crash.
    // Say no and say why instead.
    // 10KB, not 24KB. The first guess was calibrated against the Wi-Fi crash, but this
    // device sits at ~15KB free INTERNAL in ordinary use once MUI has built panels for
    // 179 nodes - so 24KB refused every screenshot on an otherwise healthy device. What a
    // capture actually costs internally is almost nothing: the 150KB buffer is PSRAM, and
    // the stream uses a 96-byte stack buffer and the shared print buffer. 10KB is the
    // genuine near-death line, not a comfortable margin.
    const size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (freeInternal < 10 * 1024) {
        LOG_INFO("@@err internal heap only %u bytes free - refusing screenshot (Wi-Fi on?)",
                 (unsigned)freeInternal);
        s_failed = true;
        return false;
    }
    if (!s_buf) {
        s_buf = (uint16_t *)heap_caps_malloc((size_t)kShotW * kShotH * 2, MALLOC_CAP_SPIRAM);
        if (!s_buf) {
            s_failed = true;
            return false;
        }
    }
    memset(s_buf, 0, (size_t)kShotW * kShotH * 2);
    s_failed = false;
    s_armedAtMs = millis();
    s_covered = 0;
    s_capturing = true;
    return true;
#else
    return false; // no filesystem, no screenshots
#endif
}

// Called from the LVGL flush callback for every area, BEFORE the byte swap that the
// panel wants - so what lands here is plain native RGB565.
//
// Areas are clipped rather than assumed to be in range: a rounder callback on some
// panels widens them, and LVGL is entitled to flush an area larger than it was asked to.
extern "C" void tdeck_shot_capture_area(int x1, int y1, int x2, int y2, const uint16_t *px)
{
#if SHOT_HAVE_FS
    if (!s_capturing || !s_buf || !px)
        return;
    const int aw = x2 - x1 + 1;
    for (int y = y1; y <= y2; y++) {
        if (y < 0 || y >= kShotH)
            continue;
        for (int x = x1; x <= x2; x++) {
            if (x < 0 || x >= kShotW)
                continue;
            s_buf[y * kShotW + x] = px[(y - y1) * aw + (x - x1)];
            s_covered++;
        }
    }
#else
    (void)x1; (void)y1; (void)x2; (void)y2; (void)px;
#endif
}

// The last area of a refresh has gone past.
//
// ⚠️ "A refresh finished" is NOT "the whole screen has been captured". LVGL only
// redraws what changed, and a clock label ticking over is a refresh of its own. Arming a
// capture and then invalidating the screen leaves a window in which some tiny in-flight
// refresh completes first - and the shot came out almost entirely black with a sliver of
// clock, which is exactly what happened when paging the launcher.
//
// So finishing requires COVERAGE: essentially every pixel written since arming. Anything
// less and we keep waiting for the full redraw that the invalidate asked for. The 2s
// watchdog in the service is still the backstop if that never arrives.
extern "C" void tdeck_shot_frame_done(void)
{
    if (!s_capturing)
        return;
    if (s_covered < (uint32_t)(kShotW * kShotH) - 64) // a hair of slack for clipped edges
        return;
    s_capturing = false;
    s_ready = true;
}

#if SHOT_HAVE_FS
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static bool writeBmp(const char *path)
{
    const int rowBytes = kShotW * 3;
    const int pad = (4 - (rowBytes % 4)) % 4; // BMP rows are padded to 4 bytes
    const uint32_t dataLen = (uint32_t)(rowBytes + pad) * kShotH;

    File f = FSCom.open(path, FILE_O_WRITE);
    if (!f)
        return false;

    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(hdr + 2, 54 + dataLen); // file size
    put32(hdr + 10, 54);          // pixel data offset
    put32(hdr + 14, 40);          // DIB header size
    put32(hdr + 18, kShotW);
    put32(hdr + 22, kShotH); // positive height = bottom-up, which is BMP's default
    hdr[26] = 1;             // planes
    hdr[28] = 24;            // bits per pixel
    put32(hdr + 34, dataLen);
    put32(hdr + 38, 2835); // 72 dpi, in pixels per metre
    put32(hdr + 42, 2835);
    if (f.write(hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
        f.close();
        return false;
    }

    // One row at a time: a second full-size buffer for the 24-bit copy would cost another
    // 230 KB for no reason. Bottom-up, because that is the direction BMP stores rows.
    static uint8_t row[kShotW * 3 + 4];
    for (int y = kShotH - 1; y >= 0; y--) {
        const uint16_t *src = s_buf + (size_t)y * kShotW;
        uint8_t *d = row;
        for (int x = 0; x < kShotW; x++) {
            const uint16_t c = src[x];
            // RGB565 -> 8 bits per channel. The high bits are repeated into the low ones so
            // that full-scale stays full-scale (0x1f -> 0xff, not 0xf8).
            const uint8_t r = (uint8_t)(((c >> 11) & 0x1f) * 255 / 31);
            const uint8_t g = (uint8_t)(((c >> 5) & 0x3f) * 255 / 63);
            const uint8_t b = (uint8_t)((c & 0x1f) * 255 / 31);
            *d++ = b; // BMP stores BGR
            *d++ = g;
            *d++ = r;
        }
        for (int p = 0; p < pad; p++)
            *d++ = 0;
        if (f.write(row, rowBytes + pad) != rowBytes + pad) {
            f.close();
            return false;
        }
    }
    f.close();
    return true;
}
#endif

#if SHOT_HAVE_FS
// Keep /shots down to kKeep files. Numbers only ever go up, so the lowest-numbered file is
// the oldest - no directory timestamps needed, which LittleFS does not give us anyway.
static void pruneShots(void)
{
    int present[1000];
    int count = 0;
    File dir = FSCom.open("/shots", FILE_O_READ);
    if (!dir)
        return;
    for (File e = dir.openNextFile(); e && count < 1000; e = dir.openNextFile()) {
        const char *nm = e.name();
        int idx = 0;
        // e.name() is sometimes the bare name and sometimes the full path, depending on the
        // core version. Take whatever follows the last slash and parse that.
        const char *slash = strrchr(nm, '/');
        if (slash)
            nm = slash + 1;
        if (sscanf(nm, "shot%d.bmp", &idx) == 1)
            present[count++] = idx;
        e.close();
    }
    dir.close();
    if (count <= kKeep)
        return;
    // Simple selection: repeatedly remove the smallest index until we are at the cap.
    for (int gone = 0; gone < count - kKeep; gone++) {
        int lo = -1, at = -1;
        for (int i = 0; i < count; i++)
            if (present[i] >= 0 && (lo < 0 || present[i] < lo)) {
                lo = present[i];
                at = i;
            }
        if (at < 0)
            break;
        present[at] = -1;
        char p[32];
        snprintf(p, sizeof(p), "/shots/shot%03d.bmp", lo);
        FSCom.remove(p);
    }
}
#endif

// ---------------------------------------------------------------------------
// Streaming a shot back over the USB command channel.
//
// The obvious way to fetch one was `esptool read_flash` + littlefs-python, and it works
// - but esptool puts the chip into download mode to do it, which REBOOTS the device. It
// also truncated a screenshot to 0 bytes by resetting mid-write. Rebooting Jake's device
// every time I want to look at the screen is not acceptable.
//
// So: stream the PSRAM capture buffer straight out, run-length encoded and base64'd. UI
// screens are mostly flat colour, so RLE takes 76,800 pixels down to a few thousand pairs
// - a couple of seconds at 115200 instead of a minute, and nothing restarts.
//
// ⚠️ EMITTED IN SMALL SLICES FROM THE MAIN LOOP, never in one go. Printing 30KB in a
// single call would block, and on the "tft" task it would hold the SPI lock the radio
// needs. A few hundred pairs per loop iteration keeps every call short.
// ---------------------------------------------------------------------------
static bool s_streaming = false;
static int s_streamAt = 0; // pixel index reached so far

extern "C" void tdeck_shot_stream_begin(void)
{
#if SHOT_HAVE_FS
    if (!s_buf) {
        LOG_INFO("@@err no capture in memory - take a shot first");
        return;
    }
    const size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (freeInternal < 8 * 1024) {
        LOG_INFO("@@err internal heap only %u bytes free - refusing to stream",
                 (unsigned)freeInternal);
        return;
    }
    s_streaming = true;
    s_streamAt = 0;
    LOG_INFO("@@img %d %d rle16", kShotW, kShotH);
#endif
}

#if SHOT_HAVE_FS
static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// One slice: encode runs until the output line is full, print it, return.
static void streamSlice(void)
{
    const int total = kShotW * kShotH;
    // ⚠️ SIZED TO FIT THE LOG LINE. RedirectablePrint has a static printBuf[160] and
    // silently truncates past it - at 192 bytes the base64 came out 256 chars, arrived
    // cut to 154, and the decoder choked on a half-finished base64 group. 96 bytes is
    // 128 characters of base64 plus the "@@d " prefix: comfortably inside 160.
    uint8_t raw[96]; // 4 bytes per run (count16, value16) -> up to 24 runs a line
    int n = 0;
    while (s_streamAt < total && n + 4 <= (int)sizeof(raw)) {
        const uint16_t v = s_buf[s_streamAt];
        int run = 1;
        while (s_streamAt + run < total && s_buf[s_streamAt + run] == v && run < 65535)
            run++;
        raw[n++] = (uint8_t)(run & 0xff);
        raw[n++] = (uint8_t)(run >> 8);
        raw[n++] = (uint8_t)(v & 0xff);
        raw[n++] = (uint8_t)(v >> 8);
        s_streamAt += run;
    }
    if (n == 0) {
        s_streaming = false;
        LOG_INFO("@@imgend");
        return;
    }
    char line[(sizeof(raw) / 3 + 2) * 4 + 8];
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        const uint32_t b0 = raw[i];
        const uint32_t b1 = (i + 1 < n) ? raw[i + 1] : 0;
        const uint32_t b2 = (i + 2 < n) ? raw[i + 2] : 0;
        const uint32_t t = (b0 << 16) | (b1 << 8) | b2;
        line[o++] = kB64[(t >> 18) & 63];
        line[o++] = kB64[(t >> 12) & 63];
        line[o++] = (i + 1 < n) ? kB64[(t >> 6) & 63] : '=';
        line[o++] = (i + 2 < n) ? kB64[t & 63] : '=';
    }
    line[o] = 0;
    LOG_INFO("@@d %s", line);
    if (s_streamAt >= total) {
        s_streaming = false;
        LOG_INFO("@@imgend");
    }
}
#endif

// Main loop. Writes the pending shot, if there is one.
extern "C" void tdeck_shot_service(void)
{
#if SHOT_HAVE_FS
    if (s_streaming) {
        streamSlice();
        return; // one slice per loop iteration; never block
    }
    // Watchdog. The capture completes when a whole frame has been flushed past; if for any
    // reason one never is, s_capturing would stay set and every later screenshot would be
    // refused by the guard in tdeck_shot_begin(). Two seconds is many frames.
    if (s_capturing && (millis() - s_armedAtMs) > 2000) {
        s_capturing = false;
        s_failed = true;
        return;
    }
    if (!s_ready)
        return;
    s_ready = false;

    // No spiLock here, deliberately: internal flash has its own controller and is not on the
    // bus the display, the card and the radio share. That is a real advantage of moving off
    // the SD card, not an oversight.
    FSCom.mkdir("/shots");
    pruneShots();

    const uint32_t total = FSCom.totalBytes();
    const uint32_t used = FSCom.usedBytes();
    if (total > used && (total - used) < kMinFreeBytes) {
        // Say so rather than half-writing a file and leaving the filesystem full.
        snprintf(s_lastPath, sizeof(s_lastPath), "%s", "");
        s_failed = true;
        return;
    }

    char path[32];
    int n = 1;
    for (; n < 1000; n++) {
        snprintf(path, sizeof(path), "/shots/shot%03d.bmp", n);
        if (!FSCom.exists(path))
            break;
    }
    if (n >= 1000) {
        s_failed = true;
        return;
    }
    if (writeBmp(path)) {
        snprintf(s_lastPath, sizeof(s_lastPath), "%s", path);
        s_failed = false;
    } else {
        FSCom.remove(path); // never leave a truncated file behind
        s_failed = true;
    }
#endif
}
