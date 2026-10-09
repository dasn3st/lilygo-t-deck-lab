// -----------------------------------------------------------------------------
// T-UI Snake — a self-contained LVGL game for the T-Deck launcher.
//
// Controls: W/A/S/D (or arrow keys) on the physical keyboard, OR swipe anywhere
// on the playfield in the direction you want to turn. The first input starts the
// snake moving. Trackball double-click = Home, same as everywhere (the launcher's
// global gesture handles it; nothing here needs to). After a game over, tap the
// field (or press Enter) to play again.
//
// Rendering is one small lv_obj per occupied cell: each tick creates one 12x12
// square at the new head and deletes the one at the vacated tail — no canvas,
// no big draw buffer.
// -----------------------------------------------------------------------------
#include "graphics/common/SdCard.h" // SDFs — high score persists on the SD card
#include "lvgl.h"
#include <cstdio>
#include <cstdlib>
#include <string.h>

extern "C" void snake_open(void);

namespace
{
constexpr int kCell = 12; // bigger snake (was 10)
constexpr int kCols = 26;
constexpr int kRows = 18; // 312x216 field below a 24px header
constexpr int kMax = kCols * kRows;
constexpr uint32_t kStartPeriodMs = 160;
constexpr uint32_t kMinPeriodMs = 70;

lv_obj_t *screen = nullptr;
lv_obj_t *field = nullptr;
lv_obj_t *scoreLbl = nullptr;
lv_obj_t *bestLbl = nullptr; // all-time best, always visible top-right
lv_obj_t *hintLbl = nullptr;
lv_obj_t *keyCatcher = nullptr; // invisible focused widget that receives WASD
lv_timer_t *tick = nullptr;

uint16_t body[kMax];     // ring buffer of occupied cells, tail..head
int headIdx, tailIdx, len;
lv_obj_t *cellObj[kMax]; // the on-screen square for each occupied cell
int foodCell = -1;
lv_obj_t *foodObj = nullptr;
int dir = 0, nextDir = 0; // 0 none, 1 up, 2 down, 3 left, 4 right
bool alive = true;
int score = 0;
int best = 0; // all-time high score, loaded from / saved to the SD card

// High score lives at /apps/snake/hiscore.txt — same .txt-in-the-app-folder
// pattern the Lua games use. (A folder with no main.lua never becomes a tile.)
constexpr const char *kBestPath = "/apps/snake/hiscore.txt";

void loadBest(void)
{
    FsFile f = SDFs.open(kBestPath, O_RDONLY);
    if (!f)
        return;
    char buf[16] = {0};
    int n = f.read((uint8_t *)buf, sizeof(buf) - 1);
    f.close();
    if (n > 0)
        best = atoi(buf);
}

void saveBest(void)
{
    SDFs.mkdir("/apps");
    SDFs.mkdir("/apps/snake");
    FsFile f = SDFs.open(kBestPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f)
        return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", best);
    f.write((const uint8_t *)buf, strlen(buf));
    f.sync();
    f.close();
}

// swipe-steer state: track where a touch starts and its latest point so a drag
// across the field turns the snake in the dominant direction of the swipe
lv_point_t touchStart, touchLast;
bool touchTracking = false;
constexpr int kSwipeMinPx = 18; // shorter than this = a tap, not a swipe

int cellX(int c) { return (c % kCols) * kCell; }
int cellY(int c) { return (c / kCols) * kCell; }

lv_obj_t *makeSquare(int c, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(field);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, kCell - 1, kCell - 1); // 1px gap = visible segmentation
    lv_obj_set_pos(o, cellX(c), cellY(c));
    lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(o, 2, LV_PART_MAIN);
    return o;
}

void updateScore(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "Snake  ·  %d", score);
    lv_label_set_text(scoreLbl, buf);
    snprintf(buf, sizeof(buf), "Best %d", best);
    lv_label_set_text(bestLbl, buf);
}

void spawnFood(void)
{
    for (int tries = 0; tries < 500; tries++) {
        int c = (int)lv_rand(0, kMax - 1);
        if (!cellObj[c]) {
            foodCell = c;
            if (!foodObj)
                foodObj = makeSquare(c, 0xff453a);
            lv_obj_set_pos(foodObj, cellX(c), cellY(c));
            return;
        }
    }
    foodCell = -1; // board almost full — you have basically won snake
}

void resetGame(void)
{
    for (int i = 0; i < kMax; i++) {
        if (cellObj[i]) {
            lv_obj_delete(cellObj[i]);
            cellObj[i] = nullptr;
        }
    }
    // 3 segments, centered, heading right (once a key starts it)
    len = 0;
    headIdx = tailIdx = 0;
    int start = (kRows / 2) * kCols + kCols / 2;
    for (int i = 0; i < 3; i++) {
        int c = start - 2 + i;
        body[len++] = c;
        cellObj[c] = makeSquare(c, 0x30d158);
    }
    headIdx = len - 1;
    tailIdx = 0;
    dir = nextDir = 0;
    alive = true;
    score = 0;
    updateScore();
    lv_label_set_text(hintLbl, "swipe or W A S D");
    spawnFood();
}

void gameOver(void)
{
    alive = false;
    char buf[56];
    if (score > best) {
        best = score;
        saveBest();
        updateScore(); // header shows the new best immediately
        snprintf(buf, sizeof(buf), "NEW BEST %d!  ·  tap to retry", score);
    } else {
        snprintf(buf, sizeof(buf), "Game over  ·  score %d  ·  tap to retry", score);
    }
    lv_label_set_text(hintLbl, buf);
    // head flashes white so you can see where it went wrong
    int head = body[headIdx];
    if (cellObj[head])
        lv_obj_set_style_bg_color(cellObj[head], lv_color_hex(0xffffff), LV_PART_MAIN);
}

void queueDir(int d)
{
    if (!alive)
        return;
    // no 180° turns into your own neck
    if ((d == 1 && dir == 2) || (d == 2 && dir == 1) || (d == 3 && dir == 4) || (d == 4 && dir == 3))
        return;
    nextDir = d;
    if (dir == 0) { // first input starts the game
        dir = d;
        lv_label_set_text(hintLbl, "");
    }
}

void step(lv_timer_t *)
{
    // the encoder indev can wander LVGL's focus; keep the keyboard aimed at us
    lv_group_t *g = lv_group_get_default();
    if (g && lv_group_get_focused(g) != keyCatcher)
        lv_group_focus_obj(keyCatcher);

    if (!alive || dir == 0)
        return;
    dir = nextDir;

    int head = body[headIdx];
    int hx = head % kCols, hy = head / kCols;
    if (dir == 1)
        hy--;
    else if (dir == 2)
        hy++;
    else if (dir == 3)
        hx--;
    else
        hx++;

    // wrap around the edges, Pac-Man style: off one side = back on the opposite side
    if (hx < 0)
        hx = kCols - 1;
    else if (hx >= kCols)
        hx = 0;
    if (hy < 0)
        hy = kRows - 1;
    else if (hy >= kRows)
        hy = 0;

    int nc = hy * kCols + hx;
    if (cellObj[nc]) { // bit yourself
        gameOver();
        return;
    }

    headIdx = (headIdx + 1) % kMax;
    body[headIdx] = nc;
    cellObj[nc] = makeSquare(nc, 0x30d158);
    len++;

    if (nc == foodCell) { // grow: keep the tail, speed up a touch
        score++;
        updateScore();
        uint32_t period = kStartPeriodMs - (uint32_t)score * 4;
        lv_timer_set_period(tick, period < kMinPeriodMs ? kMinPeriodMs : period);
        spawnFood();
    } else { // normal move: tail follows
        int tc = body[tailIdx];
        tailIdx = (tailIdx + 1) % kMax;
        lv_obj_delete(cellObj[tc]);
        cellObj[tc] = nullptr;
        len--;
    }
}

void keyEvent(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    if (k == 'w' || k == 'W' || k == LV_KEY_UP)
        queueDir(1);
    else if (k == 's' || k == 'S' || k == LV_KEY_DOWN)
        queueDir(2);
    else if (k == 'a' || k == 'A' || k == LV_KEY_LEFT)
        queueDir(3);
    else if (k == 'd' || k == 'D' || k == LV_KEY_RIGHT)
        queueDir(4);
    else if ((k == LV_KEY_ENTER || k == ' ') && !alive)
        resetGame();
}

void fieldTapped(lv_event_t *)
{
    if (!alive)
        resetGame();
}

// --- swipe steering: record the touch-down point, keep the latest point while
// dragging, then on release turn in the dominant axis of the total movement ---
void fieldPressed(lv_event_t *)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev)
        return;
    lv_indev_get_point(indev, &touchStart);
    touchLast = touchStart;
    touchTracking = true;
}

void fieldPressing(lv_event_t *)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev)
        lv_indev_get_point(indev, &touchLast);
}

void fieldReleased(lv_event_t *)
{
    if (!touchTracking)
        return;
    touchTracking = false;
    int dx = touchLast.x - touchStart.x;
    int dy = touchLast.y - touchStart.y;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (adx < kSwipeMinPx && ady < kSwipeMinPx)
        return; // a tap, not a swipe — fieldTapped handles retry-after-death
    if (adx > ady)
        queueDir(dx > 0 ? 4 : 3); // horizontal swipe: right : left
    else
        queueDir(dy > 0 ? 2 : 1); // vertical swipe: down : up
}

void buildScreen(void)
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    scoreLbl = lv_label_create(screen);
    lv_obj_set_style_text_color(scoreLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(scoreLbl, LV_ALIGN_TOP_LEFT, 6, 3);

    bestLbl = lv_label_create(screen);
    lv_obj_set_style_text_color(bestLbl, lv_color_hex(0xffd60a), LV_PART_MAIN);
    lv_obj_align(bestLbl, LV_ALIGN_TOP_RIGHT, -6, 3);

    field = lv_obj_create(screen);
    lv_obj_remove_style_all(field);
    lv_obj_set_size(field, kCols * kCell, kRows * kCell);
    lv_obj_set_pos(field, (320 - kCols * kCell) / 2, 24);
    lv_obj_set_style_bg_color(field, lv_color_hex(0x0d0d0f), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(field, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(field, fieldTapped, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(field, fieldPressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(field, fieldPressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(field, fieldReleased, LV_EVENT_RELEASED, NULL);

    // status/hint messages float centered over the field (created after it = drawn on top);
    // labels aren't clickable, so retry-taps still reach the field underneath
    hintLbl = lv_label_create(screen);
    lv_obj_set_style_text_color(hintLbl, lv_color_hex(0xd0d0d5), LV_PART_MAIN);
    lv_obj_align(hintLbl, LV_ALIGN_CENTER, 0, 0);

    // invisible key sink: lives in the input group so keyboard chars reach the game
    keyCatcher = lv_obj_create(screen);
    lv_obj_remove_style_all(keyCatcher);
    lv_obj_set_size(keyCatcher, 1, 1);
    lv_obj_add_event_cb(keyCatcher, keyEvent, LV_EVENT_KEY, NULL);
    lv_group_t *g = lv_group_get_default();
    if (g)
        lv_group_add_obj(g, keyCatcher);

    tick = lv_timer_create(step, kStartPeriodMs, NULL);
    lv_timer_pause(tick); // runs only while the game screen is showing

    lv_obj_add_event_cb(
        screen,
        [](lv_event_t *) {
            lv_timer_resume(tick);
            if (lv_group_get_default())
                lv_group_focus_obj(keyCatcher);
        },
        LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(
        screen, [](lv_event_t *) { lv_timer_pause(tick); }, LV_EVENT_SCREEN_UNLOADED, NULL);

    memset(cellObj, 0, sizeof(cellObj));
    loadBest();
    resetGame();
}
} // namespace

extern "C" void snake_open(void)
{
    if (!screen)
        buildScreen();
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}
