// -----------------------------------------------------------------------------
// T-UI Notification Centre — who messaged you, wherever you happen to be.
//
// Jake, 2026-09-18 (written in the Notes app on the device):
//   "way to show new message notification when screen locked who from too."
//   "new message popup overlay nomatter whaht app your in"
//   "then youd click the '1msg' it would take you to a notifications page showing
//    list of new mrssges and from whom. a close notifavations snd clear button.
//    cick the notifocation to take you to the convo."
//
// Before this, a message arriving while you were anywhere other than the Meshtastic
// screens was invisible: showMessagePopup() draws on objects.msg_popup_panel, which
// is a child of main_screen, so the launcher, every app, and the lock screen showed
// nothing at all. The unread COUNT made it to the launcher top bar, but not who from.
//
// Three parts, and the same low-touch arrangement as NodesApp/ChannelsApp — its own
// screen, extern "C" bridges, no reaching into MUI's internals:
//   • a small store of recent arrivals (PSRAM),
//   • a pop-up drawn on lv_layer_top(), so it is visible over ANY screen,
//   • a Notifications page listing them, each row opening that conversation.
//
// ⚠️ THE POP-UP LIVES ON lv_layer_top() AND THEREFORE FLOATS OVER EVERYTHING,
// including the PIN pad and the mesh pop-ups. It is deliberately NOT shown while a
// lock screen is up (the lock screen gets its own display of this same store — that
// is Jake's "when screen locked" item) and it takes taps only on its own two
// buttons, so nothing underneath is ever silently swallowed.
// -----------------------------------------------------------------------------
#include "graphics/view/TFT/TuiStatusBar.h" // the persistent top bar
#include "graphics/view/TFT/TuiLabel.h" // tui_one_line: LONG_DOT needs a height
#include "graphics/view/TFT/UnreadCounts.h" // unread_clear_all
#include "lvgl.h"
#include <cstdio>
#include <cstring>

// The generated UI's font, which this build definitely has. LVGL's own
// lv_font_montserrat_12 is a config option and checking the LVGL source tree does not
// tell you whether it is compiled IN — that mistake has cost a build here before.
extern const lv_font_t ui_font_montserrat_12;

#if !defined(ARCH_PORTDUINO)
#include <esp_heap_caps.h>
#define NOTIF_PSRAM 1
#else
#define NOTIF_PSRAM 0
#endif

// --- what the rest of the firmware calls ---
extern "C" void notif_add(uint32_t from, uint8_t ch, bool isChannel, const char *who, const char *text);
extern "C" void notif_init(void);      // build the pop-up up front, on the UI task
extern "C" void notif_open(void);      // the Notifications page
extern "C" int notif_count(void);      // how many are being held
extern "C" void notif_clear(void);     // "clear" button, and whenever messages are read
extern "C" bool notif_peek(int i, char *who, size_t whoN, char *text, size_t textN, uint32_t *ageSecs);
extern "C" bool notif_unread_from(uint32_t nodeNum); // does this node have something unread?
// Drop the entries for ONE conversation. isChannel picks which of from/ch identifies it.
extern "C" void notif_clear_one(uint32_t from, uint8_t ch, bool isChannel);

// --- MUI shims (TFTView_320x240.cpp) ---
extern "C" void tui_open_chat_with(uint32_t nodeNum);
extern "C" void tui_open_channel_chat(uint8_t ch);
extern "C" bool tdeck_lockscreen_active(void);
// Is the PIN/swipe still owed? This page can now be opened FROM the lock screen (the
// "Notifications" button in its top-left corner), so it has to know.
extern "C" bool tdeck_device_locked(void);
// "Clear" says it clears everything, so it clears the per-conversation counts too.
// unread_clear_all has C++ linkage, so it comes in by header rather than being re-declared
// extern "C" here - which is what the linker objected to.
extern "C" void tui_unread_recount(void); // push the new total into the top bar
extern "C" void tdeck_pop_request(void); // TDeckPop.cpp - the pop. Safe from any task.

namespace
{
// Sixteen is plenty: this is "what have I missed", not a message archive — the
// conversations themselves are MUI's job and it already keeps them.
const int kMax = 16;

struct Notif {
    uint32_t from;     // sender node number
    uint8_t ch;        // channel it came in on
    bool isChannel;    // true = a group message, so opening it opens the channel
    uint32_t when;     // lv_tick at arrival, for "3m ago"
    char who[24];      // sender's name as MUI shows it
    char text[56];     // the start of the message
};

Notif *store = nullptr; // kMax entries, PSRAM
int count = 0;          // how many are live, newest LAST

lv_obj_t *screen = nullptr;
lv_obj_t *listCont = nullptr;
lv_obj_t *emptyLbl = nullptr;
lv_obj_t *prevScreen = nullptr;
lv_obj_t *lockedLbl = nullptr; // "Unlock to open a conversation"
lv_obj_t *launcherScreen = nullptr; // captured at notif_init(): the always-safe way back

lv_obj_t *popup = nullptr;
lv_obj_t *popupWho = nullptr;
lv_obj_t *popupText = nullptr;
lv_timer_t *popupTimer = nullptr;

bool ensureStore(void)
{
    if (store)
        return true;
#if NOTIF_PSRAM
    store = (Notif *)heap_caps_calloc(kMax, sizeof(Notif), MALLOC_CAP_SPIRAM);
#else
    store = (Notif *)calloc(kMax, sizeof(Notif));
#endif
    return store != nullptr;
}

void ageText(uint32_t sinceTick, char *out, size_t n)
{
    uint32_t secs = (lv_tick_get() - sinceTick) / 1000;
    if (secs < 60)
        snprintf(out, n, "just now");
    else if (secs < 3600)
        snprintf(out, n, "%um ago", (unsigned)(secs / 60));
    else
        snprintf(out, n, "%uh ago", (unsigned)(secs / 3600));
}

// ---------------------------------------------------------------- the pop-up

void hidePopup(void)
{
    if (popup)
        lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);
}

void buildPopup(void)
{
    popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(popup, 300, 56);
    lv_obj_align(popup, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_color(popup, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(popup, lv_color_hex(0x30d158), LV_PART_MAIN);
    lv_obj_set_style_border_width(popup, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(popup, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(popup, 0, LV_PART_MAIN);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);
    // Tapping the card opens the Notifications page; the little x just dismisses it.
    lv_obj_add_flag(popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        popup,
        [](lv_event_t *) {
            hidePopup();
            // Deferred: this handler is running on an object that opening a screen
            // would tear the ground out from under. Same rule as everywhere else here.
            lv_async_call([](void *) { notif_open(); }, nullptr);
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *dot = lv_obj_create(popup);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x30d158), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 10, 11);

    popupWho = lv_label_create(popup);
    lv_obj_set_style_text_color(popupWho, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(popupWho, LV_ALIGN_TOP_LEFT, 26, 6);
    lv_obj_set_width(popupWho, 230);
    lv_label_set_long_mode(popupWho, LV_LABEL_LONG_DOT);
    tui_one_line(popupWho); // LONG_DOT needs a pinned height - see TuiLabel.h

    popupText = lv_label_create(popup);
    lv_obj_set_style_text_color(popupText, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_set_style_text_font(popupText, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(popupText, LV_ALIGN_TOP_LEFT, 26, 30);
    lv_obj_set_width(popupText, 230);
    lv_label_set_long_mode(popupText, LV_LABEL_LONG_DOT);
    tui_one_line(popupText); // LONG_DOT needs a pinned height - see TuiLabel.h

    lv_obj_t *x = lv_btn_create(popup);
    lv_obj_set_size(x, 34, 34);
    lv_obj_align(x, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
    lv_obj_set_style_radius(x, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(x, [](lv_event_t *) { hidePopup(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *xl = lv_label_create(x);
    lv_label_set_text(xl, "x");
    lv_obj_center(xl);

    // Auto-dismiss. A card that sat there forever would be in the way of whatever you
    // were actually doing; the count and the Notifications page keep the information.
    //
    // ⚠️ NOT lv_timer_set_repeat_count(t, 1). In this LVGL, a timer whose repeat count
    // reaches 0 is DELETED (lv_timer.c: "The repeat count is over, delete the timer"),
    // and auto_delete is on by default — so popupTimer would dangle after the first
    // notification and the second one would reset freed memory. Infinite repeat, paused
    // when idle, and the callback pauses itself: the pointer stays valid for good.
    popupTimer = lv_timer_create(
        [](lv_timer_t *t) {
            hidePopup();
            lv_timer_pause(t);
        },
        6000, nullptr);
    lv_timer_pause(popupTimer);
}

void showPopup(const Notif &n)
{
    if (!popup) // notif_init() should have built it; never build one from the mesh task
        return;
    char head[48];
    snprintf(head, sizeof(head), "%s", n.who[0] ? n.who : "Someone");
    lv_label_set_text(popupWho, head);
    lv_label_set_text(popupText, n.text);
    lv_obj_move_foreground(popup);
    lv_obj_clear_flag(popup, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(popupTimer);   // a second message restarts the six seconds
    lv_timer_resume(popupTimer);
}

// ---------------------------------------------------------------- the page

void rebuild(void);

void onRowTap(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!store || idx < 0 || idx >= count)
        return;
    // ⛔ READ-ONLY WHILE LOCKED, DELIBERATELY. The lock screen shows who has messaged and the
    // first line, and its "Notifications" button opens this page - so you can see WHAT is
    // waiting without the code. Walking from here into the conversation would be a way past
    // the PIN, and a lock you can step around is decoration. Jake has not asked for a bypass
    // and this will not build one on its own.
    if (tdeck_device_locked()) {
        if (lockedLbl) {
            lv_label_set_text(lockedLbl, "Unlock first - slide, then your code");
            lv_obj_set_style_text_color(lockedLbl, lv_color_hex(0xff9f0a), LV_PART_MAIN);
        }
        return;
    }
    // Copy out BEFORE anything can rebuild or clear the store — the row we are
    // standing on is about to be destroyed by the screen change.
    const uint32_t from = store[idx].from;
    const uint8_t ch = store[idx].ch;
    const bool isCh = store[idx].isChannel;
    // Opening the conversation is exactly what "read it" means, so the list empties.
    notif_clear();
    lv_async_call(
        [](void *ud) {
            const uint32_t packed = (uint32_t)(uintptr_t)ud;
            if (packed & 0x80000000u)
                tui_open_channel_chat((uint8_t)(packed & 0xffu));
            else
                tui_open_chat_with(packed);
        },
        (void *)(uintptr_t)(isCh ? (0x80000000u | ch) : from));
}

lv_obj_t *barBtn(lv_obj_t *parent, const char *txt, int w, lv_align_t align, int xofs, uint32_t color,
                 lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, 28);
    lv_obj_align(b, align, xofs, 3);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_obj_center(l);
    return b;
}

void buildScreen(void)
{
    screen = lv_obj_create(NULL);
    tui_statusbar_reserve(screen); // note item A4: the persistent top bar
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // Jake asked for both: a Close (leave the list alone) and a Clear (empty it).
    barBtn(screen, "Close", 62, LV_ALIGN_TOP_LEFT, 4, 0x2c2c2e, [](lv_event_t *) {
        lv_async_call(
            [](void *) {
                // The screen we came from may be GONE — a Lua app deletes luaScreen when it
                // closes — and loading a deleted screen is not a graceful failure. Check,
                // and otherwise go where Home goes.
                if (prevScreen && lv_obj_is_valid(prevScreen))
                    lv_screen_load_anim(prevScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
                else if (launcherScreen && lv_obj_is_valid(launcherScreen))
                    lv_screen_load_anim(launcherScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
                prevScreen = nullptr;
            },
            nullptr);
    });

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Notifications");
    lv_obj_set_style_text_color(title, lv_color_hex(0x30d158), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    barBtn(screen, "Clear", 62, LV_ALIGN_TOP_RIGHT, -4, 0x8a2a24, [](lv_event_t *) {
        // Everything means everything: the list AND every conversation's unread count.
        // Leaving badges standing after you have explicitly said you are done would be the
        // same kind of half-truth the single global counter used to tell.
        unread_clear_all();
        tui_unread_recount();
        notif_clear(); // schedules its own rebuild, deferred
    });

    listCont = lv_obj_create(screen);
    lv_obj_remove_style_all(listCont);
    lv_obj_set_size(listCont, 320, 184); // -20: the status bar took 20px off the top (tui_statusbar_reserve)
    lv_obj_align(listCont, LV_ALIGN_TOP_LEFT, 0, 36);
    lv_obj_set_flex_flow(listCont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(listCont, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(listCont, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(listCont, LV_DIR_VER);

    // Shown only when this page was opened from the lock screen. See onRowTap.
    lockedLbl = lv_label_create(screen);
    lv_label_set_text(lockedLbl, "Unlock to open a conversation");
    lv_obj_set_style_text_font(lockedLbl, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(lockedLbl, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(lockedLbl, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_flag(lockedLbl, LV_OBJ_FLAG_HIDDEN);

    emptyLbl = lv_label_create(screen);
    lv_label_set_text(emptyLbl, "Nothing new.\nMessages you have not read yet\nshow up here.");
    lv_obj_set_style_text_color(emptyLbl, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(emptyLbl, LV_ALIGN_CENTER, 0, 10);
}

void rebuild(void)
{
    lv_obj_clean(listCont);
    if (count == 0 || !store) {
        lv_obj_clear_flag(emptyLbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(emptyLbl, LV_OBJ_FLAG_HIDDEN);

    // Newest first: the thing that just arrived is the thing being looked for.
    for (int i = count - 1; i >= 0; i--) {
        lv_obj_t *row = lv_obj_create(listCont);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 296, 52);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, onRowTap, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *who = lv_label_create(row);
        lv_label_set_text(who, store[i].who[0] ? store[i].who : "Someone");
        lv_obj_set_style_text_color(who, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_width(who, 200);
        lv_label_set_long_mode(who, LV_LABEL_LONG_DOT);
        tui_one_line(who); // LONG_DOT needs a pinned height - see TuiLabel.h
        lv_obj_align(who, LV_ALIGN_TOP_LEFT, 10, 5);

        // A group message and a direct one are different things; say which.
        if (store[i].isChannel) {
            lv_obj_t *tag = lv_label_create(row);
            lv_label_set_text(tag, "group");
            lv_obj_set_style_text_font(tag, &ui_font_montserrat_12, LV_PART_MAIN);
            lv_obj_set_style_text_color(tag, lv_color_hex(0x5ac8fa), LV_PART_MAIN);
            lv_obj_align(tag, LV_ALIGN_TOP_RIGHT, -56, 7);
        }

        char age[16];
        ageText(store[i].when, age, sizeof(age));
        lv_obj_t *ag = lv_label_create(row);
        lv_label_set_text(ag, age);
        lv_obj_set_style_text_font(ag, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(ag, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_obj_align(ag, LV_ALIGN_TOP_RIGHT, -8, 7);

        lv_obj_t *tx = lv_label_create(row);
        lv_label_set_text(tx, store[i].text);
        lv_obj_set_style_text_font(tx, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(tx, lv_color_hex(0xc7c7cc), LV_PART_MAIN);
        lv_obj_set_width(tx, 276);
        lv_label_set_long_mode(tx, LV_LABEL_LONG_DOT);
        tui_one_line(tx); // LONG_DOT needs a pinned height - see TuiLabel.h
        lv_obj_align(tx, LV_ALIGN_TOP_LEFT, 10, 28);
    }
}
} // namespace

// ---------------------------------------------------------------- public doors

// ⚠️ WHICH TASK THIS RUNS ON, because it is easy to get wrong and I did at first.
//
// It is the "tft" task - the SAME one that runs LVGL. tftSetup.cpp creates exactly one:
//     while (true) { spiLock->lock(); deviceScreen->task_handler(); spiLock->unlock(); ... }
// and MeshtasticView::task_handler() calls DeviceGUI::task_handler() (lv_timer_handler) and
// then controller->runOnce() (which drains the packet queue -> packetReceived -> newMessage)
// one after the other. The radio task only hands packets to a queue; it never calls in here.
//
// So calling LVGL from this function is safe. Two other things are NOT:
//
//  1. THE SPI LOCK IS HELD for the whole of task_handler(). Anything slow here - playing a
//     sound to completion, writing to the SD card - holds the bus the radio needs, and that
//     is the documented cause of the RadioIf stalls that reboot this device. Hence
//     tdeck_pop_request(), which only sets a flag for the main loop.
//  2. DESTROYING A WIDGET INSIDE ITS OWN EVENT HANDLER still hangs LVGL, task or no task.
//     That is why the rebuilds below go through lv_async_call.
extern "C" void notif_add(uint32_t from, uint8_t ch, bool isChannel, const char *who, const char *text)
{
    if (!ensureStore())
        return;

    if (count == kMax) { // oldest falls off the front
        memmove(&store[0], &store[1], sizeof(Notif) * (kMax - 1));
        count--;
    }
    Notif &n = store[count++];
    n.from = from;
    n.ch = ch;
    n.isChannel = isChannel;
    n.when = lv_tick_get();
    snprintf(n.who, sizeof(n.who), "%s", who ? who : "");
    snprintf(n.text, sizeof(n.text), "%s", text ? text : "");

    // The sound plays wherever you are, locked or not - being locked is exactly when you
    // most need telling. Only a REQUEST: the main loop starts it. Playing it here would hold
    // the SPI lock (see above) for the length of the sound, with the radio waiting.
    tdeck_pop_request();

    // A lock screen shows this store itself, and floating a card over the PIN pad would
    // both cover the keys and leak the message to whoever picked the device up.
    if (!tdeck_lockscreen_active())
        showPopup(n);

    // Rebuilding the page deletes and recreates rows. Deferred not for thread reasons but
    // because notif_clear() shares this path with onRowTap(), which is a row's own handler.
    if (screen && lv_screen_active() == screen)
        lv_async_call([](void *) { if (screen && lv_screen_active() == screen) rebuild(); }, nullptr);
}

// Called from createLauncher(). Builds the pop-up up front so that the first notification
// does not have to, which keeps notif_add() short - see the note on the SPI lock above.
extern "C" void notif_init(void)
{
    ensureStore();
    // notif_init() is called from createLauncher(), so the active screen IS the launcher.
    // Hold on to it as the fallback for Close when the screen we came from has been deleted.
    if (!launcherScreen)
        launcherScreen = lv_screen_active();
    if (!popup)
        buildPopup();
}

extern "C" int notif_count(void) { return count; }

// Drop just this conversation's entries and close up the gap, keeping the newest-last order
// the rest of the file relies on. Opening one chat should not wipe the notice that somebody
// ELSE messaged you - that is the same mistake the device-wide unread counter made.
extern "C" void notif_clear_one(uint32_t from, uint8_t ch, bool isChannel)
{
    if (!store || count == 0)
        return;
    int w = 0;
    for (int r = 0; r < count; r++) {
        const bool mine = isChannel ? (store[r].isChannel && store[r].ch == ch)
                                    : (!store[r].isChannel && store[r].from == from);
        if (mine)
            continue;
        if (w != r)
            store[w] = store[r];
        w++;
    }
    if (w == count)
        return; // nothing matched, so nothing to redraw
    count = w;
    if (count == 0)
        hidePopup();
    // Deferred for the same two reasons notif_clear() documents below: this can be called
    // from a row's own handler, and from message handling mid-refresh with the SPI lock held.
    if (screen && lv_screen_active() == screen)
        lv_async_call([](void *) { if (screen && lv_screen_active() == screen) rebuild(); }, nullptr);
}

extern "C" void notif_clear(void)
{
    count = 0;
    hidePopup(); // a flag, safe from any task and safe inside any handler

    // ⚠️ THE REBUILD MUST BE DEFERRED, for two separate reasons, either one fatal:
    //   1. onRowTap() calls this, and rebuild() starts with lv_obj_clean(listCont) — which
    //      would destroy the row whose click handler is running. LVGL then walks freed
    //      memory and HANGS rather than panicking; that exact mistake once cost 250 nodes
    //      and every favourite, because the freeze landed mid-way through a flash write.
    //   2. it is also called straight from message handling, mid-refresh, with the SPI lock
    //      held - the wrong moment to be tearing a list down and rebuilding it.
    // lv_async_call answers both: it runs after the current handler has returned.
    if (screen && lv_screen_active() == screen)
        lv_async_call([](void *) { if (screen && lv_screen_active() == screen) rebuild(); }, nullptr);
}

// Read one out, newest first, so the lock screen can draw the same list without
// needing to know anything about how it is stored.
extern "C" bool notif_peek(int i, char *who, size_t whoN, char *text, size_t textN, uint32_t *ageSecs)
{
    if (!store || i < 0 || i >= count)
        return false;
    const Notif &n = store[count - 1 - i];
    if (who && whoN)
        snprintf(who, whoN, "%s", n.who[0] ? n.who : "Someone");
    if (text && textN)
        snprintf(text, textN, "%s", n.text);
    if (ageSecs)
        *ageSecs = (lv_tick_get() - n.when) / 1000;
    return true;
}

// Jake: "fav app needs to show who sent somthing if unread". The Nodes and Favorites rows
// ask this per node. Linear over at most 16 entries, called while building a list - far
// cheaper than keeping a second index in step with the store.
extern "C" bool notif_unread_from(uint32_t nodeNum)
{
    if (!store || !nodeNum)
        return false;
    for (int i = 0; i < count; i++)
        if (!store[i].isChannel && store[i].from == nodeNum)
            return true;
    return false;
}

extern "C" void notif_open(void)
{
    lv_obj_t *active = lv_screen_active();
    if (active != screen)
        prevScreen = active;
    if (!screen)
        buildScreen();
    hidePopup();
    rebuild();
    // The footer only earns its 18px when it has something to say.
    const bool locked = tdeck_device_locked();
    if (lockedLbl) {
        lv_label_set_text(lockedLbl, "Unlock to open a conversation");
        lv_obj_set_style_text_color(lockedLbl, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        if (locked)
            lv_obj_clear_flag(lockedLbl, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(lockedLbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (listCont)
        lv_obj_set_height(listCont, locked ? 166 : 184); // -20: the status bar took 20px off the top (tui_statusbar_reserve)
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}
