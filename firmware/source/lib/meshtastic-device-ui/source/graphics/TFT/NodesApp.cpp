// -----------------------------------------------------------------------------
// T-UI Nodes & Favorites — two launcher apps over one list.
//
// Jake, 2026-09-10: "can we make a favorites app as well? which means we need a
// way to favorite them .. which means we need a separate node list app. trying to
// keep the meshtastic app modified the least possible so that updates are easier
// in the future."
//
// That constraint shapes the whole design. Neither app touches MUI's own node
// panels. They are ordinary launcher screens built from scratch, fed by the
// extern "C" bridge in src/TDeckNodesBridge.cpp — the same arrangement the GPS
// readout already uses, because this library cannot include firmware headers. A
// future device-ui update merges without ever meeting this code.
//
// ONE LIST, TWO DOORS. Nodes shows everything and lets you tap the star;
// Favorites shows the same rows filtered to the starred ones. The bridge does the
// ordering (favourites first, then most-recently-heard), so the two screens cannot
// drift apart — Favorites is literally the same list with a filter.
//
// The star is Meshtastic's own favourite bit, not a file of ours, so a node
// starred here is starred in the phone app too and survives a reboot.
// -----------------------------------------------------------------------------
#include "graphics/view/TFT/TuiStatusBar.h" // the persistent top bar
#include "graphics/view/TFT/TuiLabel.h" // tui_one_line: LONG_DOT needs a height
#include "lvgl.h"
#include <Arduino.h>
#include <cmath>
#include <cstdio>

extern "C" void nodes_open(void);
extern "C" void favorites_open(void);
extern "C" int tdeck_notes_favorite_count(void);
extern "C" unsigned tdeck_notes_favorite_id(int index);
extern "C" const char *tdeck_note_title(unsigned id);
extern "C" void notes_open_id(unsigned id);

// --- firmware bridge (src/TDeckNodesBridge.cpp, src/TDeckGpsBridge.cpp) ---
extern "C" int tdeck_nodes_list(uint32_t *out, int maxN);
extern "C" const char *tdeck_node_name(uint32_t num);
extern "C" uint32_t tdeck_node_age_secs(uint32_t num);
extern "C" int tdeck_node_battery(uint32_t num);
extern "C" bool tdeck_node_position(uint32_t num, int32_t *latI, int32_t *lonI);
extern "C" bool tdeck_node_is_favorite(uint32_t num);
extern "C" void tdeck_node_set_favorite(uint32_t num, bool on);
extern "C" bool tdeck_gps_position(int32_t *lat, int32_t *lon);

// --- MUI shims (TFTView_320x240.cpp) ---
extern "C" void tui_open_chat_with(uint32_t nodeNum);
extern "C" void tui_show_node_on_map(uint32_t nodeNum, int32_t latI, int32_t lonI);
// Notification centre: has this node sent something that has not been read yet?
extern "C" bool notif_unread_from(uint32_t nodeNum);
extern "C" void tui_trace_route(uint32_t nodeNum);
extern "C" void tui_request_position(uint32_t nodeNum);

namespace
{
// A busy mesh holds hundreds of nodes. Building one row widget per node is exactly
// what crash-looped the Max's first admin app, so this is capped rather than left
// to grow with the mesh.
const int kMaxRows = 60;

lv_obj_t *screen = nullptr;
lv_obj_t *listCont = nullptr;
lv_obj_t *titleLbl = nullptr;
lv_obj_t *emptyLbl = nullptr;
lv_obj_t *prevScreen = nullptr;
bool favOnly = false;
uint32_t rowNode[kMaxRows];

// "2m" / "3h" / "4d" — how long since we last heard from them.
void ageText(uint32_t secs, char *out, size_t n)
{
    if (!secs) {
        out[0] = 0;
        return;
    }
    if (secs < 3600)
        snprintf(out, n, "%um", (unsigned)(secs / 60));
    else if (secs < 86400)
        snprintf(out, n, "%uh", (unsigned)(secs / 3600));
    else
        snprintf(out, n, "%ud", (unsigned)(secs / 86400));
}

// "1.2 km NE" — empty when either end has no position.
void distText(uint32_t num, char *out, size_t n)
{
    out[0] = 0;
    int32_t ourLat, ourLon, theirLat, theirLon;
    if (!tdeck_gps_position(&ourLat, &ourLon))
        return;
    if (!tdeck_node_position(num, &theirLat, &theirLon))
        return;
    const double a = ourLat * 1e-7 * M_PI / 180.0, b = ourLon * 1e-7 * M_PI / 180.0;
    const double c = theirLat * 1e-7 * M_PI / 180.0, d = theirLon * 1e-7 * M_PI / 180.0;
    const double dLat = c - a, dLon = d - b;
    const double h = sin(dLat / 2) * sin(dLat / 2) + cos(a) * cos(c) * sin(dLon / 2) * sin(dLon / 2);
    const double m = 6371000.0 * 2 * atan2(sqrt(h), sqrt(1 - h));
    const double y = sin(dLon) * cos(c), x = cos(a) * sin(c) - sin(a) * cos(c) * cos(dLon);
    double brg = atan2(y, x) * 180.0 / M_PI;
    if (brg < 0)
        brg += 360.0;
    static const char *kPts[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const char *pt = kPts[(int)((brg + 22.5) / 45.0) % 8];
    if (m < 1000)
        snprintf(out, n, "%d m %s", (int)m, pt);
    else
        snprintf(out, n, "%.1f km %s", m / 1000.0, pt);
}

void rebuild(void);
lv_obj_t *starIcon(lv_obj_t *p, int size, uint32_t color, int w);

// Tapping the star toggles it, then the list is REBUILT rather than patched: on
// Favorites an un-starred row has to leave, and on Nodes a starred one re-sorts to
// the top. Both fall out of a rebuild for free.
// ⚠️ EVERY handler defers its work with lv_async_call, and the reason is not tidiness.
//
// Jake: "froze the device when i clicked it. didnt force a reboot, just froze."
//
// onStar() used to toggle the flag and call rebuild() directly. rebuild() starts with
// lv_obj_clean(listCont) — which DELETES THE VERY BUTTON THAT IS DISPATCHING THIS EVENT. LVGL then
// carries on walking freed memory after the callback returns: a use-after-free, which hangs exactly
// the way Jake describes rather than panicking. Screen loads (chat/map) are deferred for the same
// reason: they tear down the screen these widgets live on.
//
// lv_async_call runs the work from the LVGL loop once event dispatch has finished and nothing is
// mid-walk. TFTView_320x240.cpp already uses this same pattern after a launcher tap (rebuildAppGrid).
void onStar(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= kMaxRows)
        return;
    lv_async_call(
        [](void *p) {
            const int i = (int)(intptr_t)p;
            if (i < 0 || i >= kMaxRows)
                return;
            const uint32_t num = rowNode[i];
            tdeck_node_set_favorite(num, !tdeck_node_is_favorite(num));
            rebuild();
        },
        (void *)(intptr_t)idx);
}

// A five-pointed star. lv_line does not copy its points, so the shape is built once into a static
// array in the line's own coordinate space and every star just positions its object.
lv_obj_t *starIcon(lv_obj_t *p, int size, uint32_t color, int w)
{
    static lv_point_precise_t pts[11];
    static int builtFor = -1;
    if (builtFor != size) {
        builtFor = size;
        const double R = size / 2.0, r = R * 0.42, c = R;
        for (int i = 0; i < 10; i++) {
            const double ang = (-90.0 + i * 36.0) * M_PI / 180.0;
            const double rad = (i % 2 == 0) ? R : r;
            pts[i].x = (lv_value_precise_t)(c + rad * cos(ang));
            pts[i].y = (lv_value_precise_t)(c + rad * sin(ang));
        }
        pts[10] = pts[0];
    }
    lv_obj_t *l = lv_line_create(p);
    lv_line_set_points(l, pts, 11);
    lv_obj_set_style_line_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_line_width(l, w, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(l, true, LV_PART_MAIN);
    return l;
}

// ⚠️ DRAWN, NOT FONT GLYPHS. LV_SYMBOL_ENVELOPE / LV_SYMBOL_GPS rendered as EMPTY BOXES on the
// device — this build's label font does not carry the symbol range, and a missing glyph draws as
// nothing at all rather than as an error. Shapes always render, so the icons are primitives like the
// star. Same static-points rule as starIcon(): lv_line does not copy its array, but the geometry is
// identical for every row and the points are relative to each line object.
void envelopeIcon(lv_obj_t *p)
{
    lv_obj_t *body = lv_obj_create(p);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 26, 18);
    lv_obj_center(body);
    lv_obj_set_style_border_color(body, lv_color_hex(0x0a84ff), LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(body, 3, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    static lv_point_precise_t flap[3] = {{2, 2}, {13, 11}, {24, 2}}; // the V of the envelope
    lv_obj_t *l = lv_line_create(body);
    lv_line_set_points(l, flap, 3);
    lv_obj_set_style_line_color(l, lv_color_hex(0x0a84ff), LV_PART_MAIN);
    lv_obj_set_style_line_width(l, 2, LV_PART_MAIN);
}

void pinIcon(lv_obj_t *p, uint32_t colour)
{
    lv_obj_t *head = lv_obj_create(p);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, 16, 16);
    lv_obj_align(head, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_color(head, lv_color_hex(colour), LV_PART_MAIN);
    lv_obj_set_style_border_width(head, 3, LV_PART_MAIN);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);

    static lv_point_precise_t tail[3] = {{0, 0}, {5, 8}, {10, 0}}; // the point of the pin
    lv_obj_t *l = lv_line_create(p);
    lv_line_set_points(l, tail, 3);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_line_color(l, lv_color_hex(colour), LV_PART_MAIN);
    lv_obj_set_style_line_width(l, 3, LV_PART_MAIN);
}

void onChat(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= kMaxRows)
        return;
    lv_async_call(
        [](void *p) {
            const int k = (int)(intptr_t)p;
            if (k >= 0 && k < kMaxRows)
                tui_open_chat_with(rowNode[k]);
        },
        (void *)(intptr_t)i);
}

void onMap(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= kMaxRows)
        return;
    lv_async_call(
        [](void *p) {
            const int k = (int)(intptr_t)p;
            if (k < 0 || k >= kMaxRows)
                return;
            int32_t la = 0, lo = 0;
            if (tdeck_node_position(rowNode[k], &la, &lo))
                tui_show_node_on_map(rowNode[k], la, lo);
        },
        (void *)(intptr_t)i);
}

// -----------------------------------------------------------------------------
// The "more" sheet. Jake, 2026-09-18: "request locaton trace route etc on node in fav and
// list (right arrow on each one?) opens sub menu with messagw, map, trace route etc."
//
// A panel on lv_layer_top rather than a new screen: it is a menu about the row you are
// looking at, and swapping the whole screen out and back loses your place in the list.
// Everything it does is deferred with lv_async_call - several of these actions load a
// different screen, which would tear down the button still dispatching the event.
// -----------------------------------------------------------------------------
lv_obj_t *sheet = nullptr;
lv_obj_t *sheetTitle = nullptr;
uint32_t sheetNode = 0;

void closeSheet(void)
{
    if (sheet)
        lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *sheetBtn(lv_obj_t *parent, const char *txt, int y, uint32_t colour, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 244, 34);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(colour), LV_PART_MAIN);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

void buildSheet(void)
{
    sheet = lv_obj_create(lv_layer_top());
    lv_obj_set_size(sheet, 268, 228);
    lv_obj_center(sheet);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_border_color(sheet, lv_color_hex(0x48484a), LV_PART_MAIN);
    lv_obj_set_style_border_width(sheet, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(sheet, 12, LV_PART_MAIN);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);

    sheetTitle = lv_label_create(sheet);
    lv_obj_set_style_text_color(sheetTitle, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_width(sheetTitle, 236);
    lv_label_set_long_mode(sheetTitle, LV_LABEL_LONG_DOT);
    tui_one_line(sheetTitle); // LONG_DOT needs a pinned height - see TuiLabel.h
    lv_obj_align(sheetTitle, LV_ALIGN_TOP_MID, 0, 2);

    sheetBtn(sheet, "Message", 26, 0x0a84ff, [](lv_event_t *) {
        const uint32_t n = sheetNode;
        closeSheet();
        lv_async_call([](void *p) { tui_open_chat_with((uint32_t)(uintptr_t)p); }, (void *)(uintptr_t)n);
    });

    sheetBtn(sheet, "Show on map", 64, 0x5ac8fa, [](lv_event_t *) {
        const uint32_t n = sheetNode;
        closeSheet();
        lv_async_call(
            [](void *p) {
                const uint32_t num = (uint32_t)(uintptr_t)p;
                int32_t la = 0, lo = 0;
                if (tdeck_node_position(num, &la, &lo))
                    tui_show_node_on_map(num, la, lo);
            },
            (void *)(uintptr_t)n);
    });

    sheetBtn(sheet, "Trace route", 102, 0xbf5af2, [](lv_event_t *) {
        const uint32_t n = sheetNode;
        closeSheet();
        lv_async_call([](void *p) { tui_trace_route((uint32_t)(uintptr_t)p); }, (void *)(uintptr_t)n);
    });

    // This one sends a request and stays put: the answer comes back over the mesh whenever
    // that node feels like replying, and lands on its row and the map by itself.
    sheetBtn(sheet, "Request location", 140, 0x30d158, [](lv_event_t *) {
        const uint32_t n = sheetNode;
        tui_request_position(n);
        if (sheetTitle)
            lv_label_set_text(sheetTitle, "Anfrage gesendet – die Antwort erscheint von selbst");
    });

    sheetBtn(sheet, "Close", 186, 0x48484a, [](lv_event_t *) { closeSheet(); });
}

void onMore(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= kMaxRows)
        return;
    if (!sheet)
        buildSheet();
    sheetNode = rowNode[i];
    lv_label_set_text(sheetTitle, tdeck_node_name(sheetNode));
    lv_obj_move_foreground(sheet);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_HIDDEN);
}

// One action button: a flat tile with either a symbol glyph or a drawn star.
lv_obj_t *actionBtn(lv_obj_t *parent, int x, int w, lv_event_cb_t cb, int idx)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, 38);
    lv_obj_set_pos(b, x, 46);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb)
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    return b;
}

void addRow(lv_obj_t *parent, uint32_t num, int idx)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    // Twice the old height (Jake): the top half names the node, the bottom half carries three
    // full-width action buttons — far better touch targets than icons squeezed onto one line.
    lv_obj_set_size(row, lv_pct(100), 88);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Jake, 2026-09-18: "fav app needs to show who sent somthing if unread". A green dot
    // against the name, and the row's detail line says so in words underneath - on a list
    // this dense a dot alone is easy to miss.
    const bool unread = notif_unread_from(num);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, tdeck_node_name(num));
    lv_obj_set_style_text_color(name, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    tui_one_line(name); // LONG_DOT needs a pinned height - see TuiLabel.h
    lv_obj_set_width(name, unread ? 276 : 290);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, unread ? 20 : 8, 3);

    if (unread) {
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 9, 9);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0x30d158), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 7, 8);
    }

    char age[12], dist[24], batt[12], detail[72];
    ageText(tdeck_node_age_secs(num), age, sizeof(age));
    distText(num, dist, sizeof(dist));
    const int b = tdeck_node_battery(num);
    if (b < 0)
        batt[0] = 0;
    else if (b > 100)
        snprintf(batt, sizeof(batt), "USB"); // Meshtastic sends >100 for "not on a battery"
    else
        snprintf(batt, sizeof(batt), "%d%%", b);
    // Jake: "some dont allow me to click the maps on them?" — the pin is disabled when a node has
    // never reported a position, which was correct but invisible. Say "no position" instead of
    // leaving a gap, so the dead button explains itself.
    //
    // Same for the battery: a dump of his device showed three of his four favourites have never sent
    // a battery level at all (batt=-1), so the percentage was not missing from the UI, it was missing
    // from the mesh. Showing "no battery" beats silently omitting it and looking broken.
    const char *distTxt = dist[0] ? dist : "no position";
    const char *battTxt = batt[0] ? batt : "no battery";
    snprintf(detail, sizeof(detail), "%s  ·  %s%s%s", distTxt, battTxt, age[0] ? "  ·  seen " : "", age);
    lv_obj_t *sub = lv_label_create(row);
    // An unread message is the most useful thing this line could be telling you, so it takes
    // the line over entirely and turns green rather than being appended to the end.
    lv_label_set_text(sub, unread ? "Neue Nachricht – zum Lesen auf den Umschlag tippen" : detail);
    lv_obj_set_style_text_color(sub, lv_color_hex(unread ? 0x30d158 : 0x8e8e93), LV_PART_MAIN);
    // TOP-aligned, not BOTTOM: bottom-aligning put this line straight under the buttons, which is
    // the text Jake saw being covered.
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 8, 24);

    // Four actions along the bottom: message, show on map, favourite, and more. The first
    // three keep wide targets because they are the everyday ones; "more" is a narrow chevron.
    const int bw = 82;
    envelopeIcon(actionBtn(row, 4, bw, onChat, idx));

    // The map button is only live for a node that has actually reported a position — a dead button
    // is clearer than one that looks alive and does nothing.
    int32_t la = 0, lo = 0;
    const bool hasPos = tdeck_node_position(num, &la, &lo);
    pinIcon(actionBtn(row, 8 + bw, bw, hasPos ? onMap : nullptr, idx), hasPos ? 0x5ac8fa : 0x48484a);

    // Star: yellow and thick when favourited, a thin grey outline when not.
    lv_obj_t *fav = actionBtn(row, 12 + 2 * bw, bw, onStar, idx);
    const bool on = tdeck_node_is_favorite(num);
    lv_obj_t *st = starIcon(fav, 24, on ? 0xffd60a : 0x6a6a70, on ? 4 : 2);
    lv_obj_center(st);

    // The chevron Jake asked for: "right arrow on each one?". Drawn from two lines rather
    // than a glyph - this build's label font has no verified symbol coverage, and a blank
    // button is worse than no button.
    lv_obj_t *more = actionBtn(row, 16 + 3 * bw, 44, onMore, idx);
    {
        static lv_point_precise_t up[] = {{0, 0}, {7, 7}};
        static lv_point_precise_t dn[] = {{7, 7}, {0, 14}};
        for (lv_point_precise_t *pts : {up, dn}) {
            lv_obj_t *l = lv_line_create(more);
            lv_line_set_points(l, pts, 2);
            lv_obj_set_style_line_color(l, lv_color_hex(0x8e8e93), LV_PART_MAIN);
            lv_obj_set_style_line_width(l, 3, LV_PART_MAIN);
            lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        }
    }
}

void addNoteRow(lv_obj_t *parent, unsigned id)
{
    lv_obj_t *row = lv_btn_create(parent);
    lv_obj_set_size(row, lv_pct(100), 40);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(
        row,
        [](lv_event_t *e) {
            const unsigned id = (unsigned)(uintptr_t)lv_event_get_user_data(e);
            lv_async_call([](void *p) { notes_open_id((unsigned)(uintptr_t)p); }, (void *)(uintptr_t)id);
        },
        LV_EVENT_CLICKED, (void *)(uintptr_t)id);
    lv_obj_t *label = lv_label_create(row);
    char text[64];
    snprintf(text, sizeof(text), "Notiz  %s", tdeck_note_title(id));
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    tui_one_line(label);
    lv_obj_set_width(label, 286);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffd60a), LV_PART_MAIN);
}

void rebuild(void)
{
    // Jake: "when favoriting a node, it scrolls you back to the top (can it bring that favorite to
    // the top, but keep your location on scrolling?)"
    //
    // lv_obj_clean throws the scroll away with the children. Remember where he was and put it back.
    // The starred row really does move to the top — that is the sort doing its job — so everything
    // below it shifts by one row; holding the PIXEL offset keeps the view within a row of where it
    // was instead of jumping to the start of a long list.
    const int32_t keepScroll = lv_obj_get_scroll_y(listCont);
    lv_obj_clean(listCont);
    static uint32_t all[kMaxRows];
    const int n = tdeck_nodes_list(all, kMaxRows);
    int shown = 0;
    for (int i = 0; i < n && shown < kMaxRows; i++) {
        if (favOnly && !tdeck_node_is_favorite(all[i]))
            continue;
        rowNode[shown] = all[i];
        addRow(listCont, all[i], shown);
        shown++;
    }
    if (favOnly) {
        const int notes = tdeck_notes_favorite_count();
        for (int i = 0; i < notes && shown < kMaxRows; i++) {
            const unsigned id = tdeck_notes_favorite_id(i);
            if (!id)
                continue;
            addNoteRow(listCont, id);
            shown++;
        }
    }
    lv_label_set_text(titleLbl, favOnly ? "Favoriten" : "Knoten");
    // After the rows exist, so LVGL knows how far it is allowed to scroll.
    lv_obj_update_layout(listCont);
    if (keepScroll > 0)
        lv_obj_scroll_to_y(listCont, keepScroll, LV_ANIM_OFF);
    if (shown == 0) {
        lv_label_set_text(emptyLbl, favOnly ? "Noch keine Favoriten." : "Noch keine Knoten empfangen.");
        lv_obj_clear_flag(emptyLbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(emptyLbl, LV_OBJ_FLAG_HIDDEN);
    }
}

void buildScreen(void)
{
    screen = lv_obj_create(NULL);
    tui_statusbar_reserve(screen); // note item A4: the persistent top bar
    // The "more" sheet lives on lv_layer_top, which belongs to the display and not to this
    // screen - so left open it would float over the launcher and everything else. Leaving
    // the app closes it.
    lv_obj_add_event_cb(
        screen, [](lv_event_t *) { closeSheet(); }, LV_EVENT_SCREEN_UNLOADED, nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    titleLbl = lv_label_create(screen);
    lv_label_set_text(titleLbl, "Knoten");
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0xffd60a), LV_PART_MAIN);
    lv_obj_align(titleLbl, LV_ALIGN_TOP_MID, 0, 9);

    listCont = lv_obj_create(screen);
    lv_obj_remove_style_all(listCont);
    lv_obj_set_size(listCont, 312, 178); // -20: the status bar took 20px off the top (tui_statusbar_reserve)
    lv_obj_align(listCont, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_flex_flow(listCont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(listCont, 4, LV_PART_MAIN);
    lv_obj_set_scroll_dir(listCont, LV_DIR_VER);

    emptyLbl = lv_label_create(screen);
    lv_label_set_text(emptyLbl, "");
    lv_obj_set_style_text_color(emptyLbl, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(emptyLbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(emptyLbl, LV_OBJ_FLAG_HIDDEN);
}

void openWith(bool favourites)
{
    if (!screen)
        buildScreen();
    lv_obj_t *active = lv_screen_active();
    if (active != screen)
        prevScreen = active; // remember the launcher, never our own screen
    favOnly = favourites;
    rebuild();
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}
} // namespace

extern "C" void nodes_open(void)
{
    openWith(false);
}

extern "C" void favorites_open(void)
{
    openWith(true);
}
