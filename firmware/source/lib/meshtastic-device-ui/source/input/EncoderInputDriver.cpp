#ifdef INPUTDRIVER_ENCODER_TYPE

#include "input/EncoderInputDriver.h"
#include "Arduino.h"
#include "util/ILog.h"
#include <esp_heap_caps.h>

volatile EncoderInputDriver::EncoderActionType EncoderInputDriver::action = TB_ACTION_NONE;

// Set on a trackball double-click; the launcher polls this to run the Home/lock/wake gesture.
volatile bool tb_home_request = false;

// Rolling the trackball up or down asks to move a whole ROW, not one item: -1 up, +1 down.
// The launcher polls this, because only it knows how wide the grid is (3 across) and that a
// plain list should still move one line at a time. Left/right are ordinary PREV/NEXT keys and
// need none of this.
volatile int tb_nav_rows = 0;

// Settings -> Trackball navigation (src/TDeckTrackball.cpp). Default OFF, so a device that
// has never touched the setting behaves exactly as before: rolls do nothing at all.
extern "C" bool tdeck_trackball_nav_enabled(void);
extern "C" bool tdeck_on_home_screen(void); // hold-to-lock, see the hold gesture below
// Settings -> Trackball click selects. Separate switch; HOLDING for Home is always on.
extern "C" bool tdeck_trackball_click_enabled(void);
// True while the PIN screen is up - see TFTView_320x240.cpp.
extern "C" bool tdeck_lockscreen_active(void);

// When true the screen is dark/locked: trackball *rolls* are swallowed so they can't wake it
// (only the double-click below wakes). Defined in TFTView_320x240.cpp.
extern volatile bool tdeck_input_gated;
// Settings -> Trackball cursor, and its speed (src/TDeckTrackball.cpp). Default off.
extern "C" bool tdeck_trackball_nav_enabled(void);
extern "C" int tdeck_trackball_speed(void);
extern "C" int tdeck_trackball_style(void); // 0 = see-through, 1 = solid
// "Go back" - the same request the keyboard's erase key raises (TFTView_320x240.cpp).
extern volatile bool tdeck_back_request;
// The lock screen kept lit: rolling or clicking brightens it, it does not unlock.
extern "C" bool tdeck_stayon_active(void);
extern volatile bool tdeck_stayon_boost_request;

// ---- the trackball cursor -------------------------------------------------------------
// A real LVGL pointer, reported exactly like the touchscreen, so a click lands on whatever is
// underneath it - a tile, the map, a game, a Lua app - with nothing needing to opt in.
static lv_indev_t *tbPointer = nullptr;
static lv_obj_t *tbCursorObj = nullptr;
static int16_t tbX = 160, tbY = 120; // starts in the middle
static bool tbTapNow = false;        // deliver one press on the next read
static uint32_t tbLastMoveMs = 0;
static uint32_t tbLastStepMs = 0;

// ---- raw roll events, straight from the interrupts -------------------------------------
// The two axes count SEPARATELY, and that is the whole reason diagonals work now. The old
// code had every interrupt write one shared `action`, so a diagonal roll - which really does
// pulse both axes - lost whichever pulse landed first. A variable holding one direction
// cannot express two.
//
// Incremented from interrupt context with a plain ++, deliberately: the four pins share one
// GPIO interrupt service on one core, so these handlers cannot preempt each other and the
// read-modify-write is safe. (__atomic_fetch_add would have been the textbook answer and is
// the WRONG one here - it can call into libatomic, which is not guaranteed to be resident in
// RAM, and a flash access from an interrupt is a crash.) The consumer drains with an atomic
// exchange from task context, where that is free, so the worst case is losing a single pulse
// in a one-instruction window.
static volatile int32_t tbRawX = 0, tbRawY = 0;

// ---- the motion itself ------------------------------------------------------------------
// Sub-pixel position, and velocity in pixels per 16ms sub-step. The ball adds velocity and
// friction takes it away, which is what makes a flick glide to a stop instead of landing in
// one jump.
static float tbFx = 160.0f, tbFy = 120.0f;
static float tbVx = 0.0f, tbVy = 0.0f;
static uint32_t tbIntegMs = 0; // last integration, for real elapsed time
static uint32_t tbCarryMs = 0; // leftover ms not yet turned into a sub-step

// ---- the deadzone -----------------------------------------------------------------------
// "then if i accidentally touch the ball it moves". Pulses are held back until enough arrive
// close together to be a deliberate roll; a single stray pulse from a brushed ball is thrown
// away instead of becoming a jump. Once moving, every pulse counts - the bar is only there
// to START.
static int tbArmCount = 0;
static int32_t tbPendX = 0, tbPendY = 0;
static uint32_t tbArmFirstMs = 0;
static bool tbArmed = false;
static const int kArmEvents = 2;          // pulses needed to believe it
static const uint32_t kArmWindowMs = 400; // ...within this long
static const uint32_t kDisarmMs = 500;    // silence after which we ask again
static const uint32_t kCursorIdleHideMs = 6000;

static void tbCursorShow(bool on)
{
    if (!tbCursorObj)
        return;
    if (on)
        lv_obj_remove_flag(tbCursorObj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(tbCursorObj, LV_OBJ_FLAG_HIDDEN);
}

// ---- the arrow ------------------------------------------------------------------------
// ⛔ ONE OBJECT, NOT A STACK OF BARS. lv_indev_set_cursor() moves a single object to follow
// the pointer, and it moves on every roll step - an arrow assembled from little rectangles
// would be ~40 objects to reposition and redraw each time. A canvas is one object with its
// own pixel buffer: drawn once, then LVGL only moves it. It also gives real per-pixel alpha.
//
// THE SHAPE IS A POLYGON, SCAN-FILLED - not a table of row spans. Jake's reference picture
// (2026-09-20) is the classic pointer drawn as an OUTLINE, hollow in the middle, which on a
// 320px screen is the best of both: as easy to find as a solid arrow, and it hides nothing,
// because whatever you are about to click shows through it. Hand-typed spans made the first
// attempt look like a staircase; a polygon keeps the long edges straight at any scale.
//
// Tip at (0,0), down the left edge, out to the tail point, back up, and the long diagonal
// home. Checked in the PC renderer against his picture before it came anywhere near here.
static const int kArrowPts[7][2] = {{0, 0}, {0, 19}, {5, 15}, {8, 22}, {10, 21}, {7, 15}, {12, 14}};
// Keep the pointer compact on the 320x240 display; the previous scale 2 cursor
// covered too much of the UI and looked oversized beside the trackball.
static const int kArrowScale = 1;
static const int kArrowW = 13 * kArrowScale + 2;
static const int kArrowH = 23 * kArrowScale + 2;

// Even-odd scanline test, sampled at pixel centres.
static bool arrowInside(double x, double y)
{
    bool in = false;
    for (int i = 0, j = 6; i < 7; j = i++) {
        const double xi = kArrowPts[i][0], yi = kArrowPts[i][1];
        const double xj = kArrowPts[j][0], yj = kArrowPts[j][1];
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
            in = !in;
    }
    return in;
}

static bool arrowAt(int px, int py)
{
    return arrowInside((px - 1 + 0.5) / kArrowScale, (py - 1 + 0.5) / kArrowScale);
}

// Repaint for the current style. Called at init and whenever Settings changes it, so the
// change shows without a reboot. style 0 = outline (hollow), 1 = solid.
static void tbArrowPaint(void)
{
    if (!tbCursorObj)
        return;
    const bool solid = (tdeck_trackball_style() == 1);
    lv_canvas_fill_bg(tbCursorObj, lv_color_hex(0x000000), LV_OPA_TRANSP);
    for (int y = 0; y < kArrowH; y++) {
        for (int x = 0; x < kArrowW; x++) {
            const bool me = arrowAt(x, y);
            bool rim = false, touching = false;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy)
                        continue;
                    const bool n = arrowAt(x + dx, y + dy);
                    if (me && !n)
                        rim = true;
                    if (!me && n)
                        touching = true;
                }
            if (me && (rim || solid)) {
                // The rim is always fully opaque white. That is what keeps the arrow readable
                // over a bright icon - fading it is exactly what sank an earlier translucent
                // cursor, which vanished over the red Alerts bell.
                lv_canvas_set_px(tbCursorObj, x, y, lv_color_hex(0xffffff), rim ? LV_OPA_COVER : 150);
            } else if (!me && touching) {
                // A dark halo just outside, so the white rim still shows on a white map tile.
                lv_canvas_set_px(tbCursorObj, x, y, lv_color_hex(0x000000), 200);
            }
        }
    }
}

extern "C" void tdeck_trackball_cursor_restyle(void)
{
    tbArrowPaint();
}

// Pixels of velocity one roll pulse is worth. Multiply by 1/(1-friction) = 8.3 for the
// distance it actually travels: about 3, 6, 12, 20 and 30 pixels across the five settings.
// Jake wanted the range in Settings because a ball in a 3D-printed case rolls nothing like a
// bare one - his is stiff, somebody else's is loose, and one number cannot suit both.
static float tbGain(void)
{
    static const float kGain[5] = {0.36f, 0.72f, 1.44f, 2.40f, 3.60f};
    int sp = tdeck_trackball_speed();
    if (sp < 0 || sp > 4)
        sp = 2;
    return kGain[sp];
}

// One frame of motion. Called from the pointer read callback, so it runs exactly when LVGL is
// about to use the position - roughly every 30ms.
//
// The model is deliberately physical: each pulse adds a fixed amount of VELOCITY, and each
// sub-step moves by the current velocity then scales it down. A pulse therefore travels
// kGain / (1 - kFriction) pixels in total, spread over about eighteen sub-steps instead of
// arriving at once, and a fast roll is fast because its pulses stack up - so there is no
// separate acceleration rule, and no gap-timing to get wrong.
static void tbIntegrate(void)
{
    const uint32_t now = millis();

    // ⛔ THE OFF SWITCH. This has to be HERE, not at the event dispatch, because the dispatch
    // no longer moves anything - it only creates the cursor. Leaving the check there meant a
    // switched-off trackball still rolled the arrow around while the click was correctly
    // ignored, which is the half-working state Jake found. Pulses that arrived while it was
    // off are thrown away rather than banked, so switching on does not fire a stored flick.
    if (!tdeck_trackball_nav_enabled()) {
        tbRawX = tbRawY = 0;
        tbVx = tbVy = 0.0f;
        tbArmed = false;
        tbArmCount = 0;
        tbPendX = tbPendY = 0;
        if (tbCursorObj && !lv_obj_has_flag(tbCursorObj, LV_OBJ_FLAG_HIDDEN))
            tbCursorShow(false);
        return;
    }

    // Drain both axes together. Everything that arrived since the last frame is ONE movement,
    // so a diagonal roll - pulses on both axes in the same frame - simply comes out diagonal.
    const int32_t ex = __atomic_exchange_n(&tbRawX, 0, __ATOMIC_RELAXED);
    const int32_t ey = __atomic_exchange_n(&tbRawY, 0, __ATOMIC_RELAXED);
    const int32_t events = (ex < 0 ? -ex : ex) + (ey < 0 ? -ey : ey);

    if (events) {
        if (tbArmed) {
            tbArmFirstMs = now;
        } else {
            // Not moving yet: hold these and see whether more follow. A brush gives one pulse
            // and nothing after it, so it times out below and is discarded.
            if (!tbArmCount || now - tbArmFirstMs > kArmWindowMs) {
                tbArmCount = 0;
                tbPendX = tbPendY = 0;
                tbArmFirstMs = now;
            }
            tbArmCount += (int)events;
            tbPendX += ex;
            tbPendY += ey;
            if (tbArmCount >= kArmEvents) {
                // Deliberate. Release everything held back, so nothing is lost from the start
                // of a real roll - the deadzone costs a frame of latency, never distance.
                tbArmed = true;
                tbVx += (float)tbPendX * tbGain();
                tbVy += (float)tbPendY * tbGain();
                tbArmCount = 0;
                tbPendX = tbPendY = 0;
            }
        }
        if (tbArmed) {
            tbVx += (float)ex * tbGain();
            tbVy += (float)ey * tbGain();
        }
    } else if (tbArmed && now - tbArmFirstMs > kDisarmMs) {
        tbArmed = false; // gone quiet: ask again before the next movement
    } else if (!tbArmed && tbArmCount && now - tbArmFirstMs > kArmWindowMs) {
        tbArmCount = 0; // a lone pulse, never followed up - that was the accidental touch
        tbPendX = tbPendY = 0;
    }

    // ⚠️ THE CEILING IS THE SCREEN, not the maths. LV_DEF_REFR_PERIOD is 40ms, so the panel
    // draws 25 times a second and a frame is ~2.5 of these 16ms sub-steps. The old flat clamp
    // of 22 therefore allowed 55 pixels between two DRAWN frames - perfectly smooth in the
    // model, visibly steppy in the hand, which is what Jake still saw at Max.
    //
    // So the clamp is per speed and sized in what a frame can show: v * 2.5 px per frame.
    //   Slow 6 . Normal 10 . Fast 15 . Faster 20 . Max 25 px per frame
    // Max is deliberately at the edge of what 25fps can render smoothly (~625 px/sec). Going
    // faster than that is not a tuning choice - there is simply nothing to draw in between.
    static const float kVmax[5] = {2.5f, 4.0f, 6.0f, 8.0f, 10.0f};
    int spc = tdeck_trackball_speed();
    if (spc < 0 || spc > 4)
        spc = 2;
    const float vmax = kVmax[spc];
    if (tbVx > vmax) tbVx = vmax; else if (tbVx < -vmax) tbVx = -vmax;
    if (tbVy > vmax) tbVy = vmax; else if (tbVy < -vmax) tbVy = -vmax;

    if (tbVx == 0.0f && tbVy == 0.0f) {
        tbIntegMs = now;
        tbCarryMs = 0;
        return;
    }

    // Fixed 16ms sub-steps off the REAL clock, so the feel does not change with frame rate:
    // the UI task is busy on the map and idle in Settings, and the cursor must not speed up
    // just because the screen got cheaper to draw. Capped so a stall cannot fling it.
    uint32_t dt = tbIntegMs ? (now - tbIntegMs) : 16;
    tbIntegMs = now;
    if (dt > 200)
        dt = 200;
    tbCarryMs += dt;
    int steps = (int)(tbCarryMs / 16);
    if (steps > 12)
        steps = 12;
    tbCarryMs -= (uint32_t)steps * 16;

    // A stalled UI hands us a big dt and therefore many sub-steps at once. Integrating all of
    // them is correct but looks like a teleport, and this device demonstrably stalls for
    // seconds at a time - so one call may never move further than a single frame's worth.
    const float kMaxPerCall = vmax * 3.0f;
    float movedX = 0.0f, movedY = 0.0f;

    const float kFriction = 0.88f;
    for (int i = 0; i < steps; i++) {
        if (movedX > kMaxPerCall || movedX < -kMaxPerCall || movedY > kMaxPerCall || movedY < -kMaxPerCall)
            break;
        movedX += tbVx;
        movedY += tbVy;
        tbFx += tbVx;
        tbFy += tbVy;
        // The pointer is strictly bounded to the visible panel.  Overflow at an edge is
        // discarded; it must never pan the launcher or the active desktop.
        if (tbFx < 2.0f) {
            tbFx = 2.0f;
        } else if (tbFx > 317.0f) {
            tbFx = 317.0f;
        }
        if (tbFy < 2.0f) {
            tbFy = 2.0f;
        } else if (tbFy > 237.0f) {
            tbFy = 237.0f;
        }
        tbVx *= kFriction;
        tbVy *= kFriction;
    }
    // Stop dead once the glide is worth less than a pixel in total, so the cursor settles
    // instead of creeping. It has to be well UNDER the smallest gain (0.36): at 0.33 it was
    // larger than a whole Slow pulse, so Slow zeroed itself immediately and moved 0.4px per
    // roll instead of 3 - the setting existed but did nothing.
    if (tbVx < 0.06f && tbVx > -0.06f)
        tbVx = 0.0f;
    if (tbVy < 0.06f && tbVy > -0.06f)
        tbVy = 0.0f;

    const int16_t nx = (int16_t)(tbFx + 0.5f), ny = (int16_t)(tbFy + 0.5f);
    if (nx != tbX || ny != tbY) {
        tbX = nx;
        tbY = ny;
        tbLastMoveMs = now;
        tbCursorShow(true);
    }
}

static void tb_pointer_read(lv_indev_t *, lv_indev_data_t *data)
{
    tbIntegrate(); // advance the motion for this frame before LVGL reads the position
    data->point.x = tbX;
    data->point.y = tbY;
    // One read pressed, then straight back up: a tap. A HELD button must never become a long
    // press on whatever is under the cursor - that would make dragging and scrolling
    // impossible - so the press is always exactly one read long.
    data->state = tbTapNow ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    tbTapNow = false;
    if (tbCursorObj && !lv_obj_has_flag(tbCursorObj, LV_OBJ_FLAG_HIDDEN) &&
        millis() - tbLastMoveMs > kCursorIdleHideMs)
        tbCursorShow(false);
}

// Built the first time the cursor is actually wanted, so a device with the setting off never
// creates any of it.
static void tbCursorInit(void)
{
    if (tbPointer)
        return;
    tbPointer = lv_indev_create();
    lv_indev_set_type(tbPointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(tbPointer, tb_pointer_read);

    // PSRAM, allocated once and never freed: the canvas keeps the pointer for the life of
    // the object. 28 x 38 at 4 bytes a pixel is about 4KB.
    static uint8_t *arrowBuf = nullptr;
    if (!arrowBuf)
        arrowBuf = (uint8_t *)heap_caps_malloc(kArrowW * kArrowH * 4 + 64, MALLOC_CAP_SPIRAM);
    if (!arrowBuf)
        return; // no cursor rather than a bad one
    tbCursorObj = lv_canvas_create(lv_layer_sys());
    lv_canvas_set_buffer(tbCursorObj, arrowBuf, kArrowW, kArrowH, LV_COLOR_FORMAT_ARGB8888);
    tbArrowPaint();
    lv_obj_remove_flag(tbCursorObj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(tbCursorObj, LV_OBJ_FLAG_SCROLLABLE);
    lv_indev_set_cursor(tbPointer, tbCursorObj); // LVGL keeps it on the point for us
    tbCursorShow(false);
}

// Cursor off again: put the pointer away so it cannot hover or be seen.
// Where the cursor is, for the edge-scroll in TFTView. Returns false when there is nothing
// to do - cursor off, never created, or currently hidden - so the caller can bail cheaply.
extern "C" bool tdeck_trackball_cursor_state(int *x, int *y)
{
    if (!tbCursorObj || !tdeck_trackball_nav_enabled())
        return false;
    if (lv_obj_has_flag(tbCursorObj, LV_OBJ_FLAG_HIDDEN))
        return false;
    if (x)
        *x = tbX;
    if (y)
        *y = tbY;
    return true;
}

extern "C" void tdeck_trackball_cursor_off(void)
{
    tbCursorShow(false);
}

EncoderInputDriver::EncoderInputDriver(void) {}

void EncoderInputDriver::init(void)
{
    // trackball or joystick type encoder with four directions
    if (INPUTDRIVER_ENCODER_TYPE == 3) {
#ifdef INPUTDRIVER_ENCODER_LEFT
        pinMode(INPUTDRIVER_ENCODER_LEFT, INPUT_PULLUP);
        attachInterrupt(INPUTDRIVER_ENCODER_LEFT, intLeftHandler, RISING);
#endif
#ifdef INPUTDRIVER_ENCODER_RIGHT
        pinMode(INPUTDRIVER_ENCODER_RIGHT, INPUT_PULLUP);
        attachInterrupt(INPUTDRIVER_ENCODER_RIGHT, intRightHandler, RISING);
#endif
#ifdef INPUTDRIVER_ENCODER_UP
        pinMode(INPUTDRIVER_ENCODER_UP, INPUT_PULLUP);
        attachInterrupt(INPUTDRIVER_ENCODER_UP, intUpHandler, RISING);
#endif
#ifdef INPUTDRIVER_ENCODER_DOWN
        pinMode(INPUTDRIVER_ENCODER_DOWN, INPUT_PULLUP);
        attachInterrupt(INPUTDRIVER_ENCODER_DOWN, intDownHandler, RISING);
#endif
#ifdef INPUTDRIVER_ENCODER_BTN
        pinMode(INPUTDRIVER_ENCODER_BTN, INPUT);
#endif
    }

    encoder = lv_indev_create();
    lv_indev_set_type(encoder, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(encoder, encoder_read);

    if (!inputGroup) {
        inputGroup = lv_group_create();
        lv_group_set_default(inputGroup);
    }
    lv_indev_set_group(encoder, inputGroup);
}

void EncoderInputDriver::encoder_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    // encoder w/o interrupts but read GPIOs directly
    if (INPUTDRIVER_ENCODER_TYPE == 1) {
#ifdef INPUTDRIVER_ENCODER_LEFT
        if (digitalRead(INPUTDRIVER_ENCODER_LEFT))
            data->enc_diff = -1;
#endif
#ifdef INPUTDRIVER_ENCODER_RIGHT
        if (digitalRead(INPUTDRIVER_ENCODER_RIGHT))
            data->enc_diff = 1;
#endif
#ifdef INPUTDRIVER_ENCODER_BTN
        // FIXME: need same logix as below to trigger LONG_PRESSED events
        if (!digitalRead(INPUTDRIVER_ENCODER_BTN)) {
            data->key = LV_KEY_ENTER;
            data->state = LV_INDEV_STATE_PRESSED;
        }
#endif
    }
    // trackball/joystick with additional up/down inputs to control sliders
    else if (INPUTDRIVER_ENCODER_TYPE == 3) {
        static uint32_t prevkey = 0;
        static uint32_t lastPressed = millis();
        static uint32_t lastClickMs = 0;      // for the double-click gesture
        static uint32_t tbClickPendingAt = 0; // a single click waiting out the double window

        data->key = 0;
        data->enc_diff = 0;
        data->state = LV_INDEV_STATE_RELEASED;

        // A single click that has now outlived the double-click window is a tap. Resolved here
        // rather than in the block below because it has to fire on a read where nothing at all
        // happened - that is the whole point of waiting.
        if (tbClickPendingAt && millis() - tbClickPendingAt > 320) {
            tbClickPendingAt = 0;
            tbTapNow = true;
        }

#ifdef INPUTDRIVER_ENCODER_BTN
        // Fire PRESSED only on the button's DOWN EDGE (released -> pressed). The button is
        // polled every read, so a normal click stays "down" across several polls; counting
        // each poll as a click made every single click look like a double -> Home. Tracking
        // the edge means one physical click == exactly one PRESSED.
        static bool btnWasDown = false;
        static uint32_t btnDownAt = 0;
        static bool longFired = false;
        bool btnDown = !digitalRead(INPUTDRIVER_ENCODER_BTN);

        // HOLD -> Home, for everyone, in every mode. It is deliberately not a setting: a hold
        // costs nothing to detect (a quick click is unambiguous the moment the button comes back
        // up, unlike a double-click, which you can only recognise by waiting out the whole
        // window), and it guarantees there is always one gesture that gets you out no matter how
        // the two switches are set. Double-click still goes Home too, whenever the click is not
        // being used to select.
        if (btnDown && !btnWasDown) {
            btnDownAt = millis();
            longFired = false;
        } else if (btnDown && !longFired && millis() - btnDownAt > 1500) {
            // HOLD = BACK, always, in every mode. Jake: "lets make hold to back work always.
            // hold should be lets say 1.5 seconds of hold. something a click shouldnt mess
            // with" - and at 1.5s it cannot be confused with a click, which is what lets it
            // work with the cursor on as well. Nothing here ever holds a press down on what
            // the cursor is over (the pointer emits a one-read tap), so there is no conflict.
            //
            // Two exceptions, both because Back has nothing to do:
            //   dark screen -> wake
            //   already on Home -> LOCK. Jake: "holding home wont lock it anymore either,
            //   just double click". Making hold mean Back everywhere was his call and it is
            //   right everywhere except Home, where there is nowhere to go back TO - so the
            //   gesture was simply doing nothing there. Now both of his routes to lock work.
            if (tdeck_input_gated || tdeck_on_home_screen())
                tb_home_request = true; // asleep -> wake, on Home -> lock
            else
                tdeck_back_request = true; // awake, somewhere else -> Back
            longFired = true;
        } else if (!btnDown && btnWasDown && !longFired) {
            // A quick click. What it MEANS is decided in the block below: either a select, or
            // one half of the old double-click gesture.
            action = TB_ACTION_PRESSED;
        }
        btnWasDown = btnDown;
#endif
        // slow down repeating key to max. four events per second
        // the button is an exception for LONG_PRESSED monitoring
        // Kept-lit lock screen: any trackball activity brightens it for three seconds and
        // nothing else. The double-click that goes Home still works, because that is handled
        // above this and sets tb_home_request directly.
        if (action != TB_ACTION_NONE && tdeck_stayon_active()) {
            tdeck_stayon_boost_request = true;
        }
        if (action != TB_ACTION_NONE && (action == TB_ACTION_PRESSED || millis() > lastPressed + 250)) {
            // On the PIN screen the trackball must not select or navigate anything: the group
            // still holds the launcher's tiles, so a click here would press a button behind the
            // lock. Holding for Home is handled above and still works, which is the one gesture
            // that should survive.
            if (tdeck_lockscreen_active()) {
                data->key = 0;
                data->enc_diff = 0;
                data->state = LV_INDEV_STATE_RELEASED;
                tb_nav_rows = 0;
            } else if (false) { // click-to-select: gone with its switch, see the note below
                // Click-to-select only means anything when there is a highlight to select, i.e.
                // when navigation is on. Requiring BOTH matters: navigation is currently disabled
                // (see tdeckRefreshNavGroup), so with only the click switch on the click sent
                // Enter into an empty group - it selected nothing AND it suppressed the
                // double-click that goes Home, leaving no obvious way out of an app. Tying the
                // two together means the old double-click gesture always comes back whenever
                // navigation is not actually running.
                data->key = LV_KEY_ENTER;
                data->state = LV_INDEV_STATE_PRESSED;
            } else if (action == TB_ACTION_PRESSED && tdeck_trackball_nav_enabled() &&
                       !tdeck_input_gated) {
                // Cursor mode, and only while awake. Asleep, a click belongs to the
                // double-click-to-wake gesture in the branch below - the cursor is not on
                // screen and cannot be the thing a click means.
                // Single click presses what the cursor is over; double click goes
                // Back. Telling them apart means waiting out the window before acting on a
                // single one, so it is kept short - 320ms is long enough for a deliberate
                // double and short enough that a single click still feels immediate.
                tbCursorInit();
                const uint32_t nowMs = millis();
                if (lastClickMs && nowMs - lastClickMs < 320) {
                    // Double-click is Home - and on Home it locks the device, which is the
                    // gesture Jake noticed had gone missing. Back moved to the hold.
                    tb_home_request = true;
                    lastClickMs = 0;
                    tbClickPendingAt = 0; // and cancel the single that was waiting
                } else {
                    lastClickMs = nowMs;
                    tbClickPendingAt = nowMs;  // a tap, unless a second click arrives
                }
            } else if (action == TB_ACTION_PRESSED) {
                // Cursor off: the click never selects (selection is by touch). It only feeds
                // the double-click -> Home gesture, which also wakes the screen if it's asleep.
                uint32_t nowMs = millis();
                // Generous window: two clicks within 1.5s count as a double-click. The stiff
                // trackball in Jake's 3D case makes fast double-clicks hard; single-click has
                // no action so a wide window costs nothing.
                if (nowMs - lastClickMs < 1500) {
                    tb_home_request = true; // second click within window -> Home (+ wake)
                    lastClickMs = 0;        // reset so a 3rd click starts a fresh pair
                } else {
                    lastClickMs = nowMs; // first click: remember it, emit nothing
                }
            }
            // Trackball ROLL is inert by DEFAULT: all navigation/selection is by touch, and
            // UP/DOWN/LEFT/RIGHT are consumed here with no output — only the button double-click
            // above drives Home / lock / wake. That is a deliberate choice, not a dead trackball,
            // but it looks identical to broken hardware from the outside (t-ui-firmware#1), so
            // Settings -> Trackball navigation turns rolling back on for people who want it.
            // Switched on, a roll emits the ordinary LVGL arrow keys, which is what used to move
            // the focus highlight and flip launcher pages.
            // Rolling moves the cursor, when the user has asked for it.
            if (tdeck_trackball_nav_enabled() && action != TB_ACTION_PRESSED &&
                action != TB_ACTION_NONE) {
                // Only bring the cursor into existence. The pulses themselves were already
                // counted by the interrupts and get integrated once per frame - reacting to
                // them HERE is exactly what made every single event a jump.
                tbCursorInit();
            }
            // Roll stays inert otherwise, and the stored flags are deliberately NOT consulted.
            // The switches are gone from Settings, so anyone who had turned them on would
            // otherwise be left with a trackball that shuffles focus around the node list and no
            // way to stop it. Hold-for-Home and double-click-for-Home both still work, and
            // neither is a setting.
            else if (false) {
                // PREV/NEXT, not the arrow keys. In LVGL an arrow key is delivered TO the
                // focused widget - it means "act on this thing" - so on a vertical-scrolling
                // screen LVGL turns left/right into a scroll and leaves up/down to a widget
                // that usually ignores them. That is exactly backwards for a trackball, and it
                // is why the first build scrolled Settings with LEFT and RIGHT while up and
                // down did nothing. PREV/NEXT move the FOCUS between widgets, which is what
                // rolling should do, and LVGL scrolls the newly focused item into view for free.
                switch (action) {
                case TB_ACTION_LEFT:
                    data->key = LV_KEY_PREV;
                    break;
                case TB_ACTION_RIGHT:
                    data->key = LV_KEY_NEXT;
                    break;
                case TB_ACTION_UP:
                    tb_nav_rows = -1; // a whole row; the launcher decides how wide that is
                    break;
                case TB_ACTION_DOWN:
                    tb_nav_rows = 1;
                    break;
                default:
                    break;
                }
                if (data->key)
                    data->state = LV_INDEV_STATE_PRESSED;
            }

            // Screen dark/locked: drop any roll movement (the double-click above still
            // sets tb_home_request, which is how the screen gets woken).
            if (tdeck_input_gated) {
                data->key = 0;
                data->enc_diff = 0;
                data->state = LV_INDEV_STATE_RELEASED;
            }

            lastPressed = millis();
            prevkey = data->key;
            action = TB_ACTION_NONE;
        } else {
            // this logic is required for LONG_PRESSED event, see lv_indev.c
            if (prevkey != 0) {
                data->state = LV_INDEV_STATE_RELEASED;
                data->key = prevkey;
                prevkey = 0;
            }
        }
    }
}

// Each axis keeps its OWN count. `action` is still written because the click handling reads
// it, but it is no longer what moves the cursor: a shared one-slot variable cannot carry a
// diagonal, which is why rolling on the angle never worked.
void EncoderInputDriver::intPressHandler()
{
    action = TB_ACTION_PRESSED;
}

void EncoderInputDriver::intDownHandler()
{
    action = TB_ACTION_DOWN;
    tbRawY++;
}

void EncoderInputDriver::intUpHandler()
{
    action = TB_ACTION_UP;
    tbRawY--;
}

void EncoderInputDriver::intLeftHandler()
{
    action = TB_ACTION_LEFT;
    tbRawX--;
}

void EncoderInputDriver::intRightHandler()
{
    action = TB_ACTION_RIGHT;
    tbRawX++;
}

#endif
