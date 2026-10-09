// -----------------------------------------------------------------------------
// TDeckMemInfo — tiny firmware-side bridge so the device-ui layer can read live
// RAM figures. The UI library can't include firmware src/ headers, so it links
// to these free functions across the boundary (same pattern as TDeckMeshSwitch
// and TDeckGpsBridge). Pure scalar reads — no locking needed.
//
// Used by the launcher's on-screen memory readout AND the crash investigation:
//   * current + lowest-since-boot free heap (the small fast RAM)
//   * current + lowest-since-boot free PSRAM (the big slow RAM the maps decode
//     tiles into — invisible before, and the prime crash suspect)
//   * WHY the device last restarted (esp_reset_reason) — this is what finally
//     tells RAM-vs-CPU apart: a real code fault (CRASH) vs a hung processor the
//     safety timer rebooted (FROZE) vs a power dip (BROWNOUT) leave different
//     fingerprints.
//   * the worst-case memory lows from the PREVIOUS session, persisted to NVS so
//     they survive the crash+reboot (getMinFree* alone resets every boot).
// -----------------------------------------------------------------------------
#include "configuration.h" // LOG_INFO - the boot-time diagnostics line below
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <string.h> // strncmp
#include <time.h>   // gmtime_r

#include "gps/RTC.h" // getValidTime - "last seen alive" stamps for the fault log
#include "concurrency/OSThread.h" // currentThread — names the thread a stalled main loop is stuck in
#include "SPILock.h" // spiLock — the one lock every SPI/flash user waits on; a wedged holder = the freeze

// Defined further down; used by the boot-time diagnostics line below.
extern "C" const char *tdeck_prev_reason_str(void);

extern "C" uint32_t tdeck_free_heap(void)
{
    return ESP.getFreeHeap();
}

// Lowest free-heap watermark since boot — the number that actually predicts a
// crash. Survives navigating around (only a reboot resets it).
extern "C" uint32_t tdeck_min_free_heap(void)
{
    return ESP.getMinFreeHeap();
}

extern "C" uint32_t tdeck_free_psram(void)
{
    return ESP.getFreePsram();
}

// Lowest free-PSRAM watermark since boot — the big-slow-RAM equivalent, and the
// number we were flying blind on before.
extern "C" uint32_t tdeck_min_free_psram(void)
{
    return ESP.getMinFreePsram();
}

// ---- restart reason + cross-reboot low-water persistence --------------------

static bool s_diagInit = false;
static int s_prevReason = ESP_RST_UNKNOWN;
static uint32_t s_prevPsramLow = 0; // last session's worst PSRAM free
static uint32_t s_prevHeapLow = 0;  // last session's worst heap free
static uint32_t s_savedPsramLow = 0xFFFFFFFF;
static uint32_t s_savedHeapLow = 0xFFFFFFFF;

// ---- main-loop stall detector ------------------------------------------------
// The "FROZE (task)" reboots are Meshtastic's 90s app watchdog: the MAIN loop
// stopped iterating for a minute and a half. The UI runs on its own task and
// usually stays alive through the stall, so IT can catch the freeze in the act:
// the main loop leaves a heartbeat every pass; the UI's 1s diag tick notices the
// heartbeat going stale and records HOW LONG and IN WHICH OSThread (currentThread
// = the thread the loop is stuck inside) to NVS — internal flash, no SPI, safe
// even mid SD/radio wedge. The record survives the watchdog reboot; the next
// boot reads it back and shows "FROZE in <thread>" instead of just "FROZE".
static volatile uint32_t s_lastLoopMs = 0; // 0 = loop hasn't started yet (don't arm during boot)
static uint32_t s_stallWrittenForMs = 0;   // last stall duration persisted (0 = none this stall)
static uint32_t s_prevStallMs = 0;         // read back at boot
static char s_prevStallThread[24] = {0};
static volatile void *s_loopTask = nullptr; // the main loop's task handle (for eTaskGetState)
static char s_prevStallLock[96] = {0};      // spiLock holder + loop state at stall time

// ⭐ WHEN, AND ON WHICH FIRMWARE. Until 2026-09-29 a /diaglog.txt line said what happened but not
// when, so a crash from last month and one from this morning looked identical - and after a fix,
// there was no telling whether a new line was the old bug or a new one. The previous run's
// uptime and wall-clock time are refreshed in NVS every two minutes, so the fault line can say
// "it had been up N seconds, and was last seen alive at T". The firmware version is recorded once
// per boot, so the line names the build that actually crashed.
static uint32_t s_prevUpSec = 0;   // uptime at the last "still alive" stamp of the previous run
static uint32_t s_prevSeenEp = 0;  // UTC epoch at that stamp, 0 = the clock was not set yet
static char s_prevVer[24] = {0};   // firmware version of the previous run

// The UI task's own heartbeat, so the watcher below can tell WHICH side stopped. Set from
// tdeck_diag_tick(), which the UI runs once a second.
// What the tft task was doing when it stalled (TFTView_320x240.cpp).
extern volatile const char *tdeck_tft_where;

// Set by loop() in main.cpp before each service call (TDECK_LOOP_STEP).
extern "C" {
volatile const char *tdeck_loop_where = "boot";
}

static volatile uint32_t s_lastUiMs = 0;
static volatile void *s_uiTask = nullptr;


// Record a stall, from whichever side stopped. NVS only: internal flash has its own
// controller, so this still works while SD and the radio are wedged on the SPI bus - which is
// exactly the situation being recorded.
static void recordStall(const char *who, uint32_t stuckMs)
{
    char lockInfo[96];
    char loopState = '?', uiState = '?';
    if (s_loopTask) {
        switch (eTaskGetState((TaskHandle_t)s_loopTask)) {
        case eRunning: loopState = 'R'; break;   // executing = spinning, not lock-blocked
        case eReady:   loopState = 'r'; break;
        case eBlocked: loopState = 'B'; break;   // waiting on a lock or queue
        case eSuspended: loopState = 'S'; break;
        default: break;
        }
    }
    if (s_uiTask) {
        switch (eTaskGetState((TaskHandle_t)s_uiTask)) {
        case eRunning: uiState = 'R'; break;
        case eReady:   uiState = 'r'; break;
        case eBlocked: uiState = 'B'; break;
        case eSuspended: uiState = 'S'; break;
        default: break;
        }
    }
    void *ow = spiLock ? (void *)spiLock->owner : nullptr;
    // Where the LOOP is: the service it was in, and if that is the OSThread scheduler, which thread.
    char loopAt[40];
    {
        const char *w = tdeck_loop_where ? (const char *)tdeck_loop_where : "?";
        const concurrency::OSThread *t = concurrency::OSThread::currentThread;
        if (!strcmp(w, "osthreads") && t)
            snprintf(loopAt, sizeof(loopAt), "os:%s", t->ThreadName.c_str());
        else
            snprintf(loopAt, sizeof(loopAt), "%s", w);
    }
    if (ow) {
        const uint32_t heldMs = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - spiLock->lockedAtMs;
        snprintf(lockInfo, sizeof(lockInfo), "%s %lus loop=%c@%s ui=%c in=%s", pcTaskGetName((TaskHandle_t)ow),
                 (unsigned long)(heldMs / 1000), loopState, loopAt, uiState,
                 tdeck_tft_where ? (const char *)tdeck_tft_where : "?");
    } else {
        snprintf(lockInfo, sizeof(lockInfo), "free loop=%c@%s ui=%c in=%s", loopState, loopAt, uiState,
                 tdeck_tft_where ? (const char *)tdeck_tft_where : "?");
    }
    Preferences p;
    if (p.begin("tdeckdiag", false)) {
        p.putULong("stlms", stuckMs);
        p.putString("stlth", who);
        p.putString("stlck", lockInfo);
        p.end();
    }
    LOG_WARN("STALL: %s stuck %lums  spilock=[%s]", who, (unsigned long)stuckMs, lockInfo);
}

// ⛔ ITS OWN TASK, AND A HIGHER PRIORITY, ON PURPOSE. The previous detector lived on the UI
// task and could only see the main loop stall. When both wedge on spiLock together - which is
// what the 26 unexplained "FROZE (task)" entries in /diaglog.txt are - the watcher went down
// with them and the reboot recorded nothing at all. This one shares nothing with either.
// A stall that RECOVERED must not be left in NVS. The record exists to explain a watchdog reset;
// the boot always stalls once (see TFTView updateSDCard), and before this was cleared that boot
// stall was carried to the end of the session and printed against whatever crash came hours
// later - the last CRASH line in /diaglog.txt blamed "pkt-v17", which is the boot config sync.
static void clearStallRecord(void)
{
    Preferences p;
    if (p.begin("tdeckdiag", false)) {
        p.remove("stlms");
        p.remove("stlth");
        p.remove("stlck");
        p.end();
    }
}

static void stallWatchTask(void *)
{
    uint32_t writtenLoop = 0, writtenUi = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const uint32_t now = millis();
        const uint32_t lastLoop = s_lastLoopMs, lastUi = s_lastUiMs;

        if (lastLoop) {
            const uint32_t stuck = now - lastLoop;
            if (stuck > 15000 && stuck > writtenLoop + 15000) {
                writtenLoop = stuck;
                recordStall("loop", stuck);
            } else if (stuck < 5000) {
                if (writtenLoop) {
                    LOG_INFO("STALL: loop recovered after %lums - record cleared", (unsigned long)writtenLoop);
                    clearStallRecord();
                }
                writtenLoop = 0; // it recovered
            }
        }
        if (lastUi) {
            const uint32_t stuck = now - lastUi;
            if (stuck > 15000 && stuck > writtenUi + 15000) {
                writtenUi = stuck;
                // The UI stalling is the case that was invisible before; say so plainly.
                recordStall("ui(tft)", stuck);
            } else if (stuck < 5000) {
                if (writtenUi) {
                    LOG_INFO("STALL: ui recovered after %lums - record cleared", (unsigned long)writtenUi);
                    clearStallRecord();
                }
                writtenUi = 0;
            }
        }
    }
}

extern "C" void tdeck_stallwatch_start(void)
{
    static bool started = false;
    if (started)
        return;
    started = true;
    // Priority 5: above the tft task and the main loop (both 1), so it still runs when they
    // are stuck. 3KB is ample - it calls snprintf and Preferences and nothing else.
    xTaskCreatePinnedToCore(stallWatchTask, "stallwatch", 3072, nullptr, 5, nullptr, 0);
    LOG_INFO("stall watch armed");
}

extern "C" void tdeck_loop_heartbeat(void)
{
    if (!s_loopTask)
        s_loopTask = (void *)xTaskGetCurrentTaskHandle();
    s_lastLoopMs = millis();
    // ⚠️ Only the OLD in-UI detector's record is cleared here. The watcher task keeps its own
    // "already written" state, because a stall it recorded may have been the UI side, which
    // this heartbeat says nothing about.
    if (s_stallWrittenForMs) { // loop recovered: the stall didn't kill us — clear the record
        s_stallWrittenForMs = 0;
        Preferences p;
        if (p.begin("tdeckdiag", false)) {
            p.remove("stlms");
            p.remove("stlth");
            p.remove("stlck");
            p.end();
        }
    }
}

extern "C" uint32_t tdeck_prev_stall_ms(void)
{
    return s_prevStallMs;
}

extern "C" const char *tdeck_prev_stall_thread(void)
{
    return s_prevStallThread;
}

// "who held spiLock (+ for how long) and the loop task's state" at stall time,
// e.g. "mui 63s loop=B" (UI task held it, main loop Blocked) or "free loop=R".
extern "C" const char *tdeck_prev_stall_lock(void)
{
    return s_prevStallLock;
}

// Capture-once at boot: read WHY we restarted and last session's saved lows,
// then start recording this session's lows fresh. Safe to call more than once
// (guarded) — call it before wiring up the readout.
extern "C" void tdeck_diag_boot(void)
{
    if (s_diagInit)
        return;
    s_diagInit = true;

    s_prevReason = (int)esp_reset_reason();

    Preferences p;
    if (p.begin("tdeckdiag", true)) { // read-only
        s_prevPsramLow = p.getULong("psl", 0);
        s_prevHeapLow = p.getULong("hpl", 0);
        s_prevStallMs = p.getULong("stlms", 0);
        p.getString("stlth", s_prevStallThread, sizeof(s_prevStallThread));
        p.getString("stlck", s_prevStallLock, sizeof(s_prevStallLock));
        s_prevUpSec = p.getULong("upt", 0);
        s_prevSeenEp = p.getULong("seen", 0);
        p.getString("ver", s_prevVer, sizeof(s_prevVer));
        p.end();
    }
    if (s_prevStallMs) { // consume the stall record so it only describes the LAST session
        if (p.begin("tdeckdiag", false)) {
            p.remove("stlms");
            p.remove("stlth");
            p.remove("stlck");
            p.end();
        }
    }

    // Say all of it over the cable, once, at boot. These numbers only existed inside an on-screen
    // panel you had to tap the memory readout to find - which is useless when the interesting
    // moment is a crash that already happened, or when nobody is holding the device. One line in
    // the serial log means a capture taken minutes later still answers "was it running out of
    // memory, or did something hang?" without anyone touching the screen.
    LOG_INFO("diag: last restart=%s | prev run low: fast=%uk psram=%uk | stall=%ums thread=%s lock=%s",
             tdeck_prev_reason_str(), (unsigned)(s_prevHeapLow / 1024), (unsigned)(s_prevPsramLow / 1024),
             (unsigned)s_prevStallMs, s_prevStallThread[0] ? s_prevStallThread : "-",
             s_prevStallLock[0] ? s_prevStallLock : "-");

    // Seed this session's saved lows with the current (high) free values, and
    // write them so a crash before the first new-low still leaves a sane record.
    s_savedPsramLow = ESP.getFreePsram();
    s_savedHeapLow = ESP.getFreeHeap();
    if (p.begin("tdeckdiag", false)) {
        p.putULong("psl", s_savedPsramLow);
        p.putULong("hpl", s_savedHeapLow);
        p.end();
    }
}

// Call periodically (from the 1s UI timer). Persists a new low-water mark to
// NVS only when it drops meaningfully (>8k) below what's already saved — so the
// worst PSRAM/heap free right before a crash survives the reboot, without
// hammering the flash on every tick.
extern "C" void tdeck_diag_tick(void)
{
    // The UI's heartbeat. Cheap, and it is what lets the watcher name the UI task rather than
    // just reporting that something somewhere stopped.
    if (!s_uiTask)
        s_uiTask = (void *)xTaskGetCurrentTaskHandle();
    s_lastUiMs = millis();

    if (!s_diagInit)
        return;
    uint32_t ps = ESP.getFreePsram();
    uint32_t hp = ESP.getFreeHeap();
    bool changed = false;
    if (ps + 8192 < s_savedPsramLow) {
        s_savedPsramLow = ps;
        changed = true;
    }
    if (hp + 8192 < s_savedHeapLow) {
        s_savedHeapLow = hp;
        changed = true;
    }
    if (changed) {
        Preferences p;
        if (p.begin("tdeckdiag", false)) {
            p.putULong("psl", s_savedPsramLow);
            p.putULong("hpl", s_savedHeapLow);
            p.end();
        }
    }

    // "Still alive" stamp, every two minutes. Two small NVS writes; NVS wear-levels, and at this
    // rate the flash outlives the device many times over.
    static uint32_t lastStamp = 0;
    if (millis() - lastStamp >= 120000 || !lastStamp) {
        lastStamp = millis() | 1;
        Preferences p;
        if (p.begin("tdeckdiag", false)) {
            p.putULong("upt", millis() / 1000);
            const uint32_t ep = getValidTime(RTCQualityDevice);
            if (ep)
                p.putULong("seen", ep);
            p.end();
        }
    }

    // Main-loop stall watch (runs on the UI task, which survives most stalls).
    // Arm only once the loop has heartbeat at least once (boot config-sync is slow
    // and would false-alarm), record at 15s stuck, refresh every further 15s so the
    // final record before the 90s watchdog reboot carries the near-final duration.
    uint32_t last = s_lastLoopMs;
    if (last != 0) {
        uint32_t stuck = millis() - last;
        if (stuck > 15000 && stuck > s_stallWrittenForMs + 15000) {
            s_stallWrittenForMs = stuck;
            const concurrency::OSThread *t = concurrency::OSThread::currentThread;
            // The freeze's WHY: who holds spiLock (both observed stalls — GPS and
            // RadioIf — can only block forever on it), how long they've held it,
            // and whether the main loop is Blocked (mutex wait) or Running (spin).
            char lockInfo[64];
            {
                char loopState = '?';
                if (s_loopTask) {
                    switch (eTaskGetState((TaskHandle_t)s_loopTask)) {
                    case eRunning:
                        loopState = 'R'; // actually executing = spinning, not lock-blocked
                        break;
                    case eReady:
                        loopState = 'r'; // wants cpu = spinning
                        break;
                    case eBlocked:
                        loopState = 'B'; // waiting on a lock/queue — the mutex theory
                        break;
                    case eSuspended:
                        loopState = 'S';
                        break;
                    default:
                        break;
                    }
                }
                void *ow = spiLock ? (void *)spiLock->owner : nullptr;
                if (ow) {
                    uint32_t heldMs = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - spiLock->lockedAtMs;
                    snprintf(lockInfo, sizeof(lockInfo), "%s %lus loop=%c", pcTaskGetName((TaskHandle_t)ow),
                             (unsigned long)(heldMs / 1000), loopState);
                } else {
                    snprintf(lockInfo, sizeof(lockInfo), "free loop=%c", loopState);
                }
            }
            Preferences p;
            if (p.begin("tdeckdiag", false)) {
                p.putULong("stlms", stuck);
                p.putString("stlth", t ? t->ThreadName.c_str() : "?");
                p.putString("stlck", lockInfo);
                p.end();
            }
        }
    }
}

// The UI tells us which firmware this is, once per boot. The previous value is read at boot (above)
// before this overwrites it, so the fault line can name the build that crashed.
extern "C" void tdeck_diag_set_version(const char *ver)
{
    static bool done = false;
    if (done || !ver)
        return;
    done = true;
    if (!strncmp(s_prevVer, ver, sizeof(s_prevVer)))
        return; // unchanged - no write
    Preferences p;
    if (p.begin("tdeckdiag", false)) {
        p.putString("ver", ver);
        p.end();
    }
}

// " up=1234s seen=2026-09-29 21:05Z fw=2026.09.29.1" - whatever is known, for the fault line.
extern "C" void tdeck_prev_when(char *buf, int cap)
{
    int n = snprintf(buf, cap, " up=%lus", (unsigned long)s_prevUpSec);
    if (s_prevSeenEp && n < cap) {
        time_t t = (time_t)s_prevSeenEp;
        struct tm tmv;
        gmtime_r(&t, &tmv);
        n += snprintf(buf + n, cap - n, " seen=%04d-%02d-%02d %02d:%02dZ", tmv.tm_year + 1900, tmv.tm_mon + 1,
                      tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    }
    if (s_prevVer[0] && n < cap)
        snprintf(buf + n, cap - n, " fw=%s", s_prevVer);
}

extern "C" int tdeck_prev_reason(void)
{
    return s_prevReason;
}

// Short human label for the previous restart cause.
extern "C" const char *tdeck_prev_reason_str(void)
{
    switch (s_prevReason) {
    case ESP_RST_POWERON:
        return "power on";
    case ESP_RST_SW:
        return "restart";
    case ESP_RST_PANIC:
        return "CRASH"; // code fault / bad memory access
    case ESP_RST_INT_WDT:
        return "FROZE"; // interrupt watchdog — cpu hung
    case ESP_RST_TASK_WDT:
        return "FROZE (task)"; // task watchdog — a task hogged the cpu
    case ESP_RST_WDT:
        return "FROZE (wdt)";
    case ESP_RST_BROWNOUT:
        return "power dip"; // supply voltage sagged
    case ESP_RST_DEEPSLEEP:
        return "wake";
    case ESP_RST_EXT:
        return "ext reset";
    // Named, because "unknown" hid them: a USB reset is what flashing - and a serial tool toggling
    // the control lines - does, and it read exactly like a mystery restart.
    case ESP_RST_USB:
        return "usb reset";
    case ESP_RST_JTAG:
        return "jtag reset";
    case ESP_RST_PWR_GLITCH:
        return "power glitch";
    case ESP_RST_CPU_LOCKUP:
        return "CRASH (lockup)";
    default:
        return "unknown";
    }
}

// True when the previous restart looks like a genuine fault worth logging/alerting.
extern "C" bool tdeck_prev_reason_bad(void)
{
    switch (s_prevReason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_PWR_GLITCH:
    case ESP_RST_CPU_LOCKUP:
        return true;
    default:
        return false;
    }
}

extern "C" uint32_t tdeck_prev_psram_low(void)
{
    return s_prevPsramLow;
}

extern "C" uint32_t tdeck_prev_heap_low(void)
{
    return s_prevHeapLow;
}
