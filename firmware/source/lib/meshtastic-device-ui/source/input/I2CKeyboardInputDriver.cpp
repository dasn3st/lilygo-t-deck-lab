
#include "input/I2CKeyboardInputDriver.h"
#include "graphics/view/TFT/TuiStatusBar.h"
#include "util/ILog.h"
#include <Arduino.h>
#include <Wire.h>

#include "indev/lv_indev_private.h"

// While the screen is dark/locked, keys are swallowed so they don't wake it (only a
// trackball double-click wakes). Defined in TFTView_320x240.cpp.
extern volatile bool tdeck_input_gated;
// In BT programming mode, ANY keypress requests an exit (touch-independent escape hatch).
extern volatile bool tdeck_prog_mode;
extern volatile bool tdeck_prog_key_exit;
// While the screen is dark/locked, a keypress requests a wake (alternate to trackball).
extern volatile bool tdeck_wake_request;
// Alt+C on the T-Deck keyboard emits a dedicated byte (0x0C, see the C3 keyboard firmware):
// a touch-independent request to (re)run screen calibration, handled by the launcher's poll.
extern volatile bool tdeck_calib_request;
// I / O zoom the map (jeeab/t-ui#7). -1 out, +1 in.
extern volatile int tdeck_map_zoom_request;
extern "C" bool tdeck_maps_active(void);
// The lock screen kept lit: a key brightens it rather than waking it, unless it is THE key.
extern "C" bool tdeck_stayon_active(void);
extern "C" int tdeck_lock_unlock_key(void); // 0 slide only, 1 spacebar, 2 Alt+W
extern volatile bool tdeck_stayon_boost_request;
// The "erase" key pressed while nothing is being typed into: a request to go back, polled by
// the launcher (which decides whether Back is allowed from the screen in front of the user).
extern volatile bool tdeck_back_request;
// While a Lua app is the screen in front of the user, its keys belong to that app rather
// than to LVGL. Defined in graphics/TFT/LuaApp.cpp.
extern "C" bool tdeck_lua_app_focused(void);
extern "C" void tdeck_lua_queue_key(uint32_t key);
// A Lua app can set keep_back=true to keep the erase/Back key for itself (src/TDeckLua.cpp).
extern "C" bool tdeck_lua_app_keeps_back(void);

I2CKeyboardInputDriver::KeyboardList I2CKeyboardInputDriver::i2cKeyboardList;

I2CKeyboardInputDriver::I2CKeyboardInputDriver(void) {}

void I2CKeyboardInputDriver::init(void)
{
    keyboard = lv_indev_create();
    lv_indev_set_type(keyboard, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keyboard, keyboard_read);

    if (!inputGroup) {
        inputGroup = lv_group_create();
        lv_group_set_default(inputGroup);
    }
    lv_indev_set_group(keyboard, inputGroup);
}

bool I2CKeyboardInputDriver::registerI2CKeyboard(I2CKeyboardInputDriver *driver, std::string name, uint8_t address)
{
    auto keyboardDef = std::unique_ptr<KeyboardDefinition>(new KeyboardDefinition{driver, name, address});
    i2cKeyboardList.push_back(std::move(keyboardDef));
    ILOG_INFO("Registered I2C keyboard: %s at address 0x%02X", name.c_str(), address);
    return true;
}

void I2CKeyboardInputDriver::keyboard_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    // Read from all registered keyboards
    for (auto &keyboardDef : i2cKeyboardList) {
        keyboardDef->driver->readKeyboard(keyboardDef->address, indev, data);
        if (data->state == LV_INDEV_STATE_PRESSED) {
            // On the original T-Deck keyboard the physical SYM/FN + Space combination is
            // emitted by the keyboard controller as 0xAF. Keep it out of the text field and use
            // it as the global on-screen keyboard shortcut instead.
            if (data->key == 0xAF && tui_statusbar_try_toggle_keyboard()) {
                data->state = LV_INDEV_STATE_RELEASED;
                data->key = 0;
                break;
            }
            // Alt+C (the T-Deck keyboard firmware emits 0x0C for this exact combo) requests screen
            // calibration — a keyboard escape hatch for when the touchscreen is miscalibrated. Only
            // acted on while the screen is awake (not gated); the launcher poll runs it from the
            // lock pad. Swallow the byte so it never types onto the UI.
            if (data->key == 0x0C && !tdeck_input_gated) {
                tdeck_calib_request = true;
                data->state = LV_INDEV_STATE_RELEASED;
                data->key = 0;
                break;
            }
            // I and O zoom the map (jeeab/t-ui#7). Checked here - after the wake/calibrate
            // escapes, before the key can reach a widget or a Lua app - and ONLY while the map
            // is the screen in front of you, so they stay ordinary letters everywhere else.
            // Typing is still typing: if a text box has focus, "io" types "io".
            if (!tdeck_input_gated && (data->key == 'i' || data->key == 'I' || data->key == 'o' ||
                                       data->key == 'O') &&
                tdeck_maps_active()) {
                lv_group_t *zg = lv_indev_get_group(indev);
                lv_obj_t *zf = zg ? lv_group_get_focused(zg) : nullptr;
                const bool typing = zf && (lv_obj_check_type(zf, &lv_textarea_class) ||
                                           lv_obj_check_type(zf, &lv_keyboard_class));
                if (!typing) {
                    tdeck_map_zoom_request = (data->key == 'i' || data->key == 'I') ? 1 : -1;
                    lv_display_trigger_activity(NULL); // zooming is using the device
                    data->state = LV_INDEV_STATE_RELEASED;
                    data->key = 0;
                    break;
                }
            }
            // In programming mode, any key requests an exit (see the poll timer in
            // enterProgrammingMode) — a reliable escape that doesn't rely on the touchscreen.
            if (tdeck_prog_mode)
                tdeck_prog_key_exit = true;
            // Screen dark/locked: a key press WAKES the screen (a far more reliable wake than
            // the stiff trackball double-click) — but it must not type onto the hidden screen,
            // so we record the request and swallow the key below.
            // The lock screen is being KEPT LIT. That is the pocket case, so a key must not
            // wake it - only the one chosen key does, and anything else brightens the screen
            // for three seconds so the time can be read in sunlight. Checked OUTSIDE the
            // gate, because the lit lock screen deliberately leaves input switched on: it
            // shows a slider, and the slider has to work.
            if (tdeck_stayon_active()) {
                const int uk = tdeck_lock_unlock_key();
                // ALT+W'S BYTE IS NOT KNOWN YET - these combos come from the keyboard's own
                // chip (Alt+C is 0x0C) and cannot be derived from the letter. Every key is
                // logged here so the byte can be read off the cable once and hard-coded.
                LOG_INFO("stayon key=0x%02x (%u)", (unsigned)data->key, (unsigned)data->key);
                if (uk == 1 && data->key == ' ')
                    tdeck_wake_request = true;
                else
                    tdeck_stayon_boost_request = true;
                data->state = LV_INDEV_STATE_RELEASED;
                data->key = 0;
                break;
            }
            // Screen dark/locked: a key press WAKES it.
            if (tdeck_input_gated) {
                tdeck_wake_request = true;
                break;
            }

            // The "erase" key doubles as Back — an alternative to the stiff trackball
            // double-click (requested by a web-installer user). Handled HERE, BEFORE the key is
            // handed to a Lua app or matched against a focused widget, so it works the same in
            // games, tools and built-in screens alike (an earlier version sat after the Lua
            // hand-off, so games like Pinball swallowed the key and Back did nothing in them).
            // The ONE exception is real typing: if an editable text box (or the on-screen
            // keyboard) the user is LOOKING AT has focus, backspace stays a backspace so you can
            // still correct a Wi-Fi password, a PIN, or a note — you leave those editors with the
            // trackball double-click. Lua apps have no text boxes, so Back always works in them.
            // "Looking at" is the active screen OR either overlay layer (lv_layer_top /
            // lv_layer_sys). That second half matters: the Wi-Fi network + password boxes, the
            // pin-rename box and the new-folder box are all built on lv_layer_top, and
            // lv_obj_get_screen() on anything parented there returns the LAYER, never
            // lv_screen_active(). An active-screen-only test therefore called every one of those
            // "not typing" and ate the erase key as Back — you could not correct a typo in a
            // Wi-Fi password. Testing the overlay layers too covers those and any future entry
            // box built on an overlay. (The same allowance is made for ordinary keys further
            // down, in the "keys belong to the screen the user is looking at" check.)
            // handleBackGesture() decides whether Back is actually allowed (never off the lock pad).
            if (data->key == LV_KEY_BACKSPACE && !tdeck_prog_mode) {
                lv_group_t *bgrp = lv_indev_get_group(indev);
                lv_obj_t *bfocus = bgrp ? lv_group_get_focused(bgrp) : nullptr;
                lv_obj_t *bscr = bfocus ? lv_obj_get_screen(bfocus) : nullptr;
                bool visibleHere =
                    bscr && (bscr == lv_screen_active() || bscr == lv_layer_top() || bscr == lv_layer_sys());
                bool typingHere = visibleHere && (lv_obj_check_type(bfocus, &lv_textarea_class) ||
                                                  lv_obj_check_type(bfocus, &lv_keyboard_class));
                // A Lua app that set keep_back=true keeps the erase key for itself (e.g. a game
                // whose controls sit next to erase). Then we DON'T quit it — the key falls through
                // to the Lua hand-off below and reaches the app as on_key("back") for it to use or
                // ignore. Everything else: erase = Back as usual.
                bool appKeepsBack = tdeck_lua_app_focused() && tdeck_lua_app_keeps_back();
                if (!typingHere && !appKeepsBack) {
                    tdeck_back_request = true;
                    data->state = LV_INDEV_STATE_RELEASED;
                    data->key = 0;
                    break;
                }
            }
            // A Lua app is on screen: the key is the app's, not LVGL's. Queue it for the
            // app's next tick and swallow it here, so it can't also land in whatever widget
            // the keyboard group happens to have focused underneath. Deliberately AFTER the
            // gated/prog-mode/Alt+C checks above, so waking, escaping and calibrating all
            // still work exactly as before while an app is open.
            if (tdeck_lua_app_focused()) {
                tdeck_lua_queue_key(data->key);
                // Count it as activity BEFORE swallowing it. The screen timeout only sees
                // input LVGL itself handles, so playing a game with the keyboard alone
                // used to let the screen dim and sleep mid-play - the app was getting the
                // keys but nothing else knew the user was there.
                lv_display_trigger_activity(NULL);
                data->state = LV_INDEV_STATE_RELEASED;
                data->key = 0;
                break;
            }
            // Keys belong to the screen the user is looking at. The keyboard group's focused
            // object can linger on ANOTHER screen (e.g. the Meshtastic chat input stays focused
            // after leaving the Mesh app), so typing on the lock pad or launcher would silently
            // land in that chat box. Swallow any key whose focused target isn't on the screen
            // that's actually on display — EXCEPT targets on the overlay layers (lv_layer_top/
            // lv_layer_sys), which float above every screen and are always visible when focused
            // (the pin-rename and new-folder entry boxes live there).
            lv_group_t *grp = lv_indev_get_group(indev);
            lv_obj_t *focused = grp ? lv_group_get_focused(grp) : NULL;
            if (focused) {
                lv_obj_t *scr = lv_obj_get_screen(focused);
                if (scr != lv_screen_active() && scr != lv_layer_top() && scr != lv_layer_sys()) {
                    data->state = LV_INDEV_STATE_RELEASED;
                    data->key = 0;
                    break;
                }
            }
            // If any keyboard reports a key press, we stop reading further
            return;
        }
    }
    // While dark/locked, swallow so the key can't wake-and-type on a hidden screen. The wake
    // itself was recorded above and is handled by the launcher's gesture poll timer.
    if (tdeck_input_gated) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = 0;
    }
}

// ---------- TDeckKeyboardInputDriver Implementation ----------

TDeckKeyboardInputDriver::TDeckKeyboardInputDriver(uint8_t address)
{
    registerI2CKeyboard(this, "T-Deck Keyboard", address);
}

/******************************************************************
    LV_KEY_NEXT: Focus on the next object
    LV_KEY_PREV: Focus on the previous object
    LV_KEY_ENTER: Triggers LV_EVENT_PRESSED, LV_EVENT_CLICKED, or LV_EVENT_LONG_PRESSED etc. events
    LV_KEY_UP: Increase value or move upwards
    LV_KEY_DOWN: Decrease value or move downwards
    LV_KEY_RIGHT: Increase value or move to the right
    LV_KEY_LEFT: Decrease value or move to the left
    LV_KEY_ESC: Close or exit (E.g. close a Drop down list)
    LV_KEY_DEL: Delete (E.g. a character on the right in a Text area)
    LV_KEY_BACKSPACE: Delete a character on the left (E.g. in a Text area)
    LV_KEY_HOME: Go to the beginning/top (E.g. in a Text area)
    LV_KEY_END: Go to the end (E.g. in a Text area)

    LV_KEY_UP        = 17,  // 0x11
    LV_KEY_DOWN      = 18,  // 0x12
    LV_KEY_RIGHT     = 19,  // 0x13
    LV_KEY_LEFT      = 20,  // 0x14
    LV_KEY_ESC       = 27,  // 0x1B
    LV_KEY_DEL       = 127, // 0x7F
    LV_KEY_BACKSPACE = 8,   // 0x08
    LV_KEY_ENTER     = 10,  // 0x0A, '\n'
    LV_KEY_NEXT      = 9,   // 0x09, '\t'
    LV_KEY_PREV      = 11,  // 0x0B, '
    LV_KEY_HOME      = 2,   // 0x02, STX
    LV_KEY_END       = 3,   // 0x03, ETX
*******************************************************************/

void TDeckKeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    char keyValue = 0;
    uint8_t bytes = Wire.requestFrom(address, 1);
    if (Wire.available() > 0 && bytes > 0)
        keyValue = Wire.read();

    if (bytes > 0) {
        // ignore empty reads and keycode 224(E0, shift-0 on T-Deck) which causes internal issues
        if (keyValue != (char)0x00 && keyValue != (char)0xE0) {
            data->state = LV_INDEV_STATE_PRESSED;
            ILOG_DEBUG("key press value: %d", (int)keyValue);

            switch (keyValue) {
            case 0x0D:
                keyValue = LV_KEY_ENTER;
                break;
            default:
                break;
            }
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    }
    data->key = (uint32_t)keyValue;
}

// ---------- TCA8418KeyboardInputDriver Implementation ----------

TCA8418KeyboardInputDriver::TCA8418KeyboardInputDriver(uint8_t address)
{
    registerI2CKeyboard(this, "TCA8418 Keyboard", address);
}

void TCA8418KeyboardInputDriver::init(void)
{
    // Additional initialization for TCA8418 if needed
    I2CKeyboardInputDriver::init();
}

void TCA8418KeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    // TODO
    char keyValue = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = (uint32_t)keyValue;
}

// ---------- TLoraPagerKeyboardInputDriver Implementation ----------

TLoraPagerKeyboardInputDriver::TLoraPagerKeyboardInputDriver(uint8_t address) : TCA8418KeyboardInputDriver(address)
{
    registerI2CKeyboard(this, "TLora Pager Keyboard", address);
}

void TLoraPagerKeyboardInputDriver::init(void)
{
    // Additional initialization for TLora-Pager if needed
    TCA8418KeyboardInputDriver::init();
}

void TLoraPagerKeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    // TODO
    char keyValue = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = (uint32_t)keyValue;
}

// ---------- TDeckProKeyboardInputDriver Implementation ----------

TDeckProKeyboardInputDriver::TDeckProKeyboardInputDriver(uint8_t address) : TCA8418KeyboardInputDriver(address)
{
    registerI2CKeyboard(this, "T-Deck Pro Keyboard", address);
}

void TDeckProKeyboardInputDriver::init(void)
{
    // Additional initialization for TLora-Pager if needed
    TCA8418KeyboardInputDriver::init();
}

void TDeckProKeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    // TODO
    char keyValue = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = (uint32_t)keyValue;
}

// ---------- BBQ10KeyboardInputDriver Implementation ----------

BBQ10KeyboardInputDriver::BBQ10KeyboardInputDriver(uint8_t address)
{
    registerI2CKeyboard(this, "BBQ10 Keyboard", address);
}

void BBQ10KeyboardInputDriver::init(void)
{
    I2CKeyboardInputDriver::init();
    // Additional initialization for BBQ10 if needed
}

void BBQ10KeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    char keyValue = 0;
    uint8_t bytes = Wire.requestFrom(address, 1);
    if (Wire.available() > 0 && bytes > 0) {
        keyValue = Wire.read();
        // ignore empty reads and keycode 224(E0, shift-0 on T-Deck) which causes internal issues
        if (keyValue != (char)0x00 && keyValue != (char)0xE0) {
            data->state = LV_INDEV_STATE_PRESSED;
            ILOG_DEBUG("key press value: %d", (int)keyValue);

            switch (keyValue) {
            case 0x0D:
                keyValue = LV_KEY_ENTER;
                break;
            default:
                break;
            }
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    }
    data->key = (uint32_t)keyValue;
}

// ---------- CardKBInputDriver Implementation ----------

CardKBInputDriver::CardKBInputDriver(uint8_t address)
{
    registerI2CKeyboard(this, "Card Keyboard", address);
}

void CardKBInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    char keyValue = 0;
    Wire.requestFrom(address, 1);
    if (Wire.available() > 0) {
        keyValue = Wire.read();
        // ignore empty reads and keycode 224 which causes internal issues
        if (keyValue != (char)0x00 && keyValue != (char)0xE0) {
            data->state = LV_INDEV_STATE_PRESSED;
            ILOG_DEBUG("key press value: %d", (int)keyValue);

            switch (keyValue) {
            case 0x0D:
                keyValue = LV_KEY_ENTER;
                break;
            case 0xB4:
                keyValue = LV_KEY_LEFT;
                break;
            case 0xB5:
                keyValue = LV_KEY_UP;
                break;
            case 0xB6:
                keyValue = LV_KEY_DOWN;
                break;
            case 0xB7:
                keyValue = LV_KEY_RIGHT;
                break;
            case 0x99: // Fn+UP
                keyValue = LV_KEY_HOME;
                break;
            case 0xA4: // Fn+DOWN
                keyValue = LV_KEY_END;
                break;
            case 0x8B: // Fn+BS
                keyValue = LV_KEY_DEL;
                break;
            case 0x8C: // Fn+TAB
                keyValue = LV_KEY_PREV;
                break;
            case 0xA3: // Fn+ENTER
                // simulate a long press on Fn+ENTER (see indev_keypad_proc() in indev.c)
                indev->wait_until_release = 0;
                indev->pr_timestamp = lv_tick_get() - indev->long_press_time - 1;
                indev->long_pr_sent = 0;
                indev->keypad.last_state = LV_INDEV_STATE_PRESSED;
                indev->keypad.last_key = LV_KEY_ENTER;
                keyValue = LV_KEY_ENTER;
                break;
            default:
                break;
            }
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    }
    data->key = (uint32_t)keyValue;
}

// ---------- MPR121KeyboardInputDriver Implementation ----------

MPR121KeyboardInputDriver::MPR121KeyboardInputDriver(uint8_t address)
{
    registerI2CKeyboard(this, "MPR121 Keyboard", address);
}

void MPR121KeyboardInputDriver::init(void)
{
    I2CKeyboardInputDriver::init();
    // Additional initialization for MPR121 if needed
}

void MPR121KeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    // TODO
    char keyValue = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = (uint32_t)keyValue;
}
