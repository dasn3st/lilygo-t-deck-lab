// -----------------------------------------------------------------------------
// The persistent top bar. See TuiStatusBar.h for the design and why it is opt-in.
//
// THREADING. Everything here runs on the "tft" task: tui_statusbar_init() from screen setup,
// tui_statusbar_tick() from the existing poll timer. Nothing here touches the SD card or the
// radio, so it takes no locks and cannot stall either.
// -----------------------------------------------------------------------------
#include "graphics/view/TFT/TuiStatusBar.h"
#include "graphics/view/TFT/TDeckT9.h"

#include <Arduino.h>
#include "lvgl.h"
#include "ui.h" // ui_font_montserrat_12
#include <cstdio>

extern "C" void notif_open(void);
extern "C" int notif_count(void);
extern "C" void tdeck_open_launcher(void);
// The battery figures the launcher already keeps, so the bar and the launcher can never
// disagree about what is left (src/TDeckBatteryBridge is not a thing - these come from the
// view, through a shim, for the same reason every other bridge here does).
extern "C" int tdeck_battery_pct(void);
extern "C" bool tdeck_battery_plugged(void);
extern "C" bool tdeck_clock_text(char *out, int outN); // "9:41 AM" / "21:41", false if unknown
namespace tdeckvoice
{
int state();
uint8_t inputLevel();
} // namespace tdeckvoice

namespace
{
lv_obj_t *bar = nullptr;
lv_obj_t *notifLbl = nullptr;
lv_obj_t *clockLbl = nullptr;
lv_obj_t *batLbl = nullptr;
lv_obj_t *micIcon = nullptr;
lv_obj_t *micHead = nullptr;
lv_obj_t *micStem = nullptr;
lv_obj_t *micBase = nullptr;
lv_obj_t *micMeter[4] = {};
bool micActive = false;
bool micTranscribing = false;
bool micBlinkOn = true;
uint32_t micBlinkAtMs = 0;
lv_obj_t *kbdBtn = nullptr;
lv_obj_t *softKeyboard = nullptr;
lv_obj_t *softTarget = nullptr;

// Screens that asked for the bar. Sixteen is far more than this UI has; anything past that
// simply does not get the bar rather than overwriting somebody else's entry.
const int kMaxScreens = 16;
lv_obj_t *wanted[kMaxScreens] = {nullptr};
int wantedN = 0;

bool wantsBar(lv_obj_t *scr)
{
    for (int i = 0; i < wantedN; i++)
        if (wanted[i] == scr)
            return true;
    return false;
}

bool isVisibleInput(lv_obj_t *obj, lv_obj_t *scr)
{
    if (!obj || !lv_obj_is_valid(obj) || !lv_obj_check_type(obj, &lv_textarea_class))
        return false;
    lv_obj_t *owner = lv_obj_get_screen(obj);
    return owner == scr || owner == lv_layer_top() || owner == lv_layer_sys();
}

void closeSoftKeyboard(void *)
{
    if (softKeyboard && lv_obj_is_valid(softKeyboard))
        lv_obj_delete(softKeyboard);
    softKeyboard = nullptr;
    softTarget = nullptr;
}

void softKeyboardEvent(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL || code == LV_EVENT_READY)
        lv_async_call(closeSoftKeyboard, nullptr);
}

void toggleSoftKeyboard(lv_event_t *)
{
    if (softKeyboard) {
        closeSoftKeyboard(nullptr);
        return;
    }

    lv_group_t *group = lv_group_get_default();
    lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
    if (!focused || !lv_obj_check_type(focused, &lv_textarea_class))
        return;

    softTarget = focused;
    softKeyboard = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(softKeyboard, 320, 132);
    lv_obj_align(softKeyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(softKeyboard, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(softKeyboard, lv_color_hex(0x1b1d21), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(softKeyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_keyboard_set_textarea(softKeyboard, softTarget);
    lv_keyboard_set_popovers(softKeyboard, true);
    lv_obj_add_event_cb(softKeyboard, softKeyboardEvent, LV_EVENT_CANCEL, nullptr);
    lv_obj_add_event_cb(softKeyboard, softKeyboardEvent, LV_EVENT_READY, nullptr);
    lv_obj_move_foreground(softKeyboard);
}
} // namespace

bool tui_statusbar_try_toggle_keyboard(void)
{
    lv_group_t *group = lv_group_get_default();
    lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
    if (!focused || !lv_obj_check_type(focused, &lv_textarea_class))
        return false;
    toggleSoftKeyboard(nullptr);
    return true;
}

void tui_statusbar_set_mic_active(bool active, bool transcribing)
{
    micActive = active;
    micTranscribing = transcribing;
    micBlinkOn = true;
    micBlinkAtMs = millis();
    if (!micIcon)
        return;
    if (!active) {
        lv_obj_add_flag(micIcon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const lv_color_t color = lv_color_hex(transcribing ? 0x0a84ff : 0xff453a);
    lv_obj_set_style_bg_color(micHead, color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(micStem, color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(micBase, color, LV_PART_MAIN);
    lv_obj_clear_flag(micIcon, LV_OBJ_FLAG_HIDDEN);
}

void tui_statusbar_init(void)
{
    if (bar)
        return;
    bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 320, 20);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0a0a0c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        bar,
        [](lv_event_t *e) {
            // Notifications owns its own click target. Only the free area of the bar
            // should act as the quick route back to the launcher.
            if (lv_event_get_target(e) != bar)
                return;
            lv_async_call([](void *) { tdeck_open_launcher(); }, nullptr);
        },
        LV_EVENT_CLICKED, nullptr);
    // A hairline, so the bar reads as chrome rather than as part of the screen under it.
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);

    notifLbl = lv_label_create(bar);
    lv_obj_set_style_text_font(notifLbl, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(notifLbl, 0, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(notifLbl, 0, LV_PART_MAIN);
    lv_obj_set_width(notifLbl, 92);
    lv_obj_set_height(notifLbl, 18);
    lv_label_set_long_mode(notifLbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(notifLbl, "Meldungen");
    lv_obj_align(notifLbl, LV_ALIGN_LEFT_MID, 6, 0);
    // The label itself is the button - a real lv_btn would not fit in 20px with its padding,
    // and there is nothing else up here to mis-tap. The hit area is widened below.
    lv_obj_add_flag(notifLbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(notifLbl, 8);
    lv_obj_add_event_cb(
        notifLbl,
        // Deferred: this handler belongs to a label on the top layer, and opening the page
        // changes the screen underneath it.
        [](lv_event_t *) { lv_async_call([](void *) { notif_open(); }, nullptr); },
        LV_EVENT_CLICKED, nullptr);

    // Small custom microphone mark between Notifications and the clock. Keep it as LVGL
    // primitives (rather than an emoji glyph) so it renders consistently on the T-Deck font.
    micIcon = lv_obj_create(bar);
    lv_obj_remove_style_all(micIcon);
    lv_obj_set_size(micIcon, 18, 18);
    lv_obj_align(micIcon, LV_ALIGN_LEFT_MID, 99, 0);
    lv_obj_clear_flag(micIcon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(micIcon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(micIcon, LV_OBJ_FLAG_HIDDEN);
    micHead = lv_obj_create(micIcon);
    lv_obj_remove_style_all(micHead);
    lv_obj_set_size(micHead, 5, 8);
    lv_obj_set_pos(micHead, 6, 1);
    lv_obj_set_style_radius(micHead, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(micHead, LV_OPA_COVER, LV_PART_MAIN);
    micStem = lv_obj_create(micIcon);
    lv_obj_remove_style_all(micStem);
    lv_obj_set_size(micStem, 2, 4);
    lv_obj_set_pos(micStem, 7, 8);
    lv_obj_set_style_bg_opa(micStem, LV_OPA_COVER, LV_PART_MAIN);
    micBase = lv_obj_create(micIcon);
    lv_obj_remove_style_all(micBase);
    lv_obj_set_size(micBase, 8, 2);
    lv_obj_set_pos(micBase, 4, 12);
    lv_obj_set_style_radius(micBase, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(micBase, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(micHead, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(micHead, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(micStem, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(micStem, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(micBase, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(micBase, LV_OBJ_FLAG_SCROLLABLE);

    // Live input meter to the right of the clock; bars update from the recorder's I2S blocks.
    static const int barHeights[] = {3, 6, 9, 13};
    for (int i = 0; i < 4; i++) {
        micMeter[i] = lv_obj_create(bar);
        lv_obj_remove_style_all(micMeter[i]);
        lv_obj_set_size(micMeter[i], 3, barHeights[i]);
        lv_obj_align(micMeter[i], LV_ALIGN_LEFT_MID, 202 + i * 5, 0);
        lv_obj_set_style_radius(micMeter[i], 1, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(micMeter[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(micMeter[i], lv_color_hex(0x48484a), LV_PART_MAIN);
        lv_obj_clear_flag(micMeter[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(micMeter[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(micMeter[i], LV_OBJ_FLAG_HIDDEN);
    }

    clockLbl = lv_label_create(bar);
    lv_obj_set_style_text_font(clockLbl, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(clockLbl, 0, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(clockLbl, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(clockLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_size(clockLbl, 80, 18);
    lv_obj_set_style_text_align(clockLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(clockLbl, "");
    lv_obj_align(clockLbl, LV_ALIGN_TOP_MID, 0, 1);

    batLbl = lv_label_create(bar);
    lv_obj_set_style_text_font(batLbl, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(batLbl, 0, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(batLbl, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(batLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_size(batLbl, 48, 18);
    lv_obj_set_style_text_align(batLbl, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_text(batLbl, "");
    lv_obj_align(batLbl, LV_ALIGN_TOP_RIGHT, -5, 1);

    // A single global keyboard button keeps every text field consistent. It only appears while
    // an editable field has focus, so the status bar stays clean on ordinary screens.
    kbdBtn = lv_button_create(bar);
    lv_obj_set_size(kbdBtn, 30, 19);
    lv_obj_align(kbdBtn, LV_ALIGN_TOP_LEFT, 224, 0);
    lv_obj_set_style_radius(kbdBtn, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(kbdBtn, lv_color_hex(0x1b1d21), LV_PART_MAIN);
    lv_obj_set_style_bg_color(kbdBtn, lv_color_hex(0x303238), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(kbdBtn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(kbdBtn, 0, LV_PART_MAIN);
    lv_obj_t *kbdLbl = lv_label_create(kbdBtn);
    lv_label_set_text(kbdLbl, LV_SYMBOL_KEYBOARD);
    lv_obj_set_style_text_color(kbdLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(kbdLbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(kbdLbl);
    lv_obj_add_event_cb(kbdBtn, toggleSoftKeyboard, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(kbdBtn, LV_OBJ_FLAG_HIDDEN);
}

// ⛔ A DELETED SCREEN MUST LEAVE THE LIST. Chess and Gemini delete their screens a few seconds
// after you leave them (to give the memory back) and build new ones when you return - and each new
// one registered here while the old entry stayed. After ~16 visits in one boot the list was full,
// and the next chess screen got neither the bar nor its 20px of top padding: found by a 51-minute
// stress test (2026-09-30), where chess came back with no bar and everything shifted up.
static void onReservedScreenDeleted(lv_event_t *e)
{
    lv_obj_t *scr = (lv_obj_t *)lv_event_get_current_target(e);
    for (int i = 0; i < wantedN; i++) {
        if (wanted[i] == scr) {
            wanted[i] = wanted[--wantedN];
            wanted[wantedN] = nullptr;
            return;
        }
    }
}

void tui_statusbar_reserve(lv_obj_t *screen)
{
    if (!screen || wantedN >= kMaxScreens || wantsBar(screen))
        return;
    wanted[wantedN++] = screen;
    lv_obj_add_event_cb(screen, onReservedScreenDeleted, LV_EVENT_DELETE, nullptr);
    // The whole trick: LVGL aligns children to the parent's CONTENT area, so padding the
    // screen moves every TOP_*-aligned child down by 20px without touching any of them.
    // BOTTOM_*-aligned children are unaffected, which is what we want for a bar at the top.
    lv_obj_set_style_pad_top(screen, 20, LV_PART_MAIN);
}

void tui_statusbar_tick(void)
{
    if (!bar)
        return;

    static int lastVoiceState = -1;
    const int voiceState = tdeckvoice::state();
    if (voiceState != lastVoiceState) {
        lastVoiceState = voiceState;
        tui_statusbar_set_mic_active(voiceState == 1 || voiceState == 2, voiceState == 2);
    }

    // T9 is global to all text fields, including screens that do not use the
    // persistent status bar. It hides itself when no editor is focused.
    tdeck_t9_tick();

    lv_obj_t *scr = lv_screen_active();
    const bool show = wantsBar(scr);
    const bool hidden = lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN);
    if (show == hidden) { // state changed
        if (show)
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (!show)
        return; // nothing on screen to keep up to date

    // A red blinking mic means capture is live; solid blue means the recording has ended and
    // speech is being transcribed. This is ticked on the UI task, never from the recorder task.
    if (micActive && !micTranscribing) {
        const uint32_t now = millis();
        if ((uint32_t)(now - micBlinkAtMs) >= 400) {
            micBlinkAtMs = now;
            micBlinkOn = !micBlinkOn;
        }
        if (micBlinkOn)
            lv_obj_clear_flag(micIcon, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(micIcon, LV_OBJ_FLAG_HIDDEN);
    } else if (micActive) {
        lv_obj_clear_flag(micIcon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(micIcon, LV_OBJ_FLAG_HIDDEN);
    }

    if (micActive && !micTranscribing) {
        const uint8_t level = tdeckvoice::inputLevel();
        const int activeBars = level < 4 ? 0 : level < 12 ? 1 : level < 28 ? 2 : level < 52 ? 3 : 4;
        for (int i = 0; i < 4; i++) {
            lv_obj_set_style_bg_color(micMeter[i], lv_color_hex(i < activeBars ? 0x30d158 : 0x48484a),
                                      LV_PART_MAIN);
            if (i < activeBars)
                lv_obj_clear_flag(micMeter[i], LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(micMeter[i], LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        for (int i = 0; i < 4; i++)
            lv_obj_add_flag(micMeter[i], LV_OBJ_FLAG_HIDDEN);
    }

    // If an app was left while the virtual keyboard was open, remove that overlay before the
    // old textarea disappears. This also prevents a keyboard from leaking into the launcher.
    if (softKeyboard && (!softTarget || !lv_obj_is_valid(softTarget) ||
                         (!isVisibleInput(softTarget, scr) && softTarget != lv_keyboard_get_textarea(softKeyboard))))
        closeSoftKeyboard(nullptr);

    lv_group_t *group = lv_group_get_default();
    lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
    const bool inputFocused = isVisibleInput(focused, scr);
    if (kbdBtn) {
        if (inputFocused || softKeyboard)
            lv_obj_clear_flag(kbdBtn, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(kbdBtn, LV_OBJ_FLAG_HIDDEN);
    }

    // Overlays (the pins list, rename, share) are full-screen objects on this same layer and
    // are created after us, so they sit on top. Keeping the bar in front of them would cover
    // their own Close buttons, so it deliberately stays behind - it is chrome, not a modal.

    char buf[24];
    const int unread = notif_count();
    if (unread == 0)
        snprintf(buf, sizeof(buf), "Meldungen");
    else if (unread == 1)
        snprintf(buf, sizeof(buf), "1 msg");
    else
        snprintf(buf, sizeof(buf), "%d msgs", unread);
    // ⛔ lv_label_set_text SKIPS UNCHANGED TEXT. lv_obj_set_style_text_color DOES NOT - setting
    // a style invalidates the object whether or not the value differs. The original comment here
    // said this was "cheap to call several times a second" and was half right: the text was
    // cheap, the three colour writes underneath were repainting the whole bar 17 times a second
    // on every screen that shows it.
    //
    // MEASURED with @@fps: 17 fps and 124 KB/s pushed while sitting IDLE on the Mail and Chess
    // screens, against 2 fps and 5 KB/s on the launcher, which does not use this bar. Jake
    // reported it as "having chess open is lagging hard, maybe froze my device" - it was not
    // chess, it was every app built on this bar. Only write a colour when it changes.
    static uint32_t lastNotifCol = 0xFFFFFFFF, lastBatCol = 0xFFFFFFFF;
    lv_label_set_text(notifLbl, buf);
    const uint32_t notifCol = unread ? 0x30d158 : 0x8e8e93;
    if (notifCol != lastNotifCol) {
        lastNotifCol = notifCol;
        lv_obj_set_style_text_color(notifLbl, lv_color_hex(notifCol), LV_PART_MAIN);
    }

    if (tdeck_clock_text(buf, sizeof(buf)))
        lv_label_set_text(clockLbl, buf);
    else
        lv_label_set_text(clockLbl, "");

    const int pct = tdeck_battery_pct();
    uint32_t batCol;
    if (pct < 0) {
        lv_label_set_text(batLbl, "--");
        batCol = 0x8e8e93;
    } else {
        const bool plugged = tdeck_battery_plugged();
        snprintf(buf, sizeof(buf), "%d%%%s", pct, plugged ? "+" : "");
        lv_label_set_text(batLbl, buf);
        batCol = plugged ? 0x30d158 : (pct <= 15 ? 0xff453a : 0xffffff);
    }
    if (batCol != lastBatCol) {
        lastBatCol = batCol;
        lv_obj_set_style_text_color(batLbl, lv_color_hex(batCol), LV_PART_MAIN);
    }
}
