// -----------------------------------------------------------------------------
// T-UI Calculator — a self-contained basic calculator for the launcher.
//
// iOS-style layout: readout on top, 4x5 button grid (C ± % ÷ / 789× / 456− /
// 123+ / 0 . ⌫ =). Immediate-execution logic (2 + 3 × 4 = 20, like a pocket
// calculator). The physical keyboard works too: digits, . + - * / =, Enter,
// backspace, and C to clear — via the same invisible key-catcher trick Snake
// uses (with the periodic refocus guard, since the trackball encoder can move
// LVGL's focus elsewhere). Back button or trackball double-click exits.
// -----------------------------------------------------------------------------
#include "graphics/view/TFT/TuiStatusBar.h" // the persistent top bar
#include "lvgl.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void calculator_open(void);

namespace
{
lv_obj_t *screen = nullptr;
lv_obj_t *readout = nullptr;
lv_obj_t *keyCatcher = nullptr;
lv_obj_t *prevScreen = nullptr; // where to go on Back (the launcher)
lv_timer_t *focusGuard = nullptr;

// ---- calculator state (immediate execution) ----
char entry[16] = "0"; // the number being typed
bool entering = true; // true = readout shows entry, false = shows acc
double acc = 0;       // running result
char pend = 0;        // pending operator waiting for the next number
bool err = false;     // division by zero etc.; any key clears via C

void paint(void)
{
    char buf[24];
    if (err) {
        snprintf(buf, sizeof(buf), "Fehler");
    } else if (entering) {
        snprintf(buf, sizeof(buf), "%s", entry);
    } else {
        snprintf(buf, sizeof(buf), "%.10g", acc);
        if (strlen(buf) > 13)
            snprintf(buf, sizeof(buf), "%.6g", acc);
    }
    lv_label_set_text(readout, buf);
}

void clearAll(void)
{
    strcpy(entry, "0");
    entering = true;
    acc = 0;
    pend = 0;
    err = false;
    paint();
}

// fold the typed number into acc using the pending operator
void applyPending(void)
{
    double v = atof(entry);
    switch (pend) {
    case '+':
        acc += v;
        break;
    case '-':
        acc -= v;
        break;
    case '*':
        acc *= v;
        break;
    case '/':
        if (v == 0) {
            err = true;
            return;
        }
        acc /= v;
        break;
    default:
        acc = v; // no pending op: the entry becomes the accumulator
        break;
    }
}

void pressDigit(char d)
{
    if (err)
        clearAll();
    if (!entering) {
        strcpy(entry, "0");
        entering = true;
    }
    size_t n = strlen(entry);
    if (n >= 12)
        return;
    if (!strcmp(entry, "0"))
        entry[--n] = 0; // replace the lone leading zero
    entry[n] = d;
    entry[n + 1] = 0;
    paint();
}

void pressDot(void)
{
    if (err)
        clearAll();
    if (!entering) {
        strcpy(entry, "0");
        entering = true;
    }
    if (strchr(entry, '.') || strlen(entry) >= 12)
        return;
    strcat(entry, ".");
    paint();
}

void pressOp(char o)
{
    if (err)
        return;
    if (entering) {
        applyPending();
        entering = false;
    }
    pend = o; // typing "5 + × 3" just switches the operator
    paint();
}

void pressEquals(void)
{
    if (err)
        return;
    if (entering && pend) {
        applyPending();
        entering = false;
        pend = 0;
    } else if (entering) {
        acc = atof(entry);
        entering = false;
    }
    paint();
}

void pressSign(void)
{
    if (err)
        return;
    if (entering) {
        if (!strcmp(entry, "0"))
            return;
        if (entry[0] == '-')
            memmove(entry, entry + 1, strlen(entry));
        else if (strlen(entry) < 14) {
            memmove(entry + 1, entry, strlen(entry) + 1);
            entry[0] = '-';
        }
    } else {
        acc = -acc;
    }
    paint();
}

void pressPercent(void)
{
    if (err)
        return;
    if (entering) {
        double v = atof(entry) / 100.0;
        snprintf(entry, sizeof(entry), "%.10g", v);
    } else {
        acc /= 100.0;
    }
    paint();
}

void pressBackspace(void)
{
    if (err) {
        clearAll();
        return;
    }
    if (!entering)
        return;
    size_t n = strlen(entry);
    if (n > 1)
        entry[n - 1] = 0;
    else
        strcpy(entry, "0");
    paint();
}

// one handler for every grid button; the button's user_data is its key char
void onButton(lv_event_t *e)
{
    char k = (char)(intptr_t)lv_event_get_user_data(e);
    switch (k) {
    case 'C':
        clearAll();
        break;
    case 'S':
        pressSign();
        break;
    case '%':
        pressPercent();
        break;
    case 'B':
        pressBackspace();
        break;
    case '=':
        pressEquals();
        break;
    case '.':
        pressDot();
        break;
    case '+':
    case '-':
    case '*':
    case '/':
        pressOp(k);
        break;
    default:
        if (k >= '0' && k <= '9')
            pressDigit(k);
        break;
    }
}

// physical keyboard: same keys, via the focused key-catcher
void keyEvent(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key >= '0' && key <= '9')
        pressDigit((char)key);
    else if (key == '.' || key == ',')
        pressDot();
    else if (key == '+' || key == '-' || key == '/')
        pressOp((char)key);
    else if (key == '*' || key == 'x' || key == 'X')
        pressOp('*');
    else if (key == '=' || key == LV_KEY_ENTER)
        pressEquals();
    else if (key == LV_KEY_BACKSPACE)
        pressBackspace();
    else if (key == 'c' || key == 'C')
        clearAll();
    else if (key == '%')
        pressPercent();
}

lv_obj_t *gridBtn(const char *txt, char key, int col, int row, uint32_t bg, uint32_t fg)
{
    lv_obj_t *btn = lv_btn_create(screen);
    lv_obj_set_size(btn, 75, 29);
    lv_obj_set_pos(btn, 4 + col * 79, 72 + row * 33);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, onButton, LV_EVENT_CLICKED, (void *)(intptr_t)key);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_hex(fg), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &ui_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

void buildScreen(void)
{
    screen = lv_obj_create(NULL);
    tui_statusbar_reserve(screen); // note item A4: the persistent top bar
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // big right-aligned readout
    readout = lv_label_create(screen);
    lv_obj_set_style_text_color(readout, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(readout, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_set_width(readout, 250);
    lv_obj_set_style_text_align(readout, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_pos(readout, 62, 24);
    lv_label_set_long_mode(readout, LV_LABEL_LONG_CLIP);

    // the grid — iOS colors: gray function row, orange operators, dark digits
    const uint32_t kFn = 0xa5a5a5, kOp = 0xff9f0a, kNum = 0x333333;
    gridBtn("C", 'C', 0, 0, kFn, 0x000000);
    gridBtn("+/-", 'S', 1, 0, kFn, 0x000000);
    gridBtn("%", '%', 2, 0, kFn, 0x000000);
    gridBtn("/", '/', 3, 0, kOp, 0xffffff);
    gridBtn("7", '7', 0, 1, kNum, 0xffffff);
    gridBtn("8", '8', 1, 1, kNum, 0xffffff);
    gridBtn("9", '9', 2, 1, kNum, 0xffffff);
    gridBtn("x", '*', 3, 1, kOp, 0xffffff);
    gridBtn("4", '4', 0, 2, kNum, 0xffffff);
    gridBtn("5", '5', 1, 2, kNum, 0xffffff);
    gridBtn("6", '6', 2, 2, kNum, 0xffffff);
    gridBtn("-", '-', 3, 2, kOp, 0xffffff);
    gridBtn("1", '1', 0, 3, kNum, 0xffffff);
    gridBtn("2", '2', 1, 3, kNum, 0xffffff);
    gridBtn("3", '3', 2, 3, kNum, 0xffffff);
    gridBtn("+", '+', 3, 3, kOp, 0xffffff);
    gridBtn("0", '0', 0, 4, kNum, 0xffffff);
    gridBtn(".", '.', 1, 4, kNum, 0xffffff);
    gridBtn(LV_SYMBOL_BACKSPACE, 'B', 2, 4, kNum, 0xffffff);
    gridBtn("=", '=', 3, 4, kOp, 0xffffff);

    // invisible key-catcher so the physical keyboard works (same trick as Snake)
    keyCatcher = lv_obj_create(screen);
    lv_obj_remove_style_all(keyCatcher);
    lv_obj_set_size(keyCatcher, 1, 1);
    lv_obj_add_event_cb(keyCatcher, keyEvent, LV_EVENT_KEY, NULL);
    if (lv_group_get_default())
        lv_group_add_obj(lv_group_get_default(), keyCatcher);

    // the encoder indev can wander LVGL's focus; keep the keyboard aimed at us
    focusGuard = lv_timer_create(
        [](lv_timer_t *) {
            lv_group_t *g = lv_group_get_default();
            if (g && lv_group_get_focused(g) != keyCatcher)
                lv_group_focus_obj(keyCatcher);
        },
        250, NULL);
    lv_timer_pause(focusGuard);

    lv_obj_add_event_cb(
        screen,
        [](lv_event_t *) {
            lv_timer_resume(focusGuard);
            if (lv_group_get_default())
                lv_group_focus_obj(keyCatcher);
        },
        LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(
        screen, [](lv_event_t *) { lv_timer_pause(focusGuard); }, LV_EVENT_SCREEN_UNLOADED, NULL);

    paint();
}
} // namespace

extern "C" void calculator_open(void)
{
    prevScreen = lv_screen_active();
    if (!screen)
        buildScreen();
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}
