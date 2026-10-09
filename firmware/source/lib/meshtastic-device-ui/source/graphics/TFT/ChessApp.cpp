#include "graphics/view/TFT/TuiStatusBar.h"
#include "lvgl.h"
#include <cstdio>
#include <cstring>

// -----------------------------------------------------------------------------------------
// CHESS - the board. The game and the engine live in src/TDeckChess.cpp and src/chess/.
//
// Jake, 2026-09-22: "local chess with a chess ai kinda thing with difficulty settings up to
// master". Measured on this chip the engine reaches about depth 6-7 in a couple of seconds,
// which is a decent club player - so the levels are named for how they play, not with a rating
// I have not earned the right to print.
//
// ⛔ LAYOUT IS RELATIVE TO THE CONTENT AREA, NOT THE SCREEN. tui_statusbar_reserve() adds 20px
// of top padding, so children start at y=0 and the usable height is 220, not 240. Placing
// things at y=26 "to clear the status bar" double-counts it and pushes the bottom row off the
// screen - which is exactly what happened to the Mail app earlier today.
//
// 8 squares of 24px = 192, leaving 124 across for the side panel and 28 down for the status.
// -----------------------------------------------------------------------------------------

extern "C" {
void tdeck_chess_new_game(void);
unsigned char tdeck_chess_piece_at(int sq64);
bool tdeck_chess_highlighted(int sq64);
int tdeck_chess_moves_from(int sq64, unsigned char *dests, int cap);
bool tdeck_chess_try_move(int from64, int to64);
bool tdeck_chess_player_turn(void);
bool tdeck_chess_thinking(void);
bool tdeck_chess_request_engine(void);
bool tdeck_chess_undo(void);
int tdeck_chess_result(void);
const char *tdeck_chess_status(void);
int tdeck_chess_level(void);
void tdeck_chess_set_level(int lv);
const char *tdeck_chess_level_name(void);
void chess_open(void);
void chess_idle_check(void);
void tdeck_chess_release(void);
void tdeck_chess_resume(void);
void tdeck_chess_service(void);
uint32_t tdeck_chess_version(void);
}

namespace
{
// ⭐ CENTRED. 8 x 24 = 192 on a 320 screen leaves exactly 64 either side, and those margins
// then carry the two things that used to need a whole side panel: the level on the left and the
// "space for menu" hint on the right. Jake: "would be nice to center the board".
// Vertically the content area is 220 (the status bar takes 20 of the 240), so 4 at the top and
// the status line in the 24 left at the bottom.
const int kSq = 24;
const int kBoardX = (320 - 8 * 24) / 2; // 64
const int kBoardY = 4;

lv_obj_t *screen = nullptr;
lv_obj_t *sqObj[64] = {nullptr};   // the squares
lv_obj_t *pcLbl[64] = {nullptr};   // the piece sitting on each, hidden when empty
lv_obj_t *statusLbl = nullptr;
lv_obj_t *levelLbl = nullptr;
lv_obj_t *menuBox = nullptr;   // the spacebar overlay
lv_obj_t *menuLevelLbl = nullptr;
lv_obj_t *keyCatcher = nullptr;
lv_timer_t *tick = nullptr;

int selected = -1;                 // square the player has picked up, or -1
unsigned char dests[32];
int destN = 0;
uint32_t shownVersion = 0;         // the game version last painted - see the tick below

// Light and dark squares, and the two "something is happening here" tints.
const uint32_t kLight = 0xE8D4B0, kDark = 0x9A7248;
const uint32_t kLightSel = 0xC9D98A, kDarkSel = 0x8FA85A;

// ⭐ PIECES ARE LETTERS ON A DISC, and that is a considered choice rather than laziness.
// The fonts on this device have no chess glyphs and generating one is a bigger detour than the
// whole app. Letters alone do not work - white text is invisible on a light square - so each
// piece gets a filled circle behind it in its own colour, with the letter in the contrasting
// one. That reads unambiguously at 24px and costs one object per piece.
const char *kLetters = " PNBRQK";

bool menuOpen(void); // defined below; refreshBoard() needs it
void showMenu(bool on);

int sqAt(int file, int rank) { return rank * 8 + file; }

// ⭐ ONLY TOUCH WHAT CHANGED. Setting an LVGL style invalidates the object whether or not the
// value actually differs, so the first version repainted all 128 objects - the whole screen -
// on every single tap. Remembering the last colour and piece per square turns a full redraw
// into two or three squares, which is what a chess move actually changes.
uint32_t lastColour[64] = {0};
uint16_t lastPiece[64] = {0xFFFF};

void refreshBoard(void)
{
    for (int i = 0; i < 64; i++) {
        const int file = i % 8, rank = i / 8;
        const bool lightSq = ((file + rank) & 1) != 0;
        const bool lit = tdeck_chess_highlighted(i) || (selected >= 0 && i == selected);
        bool isDest = false;
        for (int d = 0; d < destN; d++)
            if (dests[d] == i)
                isDest = true;
        uint32_t c = lightSq ? kLight : kDark;
        if (lit || isDest)
            c = lightSq ? kLightSel : kDarkSel;
        if (c != lastColour[i]) {
            lastColour[i] = c;
            lv_obj_set_style_bg_color(sqObj[i], lv_color_hex(c), LV_PART_MAIN);
        }

        const unsigned char pc = tdeck_chess_piece_at(i);
        if (pc == lastPiece[i])
            continue; // this square is already showing exactly this
        lastPiece[i] = pc;
        if (!pc) {
            lv_obj_add_flag(pcLbl[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const int type = pc & 7;
        const bool white = ((pc >> 3) & 1) == 0;
        char txt[2] = {kLetters[type], 0};
        lv_label_set_text(pcLbl[i], txt);
        lv_obj_set_style_bg_color(pcLbl[i], lv_color_hex(white ? 0xFFFFFF : 0x202020), LV_PART_MAIN);
        lv_obj_set_style_text_color(pcLbl[i], lv_color_hex(white ? 0x202020 : 0xFFFFFF), LV_PART_MAIN);
        lv_obj_clear_flag(pcLbl[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (statusLbl)
        lv_label_set_text(statusLbl, tdeck_chess_status());
    if (levelLbl)
        lv_label_set_text(levelLbl, tdeck_chess_level_name());
    if (menuLevelLbl && menuOpen())
        lv_label_set_text_fmt(menuLevelLbl, "Level: %s", tdeck_chess_level_name());
}

void showMenu(bool on)
{
    if (!menuBox)
        return;
    if (on) {
        if (menuLevelLbl)
            lv_label_set_text_fmt(menuLevelLbl, "Level: %s", tdeck_chess_level_name());
        lv_obj_clear_flag(menuBox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(menuBox);
    } else {
        lv_obj_add_flag(menuBox, LV_OBJ_FLAG_HIDDEN);
    }
}

bool menuOpen(void)
{
    return menuBox && !lv_obj_has_flag(menuBox, LV_OBJ_FLAG_HIDDEN);
}

void keyEvent(lv_event_t *e)
{
    const uint32_t k = lv_event_get_key(e);
    // Space opens it, space or Esc closes it. Anything that opens a menu should close it with
    // the same key - having to hunt for the exit is worse than having no shortcut.
    if (k == ' ')
        showMenu(!menuOpen());
    else if (k == LV_KEY_ESC && menuOpen())
        showMenu(false);
}

void onSquare(lv_event_t *e)
{
    if (menuOpen() || tdeck_chess_thinking() || tdeck_chess_result())
        return;
    // Its turn but nothing is thinking: a reloaded game, or a think that could not start for
    // want of memory (the status line says so). Any tap on the board is "try again".
    if (!tdeck_chess_player_turn()) {
        tdeck_chess_request_engine();
        refreshBoard();
        return;
    }
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (selected < 0) {
        destN = tdeck_chess_moves_from(i, dests, sizeof(dests));
        selected = destN ? i : -1;
        refreshBoard();
        return;
    }
    if (i == selected) { // tapping it again puts the piece back down
        selected = -1;
        destN = 0;
        tdeck_chess_moves_from(-1, dests, 0);
        refreshBoard();
        return;
    }
    if (tdeck_chess_try_move(selected, i)) {
        selected = -1;
        destN = 0;
        refreshBoard();
        // Let the board repaint with the player's move BEFORE the engine starts thinking -
        // otherwise the screen sits on the old position for the whole search and it looks
        // like the tap was ignored.
        lv_async_call([](void *) { tdeck_chess_request_engine(); }, nullptr);
        return;
    }
    // Not a legal destination: treat it as picking up a different piece instead of an error.
    destN = tdeck_chess_moves_from(i, dests, sizeof(dests));
    selected = destN ? i : -1;
    refreshBoard();
}

} // namespace

extern "C" void chess_open(void)
{
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x101010), LV_PART_MAIN);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        tui_statusbar_reserve(screen);

        for (int i = 0; i < 64; i++) {
            const int file = i % 8, rank = i / 8;
            lv_obj_t *s = lv_obj_create(screen);
            lv_obj_remove_style_all(s);
            // rank 0 is White's back rank and must be at the BOTTOM of the screen, so the row
            // is flipped when positioning. Getting this wrong gives a board that plays
            // correctly and looks upside down.
            lv_obj_set_pos(s, kBoardX + file * kSq, kBoardY + (7 - rank) * kSq);
            lv_obj_set_size(s, kSq, kSq);
            lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s, onSquare, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            sqObj[i] = s;

            lv_obj_t *p = lv_label_create(s);
            lv_obj_set_size(p, 20, 20);
            lv_obj_center(p);
            lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(p, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_pad_top(p, 3, LV_PART_MAIN);
            lv_obj_set_style_text_font(p, &ui_font_montserrat_12, LV_PART_MAIN);
            lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
            pcLbl[i] = p;
        }

        // Left margin: which level you are playing.
        levelLbl = lv_label_create(screen);
        lv_obj_set_pos(levelLbl, 2, 6);
        lv_obj_set_size(levelLbl, 58, 30);
        lv_label_set_long_mode(levelLbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(levelLbl, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(levelLbl, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_label_set_text(levelLbl, "");

        // Right margin: the only thing telling you the menu exists, so it is always on screen.
        lv_obj_t *hint = lv_label_create(screen);
        lv_obj_set_pos(hint, 260, 6);
        lv_obj_set_size(hint, 58, 44);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(hint, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x5a5a5e), LV_PART_MAIN);
        lv_label_set_text(hint, "space for menu");

        // The status line sits under the board in the 24px the centring leaves.
        statusLbl = lv_label_create(screen);
        lv_obj_set_pos(statusLbl, 4, 198);
        // ⛔ Bounded. The status carries engine output of unpredictable length ("e2e4 depth 6
        // +0.3", "Checkmate - you lose"), and a wrapping label with no height walks off screen.
        lv_obj_set_size(statusLbl, 312, 20);
        lv_label_set_long_mode(statusLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(statusLbl, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(statusLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_label_set_text(statusLbl, "");

        // ---------- the spacebar menu ----------
        menuBox = lv_obj_create(screen);
        lv_obj_set_size(menuBox, 220, 176);
        lv_obj_align(menuBox, LV_ALIGN_CENTER, 0, -6);
        lv_obj_set_style_bg_color(menuBox, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_border_color(menuBox, lv_color_hex(0x48484a), LV_PART_MAIN);
        lv_obj_set_style_border_width(menuBox, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(menuBox, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_all(menuBox, 8, LV_PART_MAIN);
        lv_obj_clear_flag(menuBox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(menuBox, LV_OBJ_FLAG_HIDDEN);
        {
            auto row = [&](const char *txt, int y, uint32_t colour, lv_event_cb_t cb, lv_obj_t **out) {
                lv_obj_t *b = lv_btn_create(menuBox);
                lv_obj_set_size(b, 200, 34);
                lv_obj_set_pos(b, 0, y);
                lv_obj_set_style_bg_color(b, lv_color_hex(colour), LV_PART_MAIN);
                lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
                lv_obj_t *l = lv_label_create(b);
                lv_label_set_text(l, txt);
                lv_obj_set_style_text_font(l, &ui_font_montserrat_12, LV_PART_MAIN);
                lv_obj_center(l);
                lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
                if (out)
                    *out = l;
            };
            row("Level", 0, 0x3a3a3c,
                [](lv_event_t *) {
                    tdeck_chess_set_level((tdeck_chess_level() + 1) % 6);
                    if (menuLevelLbl)
                        lv_label_set_text_fmt(menuLevelLbl, "Level: %s", tdeck_chess_level_name());
                    refreshBoard();
                },
                &menuLevelLbl);
            row("New game", 38, 0x0a84ff,
                [](lv_event_t *) {
                    tdeck_chess_new_game();
                    selected = -1;
                    destN = 0;
                    showMenu(false);
                    refreshBoard();
                },
                nullptr);
            row("Take back", 76, 0x3a3a3c,
                [](lv_event_t *) {
                    tdeck_chess_undo();
                    selected = -1;
                    destN = 0;
                    showMenu(false);
                    refreshBoard();
                },
                nullptr);
            row("Close", 114, 0x2c2c2e, [](lv_event_t *) { showMenu(false); }, nullptr);
        }

        // Invisible key sink in the input group, so the spacebar reaches this screen. Same
        // pattern as SnakeGame and the calculator.
        keyCatcher = lv_obj_create(screen);
        lv_obj_remove_style_all(keyCatcher);
        lv_obj_set_size(keyCatcher, 1, 1);
        lv_obj_add_event_cb(keyCatcher, keyEvent, LV_EVENT_KEY, nullptr);
        if (lv_group_get_default())
            lv_group_add_obj(lv_group_get_default(), keyCatcher);

        // Repaint when the game changes. The engine's move lands from the UI poll
        // (chess_idle_check -> tdeck_chess_service), so this has to notice rather than be told.
        // ⛔ BY VERSION, NOT BY THE THINKING FLAG. Watching "thinking" go true -> false can miss a
        // move entirely if both edges fall between two ticks, and then the board sat on the old
        // position until you touched it.
        tick = lv_timer_create(
            [](lv_timer_t *) {
                const uint32_t v = tdeck_chess_version();
                if (v != shownVersion) {
                    shownVersion = v;
                    refreshBoard();
                }
                // Hold onto keyboard focus while this screen is up, or the spacebar stops
                // working the moment anything else takes it.
                if (keyCatcher && lv_screen_active() == screen && lv_group_get_default() &&
                    lv_group_get_focused(lv_group_get_default()) != keyCatcher)
                    lv_group_focus_obj(keyCatcher);
            },
            250, nullptr);
    }
    selected = -1;
    destN = 0;
    // ⛔ The cache describes objects that no longer exist after a teardown. Clearing it forces
    // the first paint to actually draw, instead of skipping every square as "unchanged".
    for (int i = 0; i < 64; i++) {
        lastColour[i] = 0;
        lastPiece[i] = 0xFFFF;
    }
    showMenu(false);
    refreshBoard();
    shownVersion = tdeck_chess_version();
    lv_screen_load(screen);
    // A game saved with the engine to move - left mid-think, or a device that restarted - picks up
    // where it was: it is ITS move, so it thinks. Deferred so the board is on screen first.
    lv_async_call([](void *) { tdeck_chess_resume(); }, nullptr);
    // ⛔ ADDING THE SINK TO THE GROUP IS NOT ENOUGH - LVGL delivers keys to the FOCUSED object,
    // and without this the spacebar went to whatever was focused on some other screen. The tick
    // below re-takes focus too, because TFTView's poll has its own focus keeper that repoints
    // the group at the active screen and would quietly take it back.
    if (keyCatcher && lv_group_get_default())
        lv_group_focus_obj(keyCatcher);
}

// Hand the board's memory back once it has been left alone, exactly as Gemini does. 64 squares
// and 64 piece labels is the largest object count of any app here, so this one matters most.
extern "C" void chess_idle_check(void)
{
    // FIRST, and whether or not the board is on screen: land a finished think. This runs on the
    // UI task, which is the only task that touches the game - and the one that already holds the
    // SPI lock the save file needs. A move finished while the app is put away is still played and
    // saved, and only then can the teardown below go ahead.
    tdeck_chess_service();

    static uint32_t idleSince = 0;
    if (!screen)
        return;
    if (lv_screen_active() == screen || tdeck_chess_thinking()) {
        idleSince = 0;
        return;
    }
    if (!idleSince) {
        idleSince = lv_tick_get();
        return;
    }
    if (lv_tick_get() - idleSince < 5000)
        return;
    idleSince = 0;
    if (tick) {
        lv_timer_delete(tick);
        tick = nullptr;
    }
    lv_obj_delete(screen); // takes the squares and piece labels with it
    screen = nullptr;
    statusLbl = levelLbl = menuBox = menuLevelLbl = keyCatcher = nullptr;
    memset(sqObj, 0, sizeof(sqObj));
    memset(pcLbl, 0, sizeof(pcLbl));
    tdeck_chess_release(); // and the 10KB thinking task with them
    // ⭐ The GAME is not reset - only the screen. Come back and the position is exactly as you
    // left it, which is the whole point of a chess game you play across a day.
}
