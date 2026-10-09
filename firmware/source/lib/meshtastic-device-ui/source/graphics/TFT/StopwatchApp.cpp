// -----------------------------------------------------------------------------
// T-UI Stopwatch & Timer — a self-contained clock app for the launcher.
//
// Two modes, chosen by the segmented control up top:
//   • Stopwatch — counts UP from zero. Start/Stop and Reset.
//   • Timer     — counts DOWN from a set duration (adjust with − / +, one minute
//                 per tap). At zero the readout flashes red and shows "Time's up".
// Timing is millis()-based so it stays accurate regardless of the UI refresh rate;
// a short LVGL timer just repaints while running. Trackball double-click = Home
// exits, same as everywhere. The timer is paused when the screen isn't showing.
// -----------------------------------------------------------------------------
#include "graphics/view/TFT/TuiStatusBar.h" // the persistent top bar
#include "lvgl.h"
#include <Arduino.h> // millis()
#include <cstdio>

extern "C" void stopwatch_open(void);

// Sound is produced by the firmware's buzzer helpers (buzz.cpp), which route
// through the T-Deck's I2S speaker. This UI library can't include firmware src/
// headers, so we forward-declare them and let the linker resolve them across the
// boundary — same trick the mesh kill-switch bridge uses.
void playBeep();                          // short blip (~1/16 note)
void playLongBeep();                      // clear 1-second tone
extern "C" void tdeck_beep_gain(float g); // set audio loudness (0.2 default; timer cranks to ~1.0)

// The alarm itself lives in src/TDeckAlarm.cpp and is serviced from loop(), NOT from this app.
// An alarm that only runs while its own screen is open is not an alarm - the whole point is
// 6am with the device locked, dark and face-down. This screen only sets it.
extern "C" void tdeck_alarm_get(int *h, int *m, bool *on);
extern "C" void tdeck_alarm_set(int h, int m, bool on);
extern "C" bool tdeck_alarm_ringing(void);
extern "C" void tdeck_alarm_dismiss(void);

namespace
{
enum Mode { MODE_STOPWATCH, MODE_TIMER, MODE_ALARM };

lv_obj_t *screen = nullptr;
lv_obj_t *timeLbl = nullptr;
lv_obj_t *startBtnLbl = nullptr;
lv_obj_t *swModeBtn = nullptr;
lv_obj_t *tmModeBtn = nullptr;
lv_obj_t *alModeBtn = nullptr;
lv_obj_t *resetBtnRef = nullptr;
lv_obj_t *minusBtn = nullptr;
lv_obj_t *plusBtn = nullptr;
lv_obj_t *hintLbl = nullptr;
lv_timer_t *tick = nullptr;

Mode mode = MODE_STOPWATCH;
bool running = false;
uint32_t startMs = 0;              // millis() reference when (re)started
uint32_t accumMs = 0;              // elapsed frozen at the last Stop
uint32_t timerSetMs = 5 * 60000UL; // configured countdown duration (default 5:00)
bool timerDone = false;
uint32_t lastBeepMs = 0;     // throttles the repeating "time's up" alarm beep
uint32_t timerDoneAtMs = 0;  // when the alarm started, so it can auto-silence

const uint32_t kTimerMaxMs = 99 * 60000UL + 59000UL;

uint32_t rawElapsed(void) { return running ? (accumMs + (millis() - startMs)) : accumMs; }

// what the big label should show, in ms
uint32_t shownMs(void)
{
    if (mode == MODE_STOPWATCH)
        return rawElapsed();
    uint32_t e = rawElapsed();
    return (e >= timerSetMs) ? 0 : (timerSetMs - e);
}

void paint(void)
{
    uint32_t ms = shownMs();
    uint32_t tenths = (ms / 100) % 10;
    uint32_t secs = (ms / 1000) % 60;
    uint32_t mins = (ms / 60000) % 100;
    char buf[16];
    if (mode == MODE_ALARM) {
        // The alarm shows the time it will RING at, not an elapsed count - it is a setting
        // being edited, not a clock running.
        int h = 7, m = 0;
        bool on = false;
        tdeck_alarm_get(&h, &m, &on);
        snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
        lv_label_set_text(timeLbl, buf);
        return;
    }
    if (mode == MODE_STOPWATCH)
        snprintf(buf, sizeof(buf), "%02u:%02u.%u", (unsigned)mins, (unsigned)secs, (unsigned)tenths);
    else
        snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)mins, (unsigned)secs); // timer: no tenths
    lv_label_set_text(timeLbl, buf);
}

void setReadoutColor(uint32_t c) { lv_obj_set_style_text_color(timeLbl, lv_color_hex(c), LV_PART_MAIN); }

void showAdjust(bool show)
{
    if (!minusBtn || !plusBtn)
        return;
    if (show) {
        lv_obj_clear_flag(minusBtn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(plusBtn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(minusBtn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(plusBtn, LV_OBJ_FLAG_HIDDEN);
    }
}

void refreshModeButtons(void)
{
    // brighter fill = selected mode
    lv_obj_set_style_bg_color(swModeBtn, lv_color_hex(mode == MODE_STOPWATCH ? 0x0a84ff : 0x2c2c2e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(tmModeBtn, lv_color_hex(mode == MODE_TIMER ? 0x0a84ff : 0x2c2c2e), LV_PART_MAIN);
    if (alModeBtn)
        lv_obj_set_style_bg_color(alModeBtn, lv_color_hex(mode == MODE_ALARM ? 0x0a84ff : 0x2c2c2e), LV_PART_MAIN);
}

void stopAndReset(void)
{
    running = false;
    accumMs = 0;
    startMs = millis();
    timerDone = false;
    setReadoutColor(0xffffff);
    lv_label_set_text(startBtnLbl, "Start");
    lv_label_set_text(hintLbl, "");
    paint();
}

// What the Start button says, and whether Reset means anything, both depend on the mode.
void refreshAlarmUi(void)
{
    if (mode != MODE_ALARM)
        return;
    int h = 7, m = 0;
    bool on = false;
    tdeck_alarm_get(&h, &m, &on);
    lv_label_set_text(startBtnLbl, tdeck_alarm_ringing() ? "Stoppen" : (on ? "An" : "Aus"));
    setReadoutColor(on ? 0x30d158 : 0xffffff); // green = armed, so the state is readable at a glance
    lv_label_set_text(hintLbl, tdeck_alarm_ringing() ? "Wecker!" : (on ? "" : "Wecker ist aus"));
    paint();
}

void setMode(Mode m)
{
    mode = m;
    stopAndReset();
    // The alarm uses the same - / + buttons to set its time, so they show for it too.
    showAdjust(m == MODE_TIMER || m == MODE_ALARM);
    // Reset means nothing for an alarm - there is no running count to zero.
    if (resetBtnRef) {
        if (m == MODE_ALARM)
            lv_obj_add_flag(resetBtnRef, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(resetBtnRef, LV_OBJ_FLAG_HIDDEN);
    }
    refreshModeButtons();
    refreshAlarmUi();
}

void onStartStop(lv_event_t *)
{
    if (mode == MODE_ALARM) {
        int h = 7, m = 0;
        bool on = false;
        tdeck_alarm_get(&h, &m, &on);
        if (tdeck_alarm_ringing())
            tdeck_alarm_dismiss(); // ringing: the button is the snooze-less "stop"
        else
            tdeck_alarm_set(h, m, !on);
        refreshAlarmUi();
        return;
    }
    if (mode == MODE_TIMER && timerDone)
        return; // must Reset after a finished timer
    if (mode == MODE_TIMER && !running && timerSetMs == 0)
        return; // nothing to count down
    if (running) {
        accumMs += millis() - startMs; // freeze
        running = false;
        lv_label_set_text(startBtnLbl, "Start");
    } else {
        startMs = millis();
        running = true;
        lv_label_set_text(startBtnLbl, "Stop");
        lv_label_set_text(hintLbl, "");
    }
    showAdjust(mode == MODE_TIMER && !running);
    paint();
}

void onReset(lv_event_t *)
{
    stopAndReset();
    showAdjust(mode == MODE_TIMER);
}

void adjustTimer(int deltaMs)
{
    if (mode == MODE_ALARM) {
        // 15-minute steps, wrapping through midnight. Fine enough for a wake-up and quick
        // enough to cross the dial without forty taps.
        int h = 7, m = 0;
        bool on = false;
        tdeck_alarm_get(&h, &m, &on);
        int total = h * 60 + m + (deltaMs > 0 ? 15 : -15);
        while (total < 0)
            total += 24 * 60;
        total %= 24 * 60;
        tdeck_alarm_set(total / 60, total % 60, on);
        refreshAlarmUi();
        return;
    }
    if (mode != MODE_TIMER || running || timerDone)
        return;
    long v = (long)timerSetMs + deltaMs;
    if (v < 0)
        v = 0;
    if (v > (long)kTimerMaxMs)
        v = kTimerMaxMs;
    timerSetMs = (uint32_t)v;
    accumMs = 0; // adjusting resets progress
    paint();
}

// Alarm beep at full volume, then restore the gentle default so other system
// sounds stay quiet. playBeep/playLongBeep busy-wait until the tone finishes, so
// the loud gain is in effect for the whole beep and then reset.
void loudBeep(bool longTone)
{
    tdeck_beep_gain(1.0f);
    if (longTone)
        playLongBeep();
    else
        playBeep();
    tdeck_beep_gain(0.2f);
}

void step(lv_timer_t *)
{
    if (mode == MODE_TIMER && timerDone) {
        // flash the "Time's up" readout
        static bool on = false;
        on = !on;
        setReadoutColor(on ? 0xff453a : 0x5a1512);
        // gentle repeating alarm beep until the user taps Reset (throttled so it
        // doesn't hold up the UI on every tick); auto-silences after 2 minutes so
        // it never rings forever if you've walked away
        if (millis() - timerDoneAtMs < 120000UL && millis() - lastBeepMs >= 1500) {
            lastBeepMs = millis();
            loudBeep(false);
        }
        return;
    }
    if (!running)
        return;
    if (mode == MODE_TIMER && rawElapsed() >= timerSetMs) {
        running = false;
        timerDone = true;
        lv_label_set_text(startBtnLbl, "Start");
        lv_label_set_text(hintLbl, "Zeit abgelaufen! Tippe auf Reset");
        timerDoneAtMs = millis();
        lastBeepMs = millis();
        loudBeep(true); // strong, LOUD tone the moment it hits zero
        paint();
        return;
    }
    paint();
}

lv_obj_t *makeButton(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb, lv_obj_t **outLbl)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &ui_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(lbl);
    if (outLbl)
        *outLbl = lbl;
    return btn;
}

void buildScreen(void)
{
    screen = lv_obj_create(NULL);
    tui_statusbar_reserve(screen); // note item A4: the persistent top bar
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // mode selector (top): Stopwatch | Timer
    // Three modes across the top now, so 150 no longer fits: 6 + 100 + 4 + 100 + 4 + 100 + 6
    // is exactly the 320 available.
    swModeBtn = makeButton(screen, "Stoppuhr", 0x0a84ff, [](lv_event_t *) { setMode(MODE_STOPWATCH); }, nullptr);
    lv_obj_set_size(swModeBtn, 100, 40);
    lv_obj_align(swModeBtn, LV_ALIGN_TOP_LEFT, 6, 6);
    tmModeBtn = makeButton(screen, "Timer", 0x2c2c2e, [](lv_event_t *) { setMode(MODE_TIMER); }, nullptr);
    lv_obj_set_size(tmModeBtn, 100, 40);
    lv_obj_align(tmModeBtn, LV_ALIGN_TOP_MID, 0, 6);
    alModeBtn = makeButton(screen, "Wecker", 0x2c2c2e, [](lv_event_t *) { setMode(MODE_ALARM); }, nullptr);
    lv_obj_set_size(alModeBtn, 100, 40);
    lv_obj_align(alModeBtn, LV_ALIGN_TOP_RIGHT, -6, 6);

    // big readout, with − / + adjust buttons flanking it (timer mode only)
    timeLbl = lv_label_create(screen);
    lv_obj_set_style_text_color(timeLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(timeLbl, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_align(timeLbl, LV_ALIGN_CENTER, 0, -18);

    minusBtn = makeButton(screen, "-", 0x3a3a3c, [](lv_event_t *) { adjustTimer(-60000); }, nullptr);
    lv_obj_set_size(minusBtn, 48, 48);
    lv_obj_align(minusBtn, LV_ALIGN_LEFT_MID, 8, -18);
    plusBtn = makeButton(screen, "+", 0x3a3a3c, [](lv_event_t *) { adjustTimer(60000); }, nullptr);
    lv_obj_set_size(plusBtn, 48, 48);
    lv_obj_align(plusBtn, LV_ALIGN_RIGHT_MID, -8, -18);

    hintLbl = lv_label_create(screen);
    lv_obj_set_style_text_color(hintLbl, lv_color_hex(0xff453a), LV_PART_MAIN);
    lv_obj_align(hintLbl, LV_ALIGN_CENTER, 0, 24);

    lv_obj_t *startBtn = makeButton(screen, "Start", 0x30d158, onStartStop, &startBtnLbl);
    lv_obj_set_size(startBtn, 130, 52);
    lv_obj_align(startBtn, LV_ALIGN_BOTTOM_LEFT, 16, -14);
    lv_obj_t *resetBtn = makeButton(screen, "Reset", 0x3a3a3c, onReset, nullptr);
    resetBtnRef = resetBtn; // hidden in Alarm mode - there is no count to reset
    lv_obj_set_size(resetBtn, 130, 52);
    lv_obj_align(resetBtn, LV_ALIGN_BOTTOM_RIGHT, -16, -14);

    tick = lv_timer_create(step, 73, NULL);
    lv_timer_pause(tick);

    lv_obj_add_event_cb(
        screen, [](lv_event_t *) { lv_timer_resume(tick); }, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(
        screen,
        [](lv_event_t *) {
            // Keep counting in the background while a timer is live (running or
            // ringing) so it alarms wherever you are; otherwise pause to save cycles.
            if (!(mode == MODE_TIMER && (running || timerDone)))
                lv_timer_pause(tick);
        },
        LV_EVENT_SCREEN_UNLOADED, NULL);

    setMode(MODE_STOPWATCH); // sets adjust visibility + mode-button highlight + readout
}
} // namespace

extern "C" void stopwatch_open(void)
{
    if (!screen)
        buildScreen();
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}
