#pragma once
#include "lvgl.h"

// The persistent top bar. Jake's note, item A4: "persistsnt top bar on most things", and the
// layout he settled on 2026-09-19: "battery top right, clock center, 'Notifications' top left.
// Which turns into '1msg' or whatever when there is one. Have the notifications be a button
// that takes you to the notifications page."
//
// It is the same three things the lock screen shows, in the same order, twenty pixels tall.
//
// WHY IT IS OPT-IN PER SCREEN. Every corner of this UI already has a control in it - Back on
// Notes, Nodes and Chats, the clock on the launcher - so a strip laid over the top would cover
// one of them. Instead a screen ASKS for the bar with tui_statusbar_reserve(), which also sets
// 20px of top padding on that screen. LVGL aligns children to the parent's CONTENT area, so
// that one call moves everything on the screen down out of the way, with no per-widget edits.
//
// Screens that do not ask for it (MUI's own generated screens, which have their own top bar,
// the lock screens, which have their own chrome, and anything full-bleed like a game or the
// flashlight) simply do not get it, and the bar hides itself while they are showing.

void tui_statusbar_init(void);                  // build it once, on the UI task
void tui_statusbar_reserve(lv_obj_t *screen);   // "this screen wants the bar" + make room
void tui_statusbar_tick(void);                  // refresh + show/hide; call a few times a second
void tui_statusbar_set_mic_active(bool active, bool transcribing = false);
bool tui_statusbar_try_toggle_keyboard(void);   // global T-Deck SYM + Space shortcut
