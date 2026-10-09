// -----------------------------------------------------------------------------
// T-UI Conversations — the people you are talking to, and the channels you listen to.
//
// Started life as Channels. Jake, 2026-09-18: "channeks app needs to be renamed, include
// covorsations too" / "should show all current conversations, not just channels". So the
// list is now People first (anyone this device has exchanged a DIRECT message with, newest
// first, with the last line of the conversation) and then Channels underneath, unchanged.
//
// Jake, 2026-09-10: "another cool app would be channels: So for me personally, it
// would show, long fast, and Howe group. on the custom channels (not long fast),
// there would be an icon ... that pulls up a simple list of whos in that custom
// channel."
//
// ⚠️ ONE HONEST LIMITATION, STATED ON THE SCREEN ITSELF. Meshtastic does not track
// channel membership — a node never advertises which channels it is on, so "who is
// in this channel" is not a question the mesh can answer. What the device DOES know
// is the channel it last heard each node's NodeInfo on, which NodeDB records
// precisely so it knows how to reach them. So the member list is headed "heard here"
// rather than pretending to be a roster, and a node that talks on two channels shows
// under whichever it was last heard on.
//
// Same arrangement as NodesApp: its own launcher screen, fed by extern "C" bridges,
// touching none of MUI's internals.
// -----------------------------------------------------------------------------
#include "graphics/view/TFT/TuiStatusBar.h" // the persistent top bar
#include "graphics/view/TFT/TuiLabel.h" // tui_one_line: LONG_DOT needs a height
#include "graphics/view/TFT/UnreadCounts.h" // how many are waiting in each conversation
#include "lvgl.h"
#include "util/ILog.h"
#include <Arduino.h>
#include <cstdio>

extern "C" void channels_open(void);
extern "C" void channels_unread_changed(void);

// --- firmware bridge (src/TDeckNodesBridge.cpp) ---
extern "C" int tdeck_channel_count(void);
extern "C" const char *tdeck_channel_name(int idx);
extern "C" int tdeck_channel_role(int idx);
extern "C" int tdeck_channel_precision(int idx);
extern "C" int tdeck_channel_nodes(int chIdx, uint32_t *out, int maxN);
extern "C" const char *tdeck_node_name(uint32_t num);
extern "C" uint32_t tdeck_node_age_secs(uint32_t num);

// --- MUI shim (TFTView_320x240.cpp) ---
extern "C" void tui_open_channel_chat(uint8_t ch);
extern "C" void tui_open_chat_with(uint32_t nodeNum);
// The conversation list comes from MUI's own chat panel, NOT from MessageStore - that only
// holds 20 messages in total and a busy channel evicts every DM within minutes.
extern "C" int tui_dm_conversations(uint32_t *out, int maxN);
extern "C" bool tui_dm_last_text(uint32_t nodeNum, char *out, int outN);
extern "C" bool notif_unread_from(uint32_t nodeNum);
extern "C" void emoji_to_text(const char *in, char *out, size_t outN);

namespace
{
const int kMaxMembers = 60;

lv_obj_t *screen = nullptr;
lv_obj_t *listCont = nullptr;
lv_obj_t *titleLbl = nullptr;
lv_obj_t *noteLbl = nullptr;
lv_obj_t *prevScreen = nullptr;
int viewChannel = -1; // -1 = the main list; otherwise the members of that channel
uint32_t members[kMaxMembers];

// Jake, 2026-09-18: "channeks app needs to be renamed, include covorsations too" and
// "should show all current conversations, not just channels". So the app is Conversations
// now, and the list is both: the channels this device listens to, and every person it has
// actually exchanged a direct message with.
const int kMaxConvos = 24;
uint32_t convos[kMaxConvos];


// ---------------------------------------------------------------------------
// The row. Jake, 2026-09-19: "make it look as professional and polished as possible...
// welcome to change anything, even major stuff."
//
// The old row put a name, a preview and a timestamp all hard against the left edge of a
// 56px box, which read as a wall of text - and it had an overlapping-label artefact that
// I could never account for. This is the arrangement every phone messenger uses, because
// it works: a round avatar anchors the row and gives the eye a column to scan, name and
// time share the top line, the preview owns the second, and unread is a colour change
// rather than another thing to read. Designed and checked in the PC renderer (C:\tdsim)
// before it ever reached the device.
//
// Every label here is given its width BEFORE it is aligned - setting a width moves a
// label, so aligning first and sizing second silently puts it somewhere else.
// ---------------------------------------------------------------------------
uint32_t avatarColour(uint32_t num)
{
    // Green is deliberately absent: it is the unread accent, and an avatar the same
    // colour as "you have a new message" is a lie the eye has to unlearn every time.
    static const uint32_t kPal[10] = {0x0a84ff, 0xbf5af2, 0xff9f0a, 0x5ac8fa, 0xff6482,
                                      0x64d2ff, 0xffd60a, 0xff8a5b, 0xa78bfa, 0xf472b6};
    // A real mix, not an xor-fold: node numbers differ mostly in their low bits, and
    // folding them onto each other put two of Jake's nodes on the same colour.
    uint32_t h = num * 2654435761u; // Knuth
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return kPal[h % 10];
}

// Up to two initials: "Vip Phone-Nick2" -> "VP", "Nick" -> "N".
void initialsOf(const char *name, char *out, size_t n)
{
    size_t o = 0;
    bool atWordStart = true;
    for (const char *p = name; *p && o + 1 < n && o < 2; p++) {
        if (*p == ' ' || *p == '-' || *p == '_') {
            atWordStart = true;
            continue;
        }
        const bool alnum = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
        if (atWordStart && alnum) {
            out[o++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
            atWordStart = false;
        }
    }
    if (o == 0 && name && name[0])
        out[o++] = name[0];
    out[o] = 0;
}

lv_obj_t *convoRow(uint32_t num, const char *name, const char *preview, const char *when, uint32_t unread,
                   bool isChannel)
{
    lv_obj_t *row = lv_obj_create(listCont);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 304, 60);
    lv_obj_set_style_bg_color(row, lv_color_hex(unread ? 0x14241a : 0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *av = lv_obj_create(row);
    lv_obj_remove_style_all(av);
    lv_obj_set_size(av, 40, 40);
    lv_obj_align(av, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_radius(av, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(av, lv_color_hex(isChannel ? 0x2c2c2e : avatarColour(num)), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(av, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(av, LV_OBJ_FLAG_SCROLLABLE);
    if (isChannel) {
        // A channel is a place, not a person: bars instead of initials.
        lv_obj_set_style_border_color(av, lv_color_hex(0x30d158), LV_PART_MAIN);
        lv_obj_set_style_border_width(av, 2, LV_PART_MAIN);
        for (int k = 0; k < 3; k++) {
            lv_obj_t *b = lv_obj_create(av);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, 18, 3);
            lv_obj_align(b, LV_ALIGN_CENTER, 0, -6 + k * 6);
            lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
            lv_obj_set_style_bg_color(b, lv_color_hex(k == 0 ? 0x30d158 : 0x6a6a70), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        }
    } else {
        char ini[4];
        initialsOf(name, ini, sizeof(ini));
        lv_obj_t *l = lv_label_create(av);
        lv_label_set_text(l, ini);
        lv_obj_set_style_text_color(l, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_center(l);
    }

    lv_obj_t *tm = lv_label_create(row);
    lv_label_set_text(tm, (when && when[0]) ? when : "");
    lv_obj_set_style_text_color(tm, lv_color_hex(unread ? 0x30d158 : 0x8e8e93), LV_PART_MAIN);
    lv_obj_set_style_text_font(tm, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(tm, LV_ALIGN_TOP_RIGHT, -12, 11);

    lv_obj_t *nm = lv_label_create(row);
    lv_obj_set_width(nm, 168);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    tui_one_line(nm); // LONG_DOT needs a pinned height - see TuiLabel.h
    lv_label_set_text(nm, (name && name[0]) ? name : "(unnamed)");
    lv_obj_set_style_text_color(nm, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 60, 9);

    lv_obj_t *pv = lv_label_create(row);
    lv_obj_set_width(pv, 226);
    lv_label_set_long_mode(pv, LV_LABEL_LONG_DOT);
    lv_label_set_text(pv, (preview && preview[0]) ? preview : "No messages yet");
    lv_obj_set_style_text_color(pv, lv_color_hex(unread ? 0xc7f5d2 : 0x8e8e93), LV_PART_MAIN);
    lv_obj_set_style_text_font(pv, &ui_font_montserrat_12, LV_PART_MAIN);
    tui_one_line(pv); // LONG_DOT needs a pinned height - see TuiLabel.h
    lv_obj_align(pv, LV_ALIGN_TOP_LEFT, 60, 32);

    if (unread) {
        // A COUNT, not just a dot. The whole point of this change is that "3" in one row and
        // "1" in another is the thing a single device-wide number could never tell you.
        lv_obj_t *badge = lv_obj_create(row);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, unread > 9 ? 24 : 18, 18);
        lv_obj_align(badge, LV_ALIGN_BOTTOM_RIGHT, -12, -8);
        lv_obj_set_style_radius(badge, 9, LV_PART_MAIN);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x30d158), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *n = lv_label_create(badge);
        char nb[8];
        snprintf(nb, sizeof(nb), "%u", (unsigned)(unread > 99 ? 99 : unread));
        lv_label_set_text(n, nb);
        lv_obj_set_style_text_font(n, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(n, lv_color_hex(0x04210d), LV_PART_MAIN);
        lv_obj_center(n);
    }
    return row;
}

lv_obj_t *sectionHeader(const char *txt)
{
    lv_obj_t *l = lv_label_create(listCont);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(0x6a6a70), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_pad_left(l, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_top(l, 4, LV_PART_MAIN);
    return l;
}

void rebuild(void);

void ageText(uint32_t secs, char *out, size_t n)
{
    if (!secs) {
        out[0] = 0;
        return;
    }
    if (secs < 3600)
        snprintf(out, n, "seen %um", (unsigned)(secs / 60));
    else if (secs < 86400)
        snprintf(out, n, "seen %uh", (unsigned)(secs / 3600));
    else
        snprintf(out, n, "seen %ud", (unsigned)(secs / 86400));
}

lv_obj_t *makeRow(int h)
{
    lv_obj_t *row = lv_obj_create(listCont);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), h);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t *makeLabel(lv_obj_t *p, const char *txt, uint32_t colour, lv_align_t al, int dx, int dy)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), LV_PART_MAIN);
    lv_obj_align(l, al, dx, dy);
    return l;
}

// Two small torso silhouettes — the same shape as the Nodes tile, shrunk to a button.
void peopleIcon(lv_obj_t *p)
{
    auto box = [&](int x, int y, int w, int h, uint32_t c, int r) {
        lv_obj_t *o = lv_obj_create(p);
        lv_obj_remove_style_all(o);
        lv_obj_set_pos(o, x, y);
        lv_obj_set_size(o, w, h);
        lv_obj_set_style_bg_color(o, lv_color_hex(c), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(o, r, LV_PART_MAIN);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    };
    box(6, 14, 10, 8, 0x6a6a70, 4);  // back shoulders
    box(8, 6, 7, 7, 0x6a6a70, 4);    // back head
    box(14, 15, 13, 9, 0x5ac8fa, 5); // front shoulders
    box(17, 5, 8, 8, 0x5ac8fa, 4);   // front head
}

// Jake: "in Channels ... can clicking the channel bring you to that conversation?"
// Deferred like every other handler here: this loads a different screen, which would tear down the
// widgets still dispatching the event.
void onOpenChat(lv_event_t *e)
{
    const int ch = (int)(intptr_t)lv_event_get_user_data(e);
    lv_async_call([](void *p) { tui_open_channel_chat((uint8_t)(intptr_t)p); }, (void *)(intptr_t)ch);
}

void onOpenDm(lv_event_t *e)
{
    const uint32_t num = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_async_call([](void *p) { tui_open_chat_with((uint32_t)(uintptr_t)p); }, (void *)(uintptr_t)num);
}

void onOpenMembers(lv_event_t *e)
{
    viewChannel = (int)(intptr_t)lv_event_get_user_data(e);
    rebuild();
}

void onBackToChannels(lv_event_t *)
{
    viewChannel = -1;
    rebuild();
}

void buildChannelList(void)
{
    lv_label_set_text(titleLbl, "Conversations");
    int shown = 0;

    // People first: that is what this app is opened for.
    const int nConvo = tui_dm_conversations(convos, kMaxConvos);
    if (nConvo > 0)
        sectionHeader("PEOPLE");
    for (int i = 0; i < nConvo; i++) {
        shown++;
        char last[80], preview[80];
        preview[0] = 0;
        if (tui_dm_last_text(convos[i], last, sizeof(last)))
            emoji_to_text(last, preview, sizeof(preview)); // emoji have no glyph in this build

        char age[16];
        ageText(tdeck_node_age_secs(convos[i]), age, sizeof(age));
        // ageText gives "seen 4h"; the row has a dedicated time column, so drop the word.
        const char *when = age;
        if (!strncmp(age, "seen ", 5))
            when = age + 5;

        lv_obj_t *row = convoRow(convos[i], tdeck_node_name(convos[i]), preview, when,
                                 unread_get(convos[i]), false);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, onOpenDm, LV_EVENT_CLICKED, (void *)(uintptr_t)convos[i]);
    }

    if (nConvo > 0)
        sectionHeader("CHANNELS");
    for (int i = 0; i < tdeck_channel_count(); i++) {
        const int role = tdeck_channel_role(i);
        if (role == 0)
            continue; // disabled slots are not channels
        shown++;

        // The position precision is the interesting fact about a channel: it decides
        // whether the map can show people where they actually are.
        const int prec = tdeck_channel_precision(i);
        char sub[64];
        snprintf(sub, sizeof(sub), "%s  -  %s", role == 1 ? "Primary" : "Secondary",
                 prec >= 32 ? "exact positions" : (prec > 0 ? "approx positions" : "no positions"));

        // Channels get a count too now. They never had one: the old flag only ever knew about
        // direct messages, so a busy channel looked exactly as quiet as an empty one.
        lv_obj_t *row = convoRow((uint32_t)i, tdeck_channel_name(i), sub, "",
                                 unread_get(unreadKeyForChannel((uint8_t)i)), true);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, onOpenChat, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // People button on the custom channels only - the primary is everyone by
        // definition, so a list there says nothing. Sits between the time column and the
        // right edge, where nothing else lives on a channel row.
        if (role != 1) {
            lv_obj_t *btn = lv_obj_create(row);
            lv_obj_remove_style_all(btn);
            lv_obj_set_size(btn, 40, 40);
            lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -8, 0);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(btn, onOpenMembers, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            peopleIcon(btn);
        }
    }

    if (!shown)
        lv_label_set_text(noteLbl, "No conversations yet.\nChannels appear here once configured,\nand people once you have messaged.");
    else
        lv_label_set_text(noteLbl, "");
}

void buildMemberList(void)
{
    const char *nm = tdeck_channel_name(viewChannel);
    lv_label_set_text(titleLbl, (nm && nm[0]) ? nm : "Channel");

    lv_obj_t *back = makeRow(36);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, onBackToChannels, LV_EVENT_CLICKED, nullptr);
    makeLabel(back, LV_SYMBOL_LEFT "  All conversations", 0x0a84ff, LV_ALIGN_LEFT_MID, 10, 0);

    const int n = tdeck_channel_nodes(viewChannel, members, kMaxMembers);
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = makeRow(40);
        makeLabel(row, tdeck_node_name(members[i]), 0xffffff, LV_ALIGN_LEFT_MID, 10, 0);
        char age[16];
        ageText(tdeck_node_age_secs(members[i]), age, sizeof(age));
        if (age[0])
            makeLabel(row, age, 0x8e8e93, LV_ALIGN_RIGHT_MID, -10, 0);
    }
    // Said plainly, because the alternative is a list that quietly lies about what it is.
    if (n == 0)
        lv_label_set_text(noteLbl, "Nobody heard on this channel yet.\nNodes appear here once their\ninfo arrives over it.");
    else
        lv_label_set_text(noteLbl, "");
}

void rebuild(void)
{
    lv_obj_clean(listCont);
    if (viewChannel < 0)
        buildChannelList();
    else
        buildMemberList();
    if (lv_label_get_text(noteLbl) && lv_label_get_text(noteLbl)[0])
        lv_obj_clear_flag(noteLbl, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(noteLbl, LV_OBJ_FLAG_HIDDEN);
}

void buildScreen(void)
{
    screen = lv_obj_create(NULL);
    tui_statusbar_reserve(screen); // note item A4: the persistent top bar
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    titleLbl = lv_label_create(screen);
    lv_label_set_text(titleLbl, "Channels");
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0x30d158), LV_PART_MAIN);
    lv_obj_align(titleLbl, LV_ALIGN_TOP_MID, 0, 9);

    listCont = lv_obj_create(screen);
    lv_obj_remove_style_all(listCont);
    lv_obj_set_size(listCont, 320, 182); // -20: the status bar took 20px off the top (tui_statusbar_reserve)
    lv_obj_align(listCont, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_flex_flow(listCont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(listCont, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_left(listCont, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(listCont, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(listCont, LV_DIR_VER);

    noteLbl = lv_label_create(screen);
    lv_label_set_text(noteLbl, "");
    lv_obj_set_style_text_color(noteLbl, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_set_style_text_align(noteLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(noteLbl, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_flag(noteLbl, LV_OBJ_FLAG_HIDDEN);
}
} // namespace

// A message arrived (or was read) while this list is on screen. Redraw it, so the counts on
// the rows are true rather than whatever they were when you walked in.
//
// ⚠️ DEFERRED, and only from the top-level list. This is called straight from message
// handling, mid-refresh, with the SPI lock held - and rebuild() starts by destroying every
// row. lv_async_call runs it after the current handler has returned, which is the same rule
// NotificationCenter documents at length. Drilled into a channel's member list, it does
// nothing: yanking that out from under somebody is worse than a stale count.
extern "C" void channels_unread_changed(void)
{
    if (!screen || lv_screen_active() != screen || viewChannel >= 0)
        return;
    lv_async_call(
        [](void *) {
            if (screen && lv_screen_active() == screen && viewChannel < 0)
                rebuild();
        },
        nullptr);
}

extern "C" void channels_open(void)
{
    if (!screen)
        buildScreen();
    lv_obj_t *active = lv_screen_active();
    if (active != screen)
        prevScreen = active;
    viewChannel = -1; // always open on the channel list, never mid-drill-down
    rebuild();
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}
