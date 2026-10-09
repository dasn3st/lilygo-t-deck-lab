// -----------------------------------------------------------------------------
// LuaApp — device-ui side of the Lua app engine (firmware side: src/TDeckLua.cpp).
//
// Stage 3: apps run from Lua scripts ON THE SD CARD. The device seeds the card
// with the bundled apps (their .lua files under /apps/<name>/) and then loads +
// runs them straight off the card — so they're real files you can find, copy, and
// eventually replace with your own. Drawing is retained-by-id (no per-frame
// churn); a fresh screen is built per open and torn down on exit; the Lua heap
// lives in PSRAM. Toolbox: screen.label / screen.box / screen.hide / device.beep
// / device.time / store.read / store.write, plus on_open / on_tick / on_touch /
// on_drag / on_close. store.* is jailed to the app's own /apps/<name>/ folder
// (simple filenames only) so apps can keep high scores etc. but can't touch
// anything else on the card.
// -----------------------------------------------------------------------------
#include "graphics/common/SdCard.h" // SDFs (shared SdFat instance)
#include "lvgl.h"
#include "util/ILog.h" // so a failed launch says WHY instead of showing a black screen
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>

extern "C" void lua_app_open(void);   // "Lua" tile — the hello demo

// firmware-side engine (linked across the boundary)
extern "C" int tdeck_lua_app_start(const char *script);
extern "C" void tdeck_lua_app_tick(uint32_t dt);
extern "C" void tdeck_lua_app_touch(int x, int y);
extern "C" void tdeck_lua_app_drag(int x, int y);
extern "C" void tdeck_lua_app_stop(void);
extern "C" void tdeck_ui_canvas_free(void); // defined below; frees the game frame buffer
extern "C" void tdeck_lua_app_key(uint32_t key); // TDeckLua.cpp: hands a key to the app's on_key

// Multi-touch reader, installed by LGFXDriver<LGFX>::init (it owns the touch
// hardware handle). Null until the display driver comes up.
int (*tdeck_touch_reader)(unsigned short *xs, unsigned short *ys, int max) = nullptr;

// Bridge for the firmware-side Lua engine: device.touches() lands here. Returns the
// number of fingers on the screen (up to `max`), or -1 when no reader is installed.
extern "C" int tdeck_touch_read(unsigned short *xs, unsigned short *ys, int max)
{
    return tdeck_touch_reader ? tdeck_touch_reader(xs, ys, max) : -1;
}

namespace
{
lv_obj_t *luaScreen = nullptr;
lv_timer_t *luaTick = nullptr;

struct UObj {
    int id;
    int type; // 0 = label, 1 = box, 2 = line
    lv_obj_t *obj;
    lv_point_precise_t pts[2]; // line endpoints (lv_line keeps a pointer to these — must persist)
    int32_t cp[5];             // last-drawn geometry — lets a repeat call skip LVGL entirely
    uint32_t ccol;             // last-drawn color
};
UObj uobjs[80];
constexpr int kMaxObj = 80;
int uobjCount = 0;
// Script buffer lives in PSRAM so it doesn't eat the scarce internal heap. loadScript keeps
// it across launches and only ever grows it to fit the biggest app actually run this session.
// 192K: was 96K (and 48K before). Deep Space is ~82K and has room to keep growing, so the
// ceiling is doubled again. This costs NOTHING until an app really is that big: the cap is a
// sanity limit, not a reservation — loadScript asks only for the size of the file in front of
// it. (Stays under the 256K Get Apps download ceiling in TFTView, so anything that installs
// can also launch.)
constexpr int kScriptCap = 196608;
char *scriptBuf = nullptr;
int scriptBufCap = 0; // how much scriptBuf actually holds; grows to fit the biggest app run
// The running app's own folder on the SD card ("" = none/SD unavailable). Set per
// launch; store.read/write are jailed inside it.
char appDir[64] = "";

// Returns the slot for (id,type), creating the LVGL object on first use. nullptr if full.
UObj *findOrCreateSlot(int id, int type)
{
    for (int i = 0; i < uobjCount; i++)
        if (uobjs[i].id == id && uobjs[i].type == type)
            return &uobjs[i];
    if (uobjCount >= kMaxObj || !luaScreen)
        return nullptr;
    lv_obj_t *o;
    if (type == 0) {
        o = lv_label_create(luaScreen);
    } else if (type == 2) {
        o = lv_line_create(luaScreen);
        // pos/size are set per draw to the line's own bounding box — a full-screen
        // object here made every flipper move invalidate the whole 320x240
        lv_obj_set_style_pad_all(o, 0, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(o, 0, 0);
    } else {
        o = lv_obj_create(luaScreen);
        lv_obj_remove_style_all(o);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE); // taps fall through to the screen
    uobjs[uobjCount].id = id;
    uobjs[uobjCount].type = type;
    uobjs[uobjCount].obj = o;
    uobjs[uobjCount].cp[0] = INT32_MIN; // never matches -> first draw always lands
    uobjs[uobjCount].ccol = 0;
    return &uobjs[uobjCount++];
}

// Keys arrive on the LVGL input-read callback, which is NOT where we want to run Lua —
// so the driver drops them in this little ring and the app's tick drains it. Sixteen is
// far more than a human can type in one frame; if it ever overflowed we'd rather drop the
// oldest key than block the input driver.
constexpr int kKeyQueueLen = 16;
volatile uint32_t keyQueue[kKeyQueueLen];
volatile uint8_t keyHead = 0, keyTail = 0;

void tickCb(lv_timer_t *)
{
    while (keyTail != keyHead) {
        uint32_t k = keyQueue[keyTail];
        keyTail = (uint8_t)((keyTail + 1) % kKeyQueueLen);
        tdeck_lua_app_key(k);
    }
    tdeck_lua_app_tick(33);
}

// Write the bundled script to the SD card (overwrite so a firmware update always
// wins for OUR demos; user-added apps live in other folders and are never touched).
void seedApp(const char *dir, const char *path, const char *content)
{
    SDFs.mkdir("/apps");
    SDFs.mkdir(dir);
    FsFile f = SDFs.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (f) {
        f.write((const uint8_t *)content, strlen(content));
        f.close();
    }
}

// Load a script off the SD card into scriptBuf; returns nullptr if it can't.
const char *loadScript(const char *path)
{
    FsFile f = SDFs.open(path, O_RDONLY);
    if (!f) {
        ILOG_WARN("lua: cannot open %s", path);
        return nullptr;
    }
    int size = (int)f.fileSize();
    if (size <= 0 || size >= kScriptCap) {
        ILOG_WARN("lua: %s is %d bytes, cap is %d", path, size, kScriptCap);
        f.close();
        return nullptr;
    }

    // Allocate what THIS script needs, not the maximum a script may ever be. Claiming a
    // flat kScriptCap was survivable at 48K and broke launching entirely at 96K: it wants
    // one unbroken run of PSRAM, every app pays the largest app's price, and there often
    // isn't a free block that big once maps and the node cache have been running. A 46K
    // game now asks for 46K. The buffer is kept between launches and only ever grows, so
    // repeated launches don't churn the heap.
    if (size + 1 > scriptBufCap) {
        char *nb = (char *)heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
        if (!nb) {
            // Never fail silently again. A null here used to mean a black screen and an
            // empty log, which is the worst possible way for this to go wrong.
            ILOG_ERROR("lua: no PSRAM for %d bytes (%s) - largest free block %u", size + 1, path,
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            f.close();
            return nullptr;
        }
        if (scriptBuf)
            heap_caps_free(scriptBuf);
        scriptBuf = nb;
        scriptBufCap = size + 1;
    }

    int n = f.read((uint8_t *)scriptBuf, size);
    f.close();
    if (n <= 0) {
        ILOG_WARN("lua: read of %s returned %d", path, n);
        return nullptr;
    }
    scriptBuf[n] = 0;
    return scriptBuf;
}

// Remember the app's own folder (from ".../main.lua" -> "..."), for store.* jail.
void setAppDir(const char *scriptPath)
{
    appDir[0] = 0;
    if (!scriptPath)
        return;
    const char *slash = strrchr(scriptPath, '/');
    if (!slash || slash == scriptPath)
        return;
    size_t n = (size_t)(slash - scriptPath);
    if (n >= sizeof(appDir))
        return;
    memcpy(appDir, scriptPath, n);
    appDir[n] = 0;
}

// A store filename is a plain name like "hiscore.txt" — no paths, no dot-dot.
bool storeNameOk(const char *name)
{
    if (!name || !name[0] || strlen(name) > 32)
        return false;
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\'))
        return false;
    return true;
}

// Common open path: seed the card, load the script FROM the card, run it.
void runFromSd(const char *dir, const char *path, const char *bundled)
{
    if (luaScreen) {
        tdeck_ui_canvas_free(); // release the 150KB frame buffer with the old screen
        lv_obj_delete(luaScreen);
        luaScreen = nullptr;
    }
    uobjCount = 0;

    luaScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(luaScreen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(luaScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(luaScreen, LV_OBJ_FLAG_CLICKABLE);

    // NOTE: we no longer seed here — install happens once at boot (lua_seed_bundled),
    // so opening an app never re-creates a file the user deleted (that would defeat
    // uninstall). We just load whatever's on the card, falling back to the built-in
    // copy if the card is unavailable.
    (void)dir;
    setAppDir(path); // the app's own folder = its store.* jail
    const char *script = loadScript(path); // genuinely runs off the SD card
    if (!script)
        script = bundled; // SD unavailable / not installed -> fall back so it still works
    if (!script)
        return; // nothing to run (a user app whose file is gone) — bail safely
    tdeck_lua_app_start(script);

    if (!luaTick)
        luaTick = lv_timer_create(tickCb, 33, NULL);
    else
        lv_timer_resume(luaTick);

    // PRESSED (finger lands), not CLICKED (finger lifts) — a flipper that fires on
    // release feels like lag; every app here wants the tap the moment it happens.
    lv_obj_add_event_cb(
        luaScreen,
        [](lv_event_t *) {
            lv_indev_t *d = lv_indev_active();
            if (!d)
                return;
            lv_point_t p;
            lv_indev_get_point(d, &p);
            tdeck_lua_app_touch((int)p.x, (int)p.y);
        },
        LV_EVENT_PRESSED, NULL);
    // Continuous position while the finger is down + moving -> on_drag (paddle steering, etc.)
    lv_obj_add_event_cb(
        luaScreen,
        [](lv_event_t *) {
            lv_indev_t *d = lv_indev_active();
            if (!d)
                return;
            lv_point_t p;
            lv_indev_get_point(d, &p);
            tdeck_lua_app_drag((int)p.x, (int)p.y);
        },
        LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(
        luaScreen,
        [](lv_event_t *) {
            if (luaTick)
                lv_timer_pause(luaTick);
            tdeck_lua_app_stop();
            // Hand back the 150KB frame buffer as soon as the app is left, rather than
            // holding it until some other app happens to open.
            tdeck_ui_canvas_free();
        },
        LV_EVENT_SCREEN_UNLOADED, NULL);

    lv_screen_load_anim(luaScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// ---- bundled app scripts (seeded to the SD card, then loaded from it) ---------
const char *HELLO_SCRIPT = R"LUA(
local x,y = 150,110
local dx,dy = 4,3
function on_open()
  screen.label(1, 8, 8, 'Hello from Lua!', 0xffffff)
  screen.label(2, 8, 220, 'this app is a file on your SD card', 0x8e8e93)
end
function on_tick(dt)
  x = x + dx; y = y + dy
  if x < 8 or x > 290 then dx = -dx end
  if y < 40 or y > 208 then dy = -dy end
  screen.box(3, x, y, 22, 22, 0x30d158)
end
function on_touch(tx, ty) device.beep() end
)LUA";

#if 0 // Breakout removed from the field-tool firmware; keep the old source recoverable.
const char *BREAKOUT_SCRIPT = R"LUA(
-- Breakout 2.0 for T-UI ---------------------------------------------------
-- 5 levels (then they loop, faster), falling power-ups, 3 lives, and a high
-- score saved to hiscore.txt in this app's folder on the SD card.
-- Power-ups: W = wide paddle (12s) · L = extra life · B = +50 points
---------------------------------------------------------------------------
local W = 320
local basePw, widePw, ph, py = 56, 88, 10, 188
local pw = basePw
local sy, sh = 204, 36               -- the steer strip: big, for big thumbs
local bs = 8                         -- ball size
local px, pt = 132, 132              -- paddle x + where the thumb wants it
local bx, by = 156.0, 150.0          -- ball position
local vx, vy = 4.0, -4.8             -- ball velocity
local sc, lives, lv, loop = 0, 3, 1, 0
local hs = 0
local st = 0                         -- 0 play · 1 game over · 2 level clear · 3 ball lost
local waitUntil, wideUntil = 0, 0
local cols, rows = 8, 4
local bw, bh, gp, lx, ty = 36, 12, 3, 6, 28
local col = {0xff453a, 0xff9f0a, 0xffd60a, 0x30d158, 0x0a84ff}
local live = {}
local dtype, dpx, dpy = 0, 0, 0.0    -- falling power-up (0 = none)

local LEVELS = {
  {'11111111','22222222','33333333','44444444'},
  {'1.1.1.1.','.2.2.2.2','3.3.3.3.','.4.4.4.4'},
  {'...11...','..2222..','.333333.','44444444'},
  {'11.22.11','33.44.33','11.22.11','55.55.55'},
  {'55555555','44444444','33333333','22222222','11111111'},
  {'55555555','5......5','5..11..5','5......5','55555555'},
  {'1.2.3.4.','1.2.3.4.','1.2.3.4.','1.2.3.4.','1.2.3.4.'},
  {'...55...','..5445..','.543345.','..5445..','...55...'},
  {'22..22..','..33..33','44..44..','..55..55'},
  {'5.5.5.5.','44444444','33333333','2.2.2.2.','11111111'},
}

local function speed() return 4.4 + (lv - 1) * 3 / 5 + loop end

local function hud()
  screen.label(3, 4, 4, 'Score ' .. sc, 0xffffff)
  screen.label(6, 244, 4, 'Best ' .. hs, 0xffd60a)
  screen.label(7, 118, 4, 'Lv ' .. lv .. '   x' .. lives, 0x8e8e93)
end

local function msg(t, c)
  screen.label(4, 88, 108, t, c or 0xffffff)
end

local function buildLevel()
  local pat = LEVELS[lv]
  rows = #pat
  for i = 0, 39 do live[i] = false; screen.hide(100 + i) end
  for r = 1, rows do
    local row = pat[r]
    for c = 1, cols do
      local ch = string.sub(row, c, c)
      if ch ~= '.' then
        local i = (r - 1) * cols + (c - 1)
        live[i] = true
        screen.box(100 + i, lx + (c - 1) * (bw + gp), ty + (r - 1) * (bh + gp), bw, bh, col[tonumber(ch)])
      end
    end
  end
end

local function resetBall()
  bx, by = px + pw / 2 - bs / 2, 150.0
  local s = speed()
  vy = -s
  if math.random(2) == 1 then vx = s * 3 / 5 else vx = -s * 3 / 5 end
end

local function newGame()
  sc, lives, lv, loop = 0, 3, 1, 0
  pw, wideUntil = basePw, 0
  dtype = 0; screen.hide(30); screen.hide(31)
  st = 0
  msg('', 0)
  buildLevel()
  resetBall()
  hud()
end

local function steer(x)
  pt = x - pw // 2
  if pt < 0 then pt = 0 end
  if pt > W - pw then pt = W - pw end
end

local function saveScore()
  if sc > hs then
    hs = sc
    store.write('hiscore.txt', tostring(hs))
    hud()
  end
end

local function dropPowerup(x, y)
  if dtype ~= 0 or math.random(4) ~= 1 then return end
  local r = math.random(20)  -- weighted: wide 40%, points 45%, extra life only 15%
  if r <= 8 then dtype = 1 elseif r <= 17 then dtype = 3 else dtype = 2 end
  dpx, dpy = x + bw // 2 - 8, y + 0.0
end

local function applyPowerup()
  if dtype == 1 then wideUntil = device.time() + 12000
  elseif dtype == 2 then lives = lives + 1
  else sc = sc + 50 end
  hud()
  device.beep(true)
  dtype = 0; screen.hide(30); screen.hide(31)
end

function on_open()
  math.randomseed(device.time())
  hs = math.floor(tonumber(store.read('hiscore.txt') or '') or 0)
  screen.box(20, 0, sy, W, sh, 0x2c2c2e)                -- the steer strip
  screen.label(5, 104, sy + 10, 'drag here to steer', 0x6e6e73)
  newGame()
end

function on_touch(x, y)
  if st == 1 then newGame() return end
  steer(x)
end

function on_drag(x, y)
  if st ~= 1 then steer(x) end
end

function on_tick(dt)
  local now = device.time()

  -- wide-paddle power-up wears off
  local wantPw = basePw
  if now < wideUntil then wantPw = widePw end
  if wantPw ~= pw then pw = wantPw; steer(px + pw // 2) end

  if st == 2 or st == 3 then                -- brief pause, then next level / next ball
    if now >= waitUntil then
      if st == 2 then
        lv = lv + 1
        if lv > #LEVELS then lv = 1; loop = loop + 1 end
        buildLevel()
        hud()
      end
      msg('', 0)
      resetBall()
      st = 0
    end
  elseif st == 0 then
    -- paddle eases toward the thumb
    local d = pt - px
    if d > 16 then d = 16 elseif d < -16 then d = -16 end
    px = px + d

    bx = bx + vx
    by = by + vy
    if bx < 0 then bx = 0; vx = -vx end
    if bx > W - bs then bx = W - bs; vx = -vx end
    if by < 20 then by = 20; vy = -vy end

    -- paddle bounce (only while heading down), with edge english
    if vy > 0 and by + bs >= py and by <= py + ph and bx + bs >= px and bx <= px + pw then
      vy = -math.abs(vy)
      vx = ((bx + bs / 2) - (px + pw / 2)) / 4.5
      if vx > 7 then vx = 7 elseif vx < -7 then vx = -7 end
      if vx < 2 and vx > -2 then
        if vx < 0 then vx = -2 else vx = 2 end
      end
      device.beep()
    end

    -- brick hits
    local done = false
    for r = 0, rows - 1 do
      if done then break end
      for c = 0, cols - 1 do
        local i = r * cols + c
        if live[i] then
          local qx = lx + c * (bw + gp)
          local qy = ty + r * (bh + gp)
          if bx + bs >= qx and bx <= qx + bw and by + bs >= qy and by <= qy + bh then
            live[i] = false
            screen.hide(100 + i)
            vy = -vy
            sc = sc + 10
            hud()
            dropPowerup(qx, qy)
            device.beep()
            done = true
            break
          end
        end
      end
    end

    -- ball past the paddle = ball lost
    if by > sy then
      lives = lives - 1
      hud()
      if lives <= 0 then
        st = 1
        saveScore()
        msg('Game Over - tap to retry', 0xff453a)
      else
        st = 3
        waitUntil = now + 900
        msg('Ball lost!', 0xff9f0a)
      end
    end

    -- level cleared?
    local any = false
    for i = 0, rows * cols - 1 do if live[i] then any = true break end end
    if not any and st == 0 then
      st = 2
      waitUntil = now + 900
      sc = sc + 25
      saveScore()
      msg('Level up!', 0x30d158)
      device.beep(true)
    end
  end

  -- falling power-up
  if dtype ~= 0 then
    dpy = dpy + 2.2
    local letters = {'W', 'L', 'B'}
    local dcol = {0x0a84ff, 0x30d158, 0xffd60a}
    screen.box(30, dpx, math.floor(dpy), 16, 16, dcol[dtype])
    screen.label(31, dpx + 4, math.floor(dpy), letters[dtype], 0x000000)
    if dpy + 16 >= py and dpy <= py + ph and dpx + 16 >= px and dpx <= px + pw then
      applyPowerup()
    elseif dpy > sy then
      dtype = 0; screen.hide(30); screen.hide(31)
    end
  end

  screen.box(2, math.floor(px), py, pw, ph, 0x0a84ff)                    -- paddle
  if st == 0 then screen.box(1, math.floor(bx), math.floor(by), bs, bs, 0xffffff)
  else screen.hide(1) end
  screen.box(21, math.floor(px) + pw // 2 - 17, sy + 3, 34, 30, 0x0a84ff) -- strip knob
end
)LUA";
#endif

} // namespace

// ---- drawing bridges the Lua toolbox calls (from src/TDeckLua.cpp) -----------
// ===== Physical keyboard for apps ===============================================
//
// Is a Lua app the thing on screen right now? The keyboard driver asks before handing
// a key to an app, so keys only ever divert while an app is genuinely in front of the
// user — typing in Notes, the PIN pad and the wake-on-keypress path are untouched.
extern "C" bool tdeck_lua_app_focused(void)
{
    return luaScreen != nullptr && lv_screen_active() == luaScreen;
}

// Called from the keyboard driver's read callback. Queues the key for the app's next
// tick; running Lua straight from an input callback would be asking for trouble.
extern "C" void tdeck_lua_queue_key(uint32_t key)
{
    uint8_t next = (uint8_t)((keyHead + 1) % kKeyQueueLen);
    if (next == keyTail)
        return; // full: drop it rather than stall the input driver
    keyQueue[keyHead] = key;
    keyHead = next;
}

// ===== Canvas: a real pixel surface for games ===================================
//
// The label/box/line API above is a UI toolkit — every element is a full LVGL object,
// and there's a hard ceiling of 80 of them. That's fine for tools and just about
// stretches to Pinball, but a tile map or a particle effect needs hundreds of things
// on screen, which that model can't reach at any speed.
//
// The canvas is ONE LVGL object holding a 320x240 RGB565 buffer in PSRAM (150KB of the
// spare 8MB). Apps draw into it as pixels, so the element ceiling stops applying.
// Crucially every primitive below runs in C: Lua decides *what* to draw, this decides
// *how*, which is what makes hundreds of moving sprites affordable on this chip.
//
// It sits behind the label/box elements, so an app can draw a game world on the canvas
// and still put a crisp text score on top using the old API.
namespace
{
lv_obj_t *luaCanvas = nullptr;
uint16_t *canvasBuf = nullptr;
int32_t canvasW = 0, canvasH = 0;
uint32_t canvasStridePx = 0; // row length in PIXELS (LVGL may pad it beyond the width)

inline uint16_t rgb565(uint32_t c)
{
    return (uint16_t)((((c >> 16) & 0xFF) >> 3) << 11 | (((c >> 8) & 0xFF) >> 2) << 5 | ((c & 0xFF) >> 3));
}

// Clip a rectangle to the canvas; returns false if nothing is left to draw. Every
// primitive goes through this, so an app can pass wild coordinates (an off-screen
// sprite, a negative position) without ever writing outside the buffer.
inline bool clipRect(int &x, int &y, int &w, int &h)
{
    if (!canvasBuf || w <= 0 || h <= 0)
        return false;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= canvasW || y >= canvasH)
        return false;
    if (x + w > canvasW) w = canvasW - x;
    if (y + h > canvasH) h = canvasH - y;
    return w > 0 && h > 0;
}
} // namespace

extern "C" int tdeck_ui_canvas_begin(void)
{
    if (luaCanvas)
        return 1;
    if (!luaScreen)
        return 0;

    canvasW = 320;
    canvasH = 240;
    canvasStridePx = lv_draw_buf_width_to_stride(canvasW, LV_COLOR_FORMAT_RGB565) / 2;
    size_t bytes = (size_t)canvasStridePx * canvasH * 2;

    canvasBuf = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!canvasBuf) {
        // Out of PSRAM: canvas.begin() returns false and the app can fall back to the
        // ordinary screen.* API rather than dying.
        LV_LOG_ERROR("Lua canvas: no PSRAM for the frame buffer");
        return 0;
    }
    memset(canvasBuf, 0, bytes);

    luaCanvas = lv_canvas_create(luaScreen);
    lv_canvas_set_buffer(luaCanvas, canvasBuf, canvasW, canvasH, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(luaCanvas, 0, 0);
    lv_obj_clear_flag(luaCanvas, LV_OBJ_FLAG_CLICKABLE); // taps belong to the screen
    lv_obj_move_background(luaCanvas);                   // labels/boxes stay on top
    return 1;
}

extern "C" void tdeck_ui_canvas_clear(uint32_t color)
{
    if (!canvasBuf)
        return;
    uint16_t c = rgb565(color);
    // Fill one row then copy it down — memcpy beats a per-pixel loop by a wide margin.
    uint16_t *row = canvasBuf;
    for (int32_t x = 0; x < canvasW; x++)
        row[x] = c;
    for (int32_t y = 1; y < canvasH; y++)
        memcpy(canvasBuf + (size_t)y * canvasStridePx, row, (size_t)canvasW * 2);
}

extern "C" void tdeck_ui_canvas_rect(int x, int y, int w, int h, uint32_t color)
{
    if (!clipRect(x, y, w, h))
        return;
    uint16_t c = rgb565(color);
    for (int row = 0; row < h; row++) {
        uint16_t *p = canvasBuf + (size_t)(y + row) * canvasStridePx + x;
        for (int col = 0; col < w; col++)
            p[col] = c;
    }
}

extern "C" void tdeck_ui_canvas_pixel(int x, int y, uint32_t color)
{
    if (!canvasBuf || x < 0 || y < 0 || x >= canvasW || y >= canvasH)
        return;
    canvasBuf[(size_t)y * canvasStridePx + x] = rgb565(color);
}

// Bresenham — no floating point, and it clips per-pixel so any coordinates are safe.
extern "C" void tdeck_ui_canvas_line(int x1, int y1, int x2, int y2, uint32_t color)
{
    if (!canvasBuf)
        return;
    uint16_t c = rgb565(color);
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x1 >= 0 && y1 >= 0 && x1 < canvasW && y1 < canvasH)
            canvasBuf[(size_t)y1 * canvasStridePx + x1] = c;
        if (x1 == x2 && y1 == y2)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

// Midpoint circle. `filled` draws spans instead of an outline.
extern "C" void tdeck_ui_canvas_circle(int cx, int cy, int r, uint32_t color, int filled)
{
    if (!canvasBuf || r <= 0)
        return;
    uint16_t c = rgb565(color);
    int x = r, y = 0, err = 1 - r;
    auto plot = [&](int px, int py) {
        if (px >= 0 && py >= 0 && px < canvasW && py < canvasH)
            canvasBuf[(size_t)py * canvasStridePx + px] = c;
    };
    auto span = [&](int x0, int x1, int py) {
        if (py < 0 || py >= canvasH)
            return;
        if (x0 < 0) x0 = 0;
        if (x1 >= canvasW) x1 = canvasW - 1;
        uint16_t *p = canvasBuf + (size_t)py * canvasStridePx;
        for (int i = x0; i <= x1; i++)
            p[i] = c;
    };
    while (x >= y) {
        if (filled) {
            span(cx - x, cx + x, cy + y);
            span(cx - x, cx + x, cy - y);
            span(cx - y, cx + y, cy + x);
            span(cx - y, cx + y, cy - x);
        } else {
            plot(cx + x, cy + y); plot(cx - x, cy + y);
            plot(cx + x, cy - y); plot(cx - x, cy - y);
            plot(cx + y, cy + x); plot(cx - y, cy + x);
            plot(cx + y, cy - x); plot(cx - y, cy - x);
        }
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

// Blit a sprite. The art arrives as one character per pixel plus a lookup table the Lua
// side has already flattened from the app's palette, so this loop never calls back into
// Lua — it just reads bytes and writes pixels.
//
// `lut` is indexed by (character - 32), 95 entries. `opaque` says whether that character
// draws at all, which is how transparency works: any character with no palette entry
// (space, by convention) leaves the background alone.
//
// `scale` draws each sprite pixel as a scale x scale block, so 8x8 pixel art is actually
// visible on a 320x240 screen.
extern "C" void tdeck_ui_canvas_sprite(int x, int y, int w, int h, const char *pix, const uint32_t *lut,
                                       const uint8_t *opaque, int scale)
{
    if (!canvasBuf || !pix || w <= 0 || h <= 0)
        return;
    if (scale < 1)
        scale = 1;
    if (scale > 16)
        scale = 16;

    for (int sy = 0; sy < h; sy++) {
        int py0 = y + sy * scale;
        if (py0 + scale <= 0 || py0 >= canvasH)
            continue; // whole row is off-screen
        for (int sx = 0; sx < w; sx++) {
            unsigned char c = (unsigned char)pix[sy * w + sx];
            if (c < 32 || c > 126)
                continue;
            int idx = c - 32;
            if (!opaque[idx])
                continue; // transparent
            uint16_t col = rgb565(lut[idx]);

            int px0 = x + sx * scale;
            for (int dy = 0; dy < scale; dy++) {
                int py = py0 + dy;
                if (py < 0 || py >= canvasH)
                    continue;
                uint16_t *row = canvasBuf + (size_t)py * canvasStridePx;
                for (int dx = 0; dx < scale; dx++) {
                    int px = px0 + dx;
                    if (px < 0 || px >= canvasW)
                        continue;
                    row[px] = col;
                }
            }
        }
    }
}

// Show the frame. Nothing reaches the screen until this is called, so a half-drawn
// frame never appears — the app builds the whole picture, then flips it up.
extern "C" void tdeck_ui_canvas_flip(void)
{
    if (luaCanvas)
        lv_obj_invalidate(luaCanvas);
}

// Delete the canvas object BEFORE releasing its buffer — LVGL holds a pointer to it,
// and freeing first would leave the widget referencing dead memory.
extern "C" void tdeck_ui_canvas_free(void)
{
    if (luaCanvas) {
        lv_obj_delete(luaCanvas);
        luaCanvas = nullptr;
    }
    if (canvasBuf) {
        heap_caps_free(canvasBuf);
        canvasBuf = nullptr;
    }
    canvasW = canvasH = 0;
    canvasStridePx = 0;
}

extern "C" void tdeck_ui_label(int id, int x, int y, const char *text, uint32_t color)
{
    UObj *s = findOrCreateSlot(id, 0);
    if (!s)
        return;
    lv_obj_t *o = s->obj;
    if (!text)
        text = "";
    // Games redraw everything every tick; only touch LVGL when something changed,
    // or every repeat call invalidates (repaints) the area for nothing.
    if (s->cp[0] == x && s->cp[1] == y && s->ccol == color && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) &&
        strcmp(lv_label_get_text(o), text) == 0)
        return;
    s->cp[0] = x;
    s->cp[1] = y;
    s->ccol = color;
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(o, text);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
}

extern "C" void tdeck_ui_box(int id, int x, int y, int w, int h, uint32_t color, int radius)
{
    UObj *s = findOrCreateSlot(id, 1);
    if (!s)
        return;
    lv_obj_t *o = s->obj;
    if (s->cp[0] == x && s->cp[1] == y && s->cp[2] == w && s->cp[3] == h && s->cp[4] == radius && s->ccol == color &&
        !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
        return; // unchanged — e.g. redrawing all bumpers when only one flashed
    s->cp[0] = x;
    s->cp[1] = y;
    s->cp[2] = w;
    s->cp[3] = h;
    s->cp[4] = radius;
    s->ccol = color;
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0); // big radius + square size = a circle
}

// A thick line from (x1,y1) to (x2,y2) — angled bars for flippers, rails, lane
// guides. Rounded ends so it reads as a solid paddle. Points live in the slot so
// lv_line's retained pointer stays valid.
extern "C" void tdeck_ui_line(int id, int x1, int y1, int x2, int y2, int thickness, uint32_t color)
{
    UObj *s = findOrCreateSlot(id, 2);
    if (!s)
        return;
    if (s->cp[0] == x1 && s->cp[1] == y1 && s->cp[2] == x2 && s->cp[3] == y2 && s->cp[4] == thickness &&
        s->ccol == color && !lv_obj_has_flag(s->obj, LV_OBJ_FLAG_HIDDEN))
        return; // unchanged (a resting flipper redrawn every tick) — skip the repaint
    s->cp[0] = x1;
    s->cp[1] = y1;
    s->cp[2] = x2;
    s->cp[3] = y2;
    s->cp[4] = thickness;
    s->ccol = color;
    lv_obj_clear_flag(s->obj, LV_OBJ_FLAG_HIDDEN);
    // Size the object to the line's own bounding box (pad covers the rounded caps),
    // with the points relative to it — so moving a flipper repaints a small patch,
    // not the whole screen.
    int pad = thickness / 2 + 2;
    int ox = (x1 < x2 ? x1 : x2) - pad;
    int oy = (y1 < y2 ? y1 : y2) - pad;
    int w = (x1 > x2 ? x1 - x2 : x2 - x1) + pad * 2;
    int h = (y1 > y2 ? y1 - y2 : y2 - y1) + pad * 2;
    lv_obj_set_pos(s->obj, ox, oy);
    lv_obj_set_size(s->obj, w, h);
    s->pts[0].x = x1 - ox;
    s->pts[0].y = y1 - oy;
    s->pts[1].x = x2 - ox;
    s->pts[1].y = y2 - oy;
    lv_line_set_points(s->obj, s->pts, 2);
    lv_obj_set_style_line_width(s->obj, thickness, 0);
    lv_obj_set_style_line_color(s->obj, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(s->obj, true, 0);
}

extern "C" void tdeck_ui_hide(int id)
{
    for (int i = 0; i < uobjCount; i++)
        if (uobjs[i].id == id) {
            lv_obj_add_flag(uobjs[i].obj, LV_OBJ_FLAG_HIDDEN);
            return;
        }
}

// ---- app-folder file store (the store.* toolbox, jailed) ----------------------
// Reads /apps/<app>/<name> into buf; returns bytes read or -1.
extern "C" int tdeck_appfs_read(const char *name, char *buf, int cap)
{
    if (!appDir[0] || !storeNameOk(name) || !buf || cap <= 1)
        return -1;
    char path[104];
    snprintf(path, sizeof(path), "%s/%s", appDir, name);
    FsFile f = SDFs.open(path, O_RDONLY);
    if (!f)
        return -1;
    int n = f.read((uint8_t *)buf, cap - 1);
    f.close();
    if (n < 0)
        return -1;
    buf[n] = 0;
    return n;
}

// Writes /apps/<app>/<name> (overwrite). Returns true on success.
extern "C" bool tdeck_appfs_write(const char *name, const char *data, int len)
{
    if (!appDir[0] || !storeNameOk(name) || !data || len < 0)
        return false;
    char path[104];
    snprintf(path, sizeof(path), "%s/%s", appDir, name);
    FsFile f = SDFs.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f)
        return false;
    int wrote = f.write((const uint8_t *)data, (size_t)len);
    f.sync();
    f.close();
    return wrote == len;
}

// ---- launcher entry points ---------------------------------------------------
extern "C" void lua_app_open(void) { runFromSd("/apps/hello", "/apps/hello/main.lua", HELLO_SCRIPT); }

// Launch ANY user app by its SD-card script path (no bundled fallback). Used by the
// launcher for apps a user dropped onto the card themselves.
extern "C" void lua_run_path(const char *path)
{
    if (path && SDFs.exists(path))
        runFromSd("", path, nullptr);
}

// First-run installer: seed the bundled SD apps ONCE per firmware version. After
// this, the apps are files the user owns — delete a folder to uninstall it and it
// stays gone (we never re-seed unless the version marker bumps). Bump the marker
// name when a bundled script changes so the update reinstalls.
extern "C" void lua_seed_bundled(void)
{
    // Install each bundled app ONCE — only when its folder doesn't exist yet. The folder
    // then acts as a permanent tombstone: deleting an app's main.lua (a FILE, which the
    // on-device Files app can delete) uninstalls it for good, because the leftover folder
    // stops it from ever being recreated. (Removing the whole folder = a fresh reinstall.)
    SDFs.mkdir("/apps");
    // Remove the old game payloads from cards that were used with an earlier T-UI build.
    // These are deliberately explicit files: user-created tools remain untouched.
    const char *oldGameFiles[] = {
        "/apps/breakout/main.lua", "/apps/breakout/hiscore.txt", "/apps/breakout/.ver",
        "/apps/breakout/name.txt", "/apps/breakout/icon.txt", "/apps/snake/main.lua",
        "/apps/snake/hiscore.txt", "/apps/snake/.ver", "/apps/snake/name.txt", "/apps/snake/icon.txt",
    };
    for (const char *p : oldGameFiles)
        if (SDFs.exists(p))
            SDFs.remove(p);
    SDFs.rmdir("/apps/breakout");
    SDFs.rmdir("/apps/snake");
    // Metronome was a bundled tool; remove its old payload from cards during the next boot.
    const char *removedMetronomeFiles[] = {
        "/apps/metronome/main.lua", "/apps/metronome/.ver", "/apps/metronome/name.txt",
        "/apps/metronome/icon.txt",
    };
    for (const char *p : removedMetronomeFiles)
        if (SDFs.exists(p))
            SDFs.remove(p);
    SDFs.rmdir("/apps/metronome");
    if (!SDFs.exists("/apps/hello"))
        seedApp("/apps/hello", "/apps/hello/main.lua", HELLO_SCRIPT);
}
