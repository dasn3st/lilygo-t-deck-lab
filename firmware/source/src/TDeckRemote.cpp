// -----------------------------------------------------------------------------
// TDeckRemote - drive the device over the USB cable.
//
// Jake, 2026-09-19: "can you remotely control the device? Navigate it for me,
// screen cap etc?"
//
// WHY THIS IS ON USB AND NOT WI-FI. The obvious build was a little web server, but
// bringing Wi-Fi up on this hardware drops the Bluetooth link to his phone until the
// next reboot, and it would put an unauthenticated listener on his home network. The
// USB cable is already plugged in whenever I am working, costs him nothing, and can
// only be driven by something physically attached to the device.
//
// HOW IT SHARES THE PORT WITH MESHTASTIC. The USB serial carries Meshtastic's protobuf
// API. Its framing search (StreamAPI::readStream) throws away every byte that is not
// the 0x94 start marker and restarts - so plain text is already being discarded. This
// hooks exactly those discarded bytes, which means the two protocols cannot collide:
// anything we accept was, by definition, not part of a protobuf frame.
//
// Commands are one line, starting with "@@" so a stray newline is never mistaken for
// one. Replies come back as "@@ok ..." or "@@err ...", so a script can parse them out
// of the ordinary log chatter.
//
//   @@ping            - is anyone home
//   @@info            - uptime, active screen, free internal heap
//   @@mem             - full heap picture: free, largest block, all-time low
//   @@tap <x> <y>     - touch the screen at that point
//   @@swipe x1 y1 x2 y2 - drag, for paging the launcher and moving sliders
//   @@key <code>      - inject an LVGL keycode (see lv_keys)
//   @@home            - the Home gesture (trackball double-click)
//   @@back            - the Back gesture
//   @@open <screen>   - lock|settings|chats|nodes|alerts|notes|glance
//   @@shot            - take a screenshot to internal flash
//   @@get             - stream the last capture back (RLE+base64, ~2s)
//
// THREADING: the bytes arrive on whichever task runs the serial API, so this file only
// ever parses and queues. Everything that touches LVGL is executed by the UI poll timer
// on the "tft" task - see tdeck_remote_take(). That is the same rule as the rest of the
// launcher bridges; see the notes in TDeckPop.cpp.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include "TDeckMail.h"   // @@mail / @@inbox
#include "TDeckGemini.h" // @@gem - ask it headlessly, no unlock needed
#include "TDeckCoverage.h" // @@cov
#include "graphics/common/SdCard.h" // @@mailfile reads /gmail.txt
#include <lvgl.h> // @@refr needs the refresh timer
extern "C" void tdeck_fps_set(bool on); // @@fps - LGFXDriver.h
#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h> // uxTaskGetStackHighWaterMark, for the stack figures in @@mem

// Radio-to-UI packet queue (lib/.../comms/packet/PacketServer.cpp and src/mesh/api/PacketAPI.cpp):
// how deep it is now, how deep it has ever been, the cap, and what one entry costs. Reported on
// the @@mem line while the boot-time heap collapse is being chased.
extern "C" uint32_t tdeck_pktq_depth(void);
extern "C" uint32_t tdeck_pktq_peak(void);
extern "C" uint32_t tdeck_pktq_cap(void);
extern "C" uint32_t tdeck_pktq_itemsz(void);

static const int kMaxLine = 64;
static char s_line[kMaxLine];
static int s_len = 0;
static bool s_armed = false; // saw '@' '@' at the start of a line

// One pending command, parsed. The UI task picks it up.
static volatile int s_cmd = 0; // 0 none, 1 tap, 2 home, 3 back, 4 shot, 5 info, 6 ping
static volatile int s_x = 0, s_y = 0;
static volatile int s_x2 = 0, s_y2 = 0; // swipe end point
static char s_arg[48];                   // text argument (@@place <query>), copied before queuing

extern "C" void tdeck_shot_stream_begin(void); // src/TDeckScreenshot.cpp

// Fed one byte at a time from the protobuf framing search, for bytes it rejected.
extern "C" void tdeck_remote_feed(uint8_t c)
{
    if (c == '\r')
        return;
    if (c == '\n') {
        if (s_armed && s_len > 0) {
            s_line[s_len] = 0;
            // Parse here (cheap, no allocation); execute on the UI task.
            if (!strncmp(s_line, "ping", 4)) {
                s_cmd = 6;
            } else if (!strncmp(s_line, "info", 4)) {
                s_cmd = 5;
            } else if (!strncmp(s_line, "mem", 3)) {
                // Memory is answered straight here, not on the UI task: it reads heap
                // counters only, and the UI task is the one under pressure.
                // Stack headroom is on the SAME line as the heap figures: the driver on the
                // PC stops reading at the first "@@ok", so a second line would never arrive.
                // Added 2026-09-19 during the UI audit - several screens put 512B-1KB arrays
                // on the stack inside LVGL callbacks, and "is that safe?" deserves a
                // measurement. The tft task gets 16KB (TFT_TASK_STACK_SIZE).
                TaskHandle_t tftT = xTaskGetHandle("tft");
                TaskHandle_t loopT = xTaskGetHandle("loopTask");
                // The freeze watcher (TDeckMemInfo.cpp). Reported so its being alive is a
                // fact rather than an assumption - a watchdog nobody checks is worse than none.
                TaskHandle_t swT = xTaskGetHandle("stallwatch");
                LOG_INFO("@@ok mem internal free=%u largest=%u min=%u | psram free=%u largest=%u | stack tft=%u loop=%u sw=%u | pktq=%u peak=%u cap=%u itemsz=%u",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                         tftT ? (unsigned)(uxTaskGetStackHighWaterMark(tftT) * sizeof(StackType_t)) : 0u,
                         loopT ? (unsigned)(uxTaskGetStackHighWaterMark(loopT) * sizeof(StackType_t)) : 0u,
                         swT ? (unsigned)(uxTaskGetStackHighWaterMark(swT) * sizeof(StackType_t)) : 0u,
                         (unsigned)tdeck_pktq_depth(), (unsigned)tdeck_pktq_peak(), (unsigned)tdeck_pktq_cap(),
                         (unsigned)tdeck_pktq_itemsz());
            } else if (!strncmp(s_line, "refr", 4)) {
                // Set LVGL's refresh period at RUNTIME so candidate values can be measured in one
                // build instead of one build each. Dropping it from 40 to 25 made frames LESS
                // frequent (38ms gap -> 67ms), which is the opposite of the intent and not
                // something to keep guessing at across five-minute rebuilds.
                int ms = atoi(s_line + 4);
                if (ms < 8 || ms > 200)
                    ms = 40;
                lv_timer_t *t = lv_display_get_refr_timer(lv_display_get_default());
                if (t) {
                    lv_timer_set_period(t, (uint32_t)ms);
                    LOG_INFO("@@ok refr period=%d ms", ms);
                } else {
                    LOG_INFO("@@err refr no refresh timer");
                }
            } else if (!strncmp(s_line, "fps", 3)) {
                const char *a = s_line + 3;
                while (*a == ' ')
                    a++;
                bool on = strncmp(a, "off", 3) != 0;
                tdeck_fps_set(on);
                LOG_INFO("@@ok fps probe %s", on ? "ON" : "off");
            } else if (!strncmp(s_line, "cov", 3)) {
                // @@cov on | off | clear | stat - drive the coverage mapper over the cable, so
                // it can be tested without walking around tapping the screen.
                const char *a = s_line + 3;
                while (*a == ' ')
                    a++;
                if (!strncmp(a, "on", 2))
                    tdeck_coverage_set_enabled(true);
                else if (!strncmp(a, "off", 3))
                    tdeck_coverage_set_enabled(false);
                else if (!strncmp(a, "clear", 5))
                    tdeck_coverage_clear();
                // ⛔ "test" lays down SYNTHETIC cells so the heatmap can be looked at without a
                // survey. QUEUED, not run here: it draws, and drawing belongs to the "tft" task.
                // Poisons the grid until "clear" - see tdeck_coverage_inject(). It reports its
                // own result from the UI task, so skip the reply below rather than printing a
                // count taken before the fill has happened.
                //
                // ⚠️ And skip it by NOT REPLYING, never by returning early: the end of this
                // function resets s_len and s_armed, and jumping over that would leave the
                // parser holding half a stale command.
                const bool queued = !strncmp(a, "test", 4);
                if (queued)
                    s_cmd = 15;
                if (!queued) {
                    int32_t la = 0, lo = 0;
                    int q = 0, rs = 0, n = 0;
                    bool any = tdeck_coverage_cell(0, &la, &lo, &q, &rs, &n);
                    LOG_INFO("@@ok cov rec=%d cells=%d first=%ld,%ld snr=%.2f rssi=%d n=%d",
                             (int)tdeck_coverage_enabled(), tdeck_coverage_count(), (long)(any ? la : 0),
                             (long)(any ? lo : 0), any ? q / 4.0f : 0.0f, any ? rs : 0, any ? n : 0);
                }
            } else if (!strncmp(s_line, "place", 5)) {
                // @@place <words> - search the offline place names from the map's centre and
                // report timing plus the first few hits. Place names are public data; this never
                // prints a coordinate, only names and distances.
                const char *a = s_line + 5;
                while (*a == ' ')
                    a++;
                strncpy(s_arg, a, sizeof(s_arg) - 1);
                s_arg[sizeof(s_arg) - 1] = 0;
                s_cmd = 16; // SD + LVGL: run on the UI task
            } else if (!strncmp(s_line, "gem", 3)) {
                // @@gem [prompt]  - ask Gemini HEADLESSLY, no screen and no unlock needed.
                // @@gem           - with no prompt, just report where the last ask got to.
                //
                // Built because Gemini sat on "Connecting..." forever and driving the app by
                // hand needs Jake's PIN, which is his. This exercises the whole path - config,
                // wi-fi, TLS, Google, parse - from the cable, and says which stage it reached.
                // ⛔ Reports the REPLY, which is Gemini's own words about a prompt sent from
                // here; it never touches his messages or anything else on the device.
                const char *a = s_line + 3;
                while (*a == ' ')
                    a++;
                if (*a)
                    tdeckgemini::ask(a);
                const char *r = tdeckgemini::reply();
                LOG_INFO("@@ok gem state=%d status=\"%s\" reply=\"%.70s\"", tdeckgemini::state(),
                         tdeckgemini::statusText(), r ? r : "");
            } else if (!strncmp(s_line, "mailsend", 8)) {
                if (tdeck_mail_send_selftest())
                    LOG_INFO("@@ok mailsend queued (to the signed-in address only)");
                else
                    LOG_INFO("@@err mailsend busy, or no account saved");
            } else if (!strncmp(s_line, "mailfile", 8)) {
                // ⛔ REPORTS SHAPE, NEVER CONTENT. Jake says the app asks for the password every
                // time it opens, which means haveCreds() is reading false. This says whether the
                // file is there and how long each line is - enough to tell "missing" from
                // "empty" from "fine" - without putting a password on the wire.
                FsFile f = SDFs.open("/gmail.txt", O_RDONLY);
                if (!f) {
                    LOG_INFO("@@ok mailfile MISSING - SDFs.open failed");
                } else {
                    char l1[96] = {0}, l2[96] = {0};
                    int n1 = f.fgets(l1, sizeof(l1));
                    int n2 = f.fgets(l2, sizeof(l2));
                    const uint32_t sz = (uint32_t)f.fileSize();
                    f.close();
                    LOG_INFO("@@ok mailfile size=%u line1=%d chars line2=%d chars", (unsigned)sz, n1, n2);
                    memset(l1, 0, sizeof(l1));
                    memset(l2, 0, sizeof(l2));
                }
            } else if (!strncmp(s_line, "mailtest", 8)) {
                // ⚠️ BEFORE "mail", or the shorter prefix swallows it - the same trap the
                // lockpad/lock pair hit in @@open.
                if (tdeck_mail_connect_test())
                    LOG_INFO("@@ok mailtest connecting");
                else
                    LOG_INFO("@@err mailtest busy");
            } else if (!strncmp(s_line, "mail", 4)) {
                // Kick an inbox check and report the counts when they land. Straight on the
                // main loop: tdeck_mail_check() only records the intent, and the session itself
                // runs in tdeck_mail_service() on this same thread a moment later.
                if (tdeck_mail_check())
                    LOG_INFO("@@ok mail checking");
                else
                    LOG_INFO("@@err mail busy");
            } else if (!strncmp(s_line, "inbox", 5)) {
                // Read the result of the last @@mail without starting another one.
                int st = tdeck_mail_poll(), tot = -1, uns = -1;
                tdeck_mail_counts(&tot, &uns);
                if (st == 1)
                    LOG_INFO("@@ok inbox total=%d unseen=%d", tot, uns);
                else if (st < 0)
                    LOG_INFO("@@err inbox %s", tdeck_mail_error());
                else
                    LOG_INFO("@@ok inbox working");
            } else if (!strncmp(s_line, "shot", 4)) {
                s_cmd = 4;
            } else if (!strncmp(s_line, "get", 3)) {
                // Streams the last capture back over this same cable. Handled straight on
                // the main loop rather than the UI task - it only reads the PSRAM buffer,
                // and the UI task holds the SPI lock the radio needs.
                tdeck_shot_stream_begin();
            } else if (!strncmp(s_line, "open", 4)) {
                // Jump straight to a screen by name. Tapping tiles to reach a screen means
                // knowing which launcher page is showing, and getting that wrong opens the
                // wrong app - which cost more time than this command took to write.
                const char *a = s_line + 4;
                while (*a == ' ')
                    a++;
                int which = -1;
                // ⚠️ "lockpad" BEFORE "lock", or the shorter prefix swallows it - which it
                // did, and the sweep captured Lock settings twice instead of the keypad.
                if (!strncmp(a, "lockpad", 7))       which = 12; // the PIN keypad itself
                else if (!strncmp(a, "lock", 4))     which = 1;  // Settings > Lock screen
                else if (!strncmp(a, "settings", 8)) which = 2;
                else if (!strncmp(a, "chats", 5))    which = 3;
                else if (!strncmp(a, "nodes", 5))    which = 4;
                else if (!strncmp(a, "alerts", 6))   which = 5;
                else if (!strncmp(a, "notes", 5))    which = 6;
                else if (!strncmp(a, "glance", 6))   which = 7;
                else if (!strncmp(a, "favorites", 9)) which = 8;
                else if (!strncmp(a, "maps", 4))     which = 9;
                else if (!strncmp(a, "pins", 4))     which = 10; // the pins list, i.e. the search
                else if (!strncmp(a, "getapps", 7))  which = 11;
                else if (!strncmp(a, "mail", 4))     which = 14;
                if (which > 0) {
                    s_x = which;
                    s_cmd = 9;
                } else {
                    LOG_INFO("@@err open: lock|settings|chats|nodes|alerts|notes|glance|favorites|maps|pins|getapps|lockpad|mail");
                }
            } else if (!strncmp(s_line, "scroll", 6)) {
                // Scroll the active screen to an absolute y. Walking a swipe down a
                // 1,400-pixel Settings page took dozens of commands and usually landed
                // somewhere random - which cost more time today than writing this.
                int y = 0;
                if (sscanf(s_line + 6, "%d", &y) == 1) {
                    s_x = y;
                    s_cmd = 11;
                } else {
                    LOG_INFO("@@err scroll needs a y");
                }
            } else if (!strncmp(s_line, "batt", 4)) {
                // The tail of /battlog.csv, same path as @@diag.
                s_x = 1;
                s_cmd = 10;
            } else if (!strncmp(s_line, "diag", 4)) {
                // The tail of /diaglog.txt. Read on the UI task, where SDFs lives and the SPI
                // lock is already held - the same rule as every other card read here.
                s_x = 0;
                s_cmd = 10;
            } else if (!strncmp(s_line, "home", 4)) {
                s_cmd = 2;
            } else if (!strncmp(s_line, "back", 4)) {
                s_cmd = 3;
            } else if (!strncmp(s_line, "swipe", 5)) {
                int x1 = -1, y1 = -1, x2 = -1, y2 = -1;
                if (sscanf(s_line + 5, "%d %d %d %d", &x1, &y1, &x2, &y2) == 4) {
                    s_x = x1; s_y = y1; s_x2 = x2; s_y2 = y2;
                    s_cmd = 7;
                } else {
                    LOG_INFO("@@err swipe needs x1 y1 x2 y2");
                }
            } else if (!strncmp(s_line, "key", 3)) {
                int k = -1;
                if (sscanf(s_line + 3, "%d", &k) == 1 && k > 0) {
                    s_x = k;
                    s_cmd = 8;
                } else {
                    LOG_INFO("@@err key needs a keycode");
                }
            } else if (!strncmp(s_line, "tap", 3)) {
                int x = -1, y = -1;
                if (sscanf(s_line + 3, "%d %d", &x, &y) == 2 && x >= 0 && x < 320 && y >= 0 && y < 240) {
                    s_x = x;
                    s_y = y;
                    s_cmd = 1;
                } else {
                    LOG_INFO("@@err tap needs x y within 0,0..319,239");
                }
            } else {
                LOG_INFO("@@err unknown command");
            }
        }
        s_len = 0;
        s_armed = false;
        return;
    }
    if (!s_armed) {
        // Waiting for the "@@" prefix. s_len counts the '@'s seen so far.
        if (c == '@') {
            if (++s_len >= 2) {
                s_armed = true;
                s_len = 0;
            }
        } else {
            s_len = 0;
        }
        return;
    }
    if (s_len < kMaxLine - 1)
        s_line[s_len++] = (char)c;
}

// The text that came with the last command (@@place), for the UI task to read.
extern "C" const char *tdeck_remote_arg(void)
{
    return s_arg;
}

// Called by the UI poll timer on the tft task. Returns the pending command (and clears
// it), so that everything touching LVGL happens where LVGL lives.
extern "C" int tdeck_remote_take(int *x, int *y, int *x2, int *y2)
{
    const int c = s_cmd;
    if (!c)
        return 0;
    if (x)
        *x = s_x;
    if (y)
        *y = s_y;
    if (x2)
        *x2 = s_x2;
    if (y2)
        *y2 = s_y2;
    s_cmd = 0;
    return c;
}

// Replies go out as ordinary log lines with a @@ prefix, so they ride the existing
// serial output and a script can grep them out of the chatter.
extern "C" void tdeck_remote_reply(const char *what)
{
    LOG_INFO("@@ok %s", what ? what : "");
}
