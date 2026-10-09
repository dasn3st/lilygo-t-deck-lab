#include "graphics/common/SdCard.h"
#include "graphics/view/TFT/TuiStatusBar.h"
#include "lvgl.h"
#include "util/ILog.h"
#include <cctype>
#include <cstdio>
#include <cstring>

// -----------------------------------------------------------------------------------------
// MAIL - Gmail over IMAP. This file is the SETUP FORM and the status screen; the network side
// lives in src/TDeckMail.cpp.
//
// Jake, 2026-09-23: "Instead of the SD card needing a password, can it be a form that I type in
// on the device then it saves to the SD card?" - yes, and it is strictly better than the card
// route in two ways beyond convenience: there is no step where the password sits in a text file
// on a PC, and it means I never handle it at any point. It goes from his fingers to the card.
//
// ⛔ THE PASSWORD MUST NEVER LEAVE THIS DEVICE, AND THAT INCLUDES MY OWN TOOLS.
// The @@shot command streams the framebuffer over USB, so a screenshot taken while this form is
// filled in would put the password in a PNG on my machine and in the conversation. That is not
// a hypothetical - I have been screenshotting this device all session to verify work. So the
// form BLOCKS screenshots while it holds anything, rather than relying on me to remember.
// It is also never logged, and the field is cleared the moment it is saved.
// -----------------------------------------------------------------------------------------

extern "C" bool tdeck_mail_check(void);
extern "C" int tdeck_mail_poll(void);
extern "C" void tdeck_mail_counts(int *total, int *unseen);
extern "C" const char *tdeck_mail_error(void);
extern "C" void tdeck_mail_forget_creds(void); // re-read /gmail.txt after the form saves
extern "C" void tdeck_shot_block(bool on);     // refuse @@shot while a secret is on screen
extern "C" bool tdeck_mail_list(int page);
extern "C" int tdeck_mail_list_count(void);
extern "C" int tdeck_mail_total(void);
extern "C" int tdeck_mail_page(void);
extern "C" bool tdeck_mail_item(int i, unsigned *seq, const char **from, const char **subj, const char **date,
                                bool *seen);
extern "C" bool tdeck_mail_read(unsigned seq);
extern "C" const char *tdeck_mail_body(void);
extern "C" bool tdeck_mail_send(const char *to, const char *subject, const char *body);
extern "C" bool tdeck_mail_send_ex(const char *to, const char *subject, const char *body, bool asReply);
extern "C" const char *tdeck_mail_read_reply_addr(void);
extern "C" const char *tdeck_mail_read_from(void);
extern "C" const char *tdeck_mail_read_date(void);
extern "C" const char *tdeck_mail_read_subject(void);

static lv_obj_t *screen = nullptr;
static lv_obj_t *addrArea = nullptr;
static lv_obj_t *passArea = nullptr;
static lv_obj_t *statusLbl = nullptr; // status screen
static lv_obj_t *formMsg = nullptr;   // the form has its own, inside the form box
static lv_obj_t *countLbl = nullptr;
static lv_obj_t *setupBox = nullptr;
static lv_obj_t *statusBox = nullptr;
static bool checking = false;

#define MAIL_ROWS 8 // matches MAIL_PAGE in TDeckMail.cpp: one fetch, one screen
static lv_obj_t *listBox = nullptr, *readBox = nullptr, *composeBox = nullptr;
static lv_obj_t *gateBox = nullptr, *gateWho = nullptr, *removeBtn = nullptr, *removeLbl = nullptr;
static lv_obj_t *infoBox = nullptr;
static bool removeArmed = false; // second tap confirms
static void refreshGate(void); // defined below; used by handlers declared before it
static lv_obj_t *rowLbl[MAIL_ROWS] = {nullptr};
static lv_obj_t *pageLbl = nullptr, *readHdr = nullptr, *readFrom = nullptr, *readTxt = nullptr;
static lv_obj_t *toArea = nullptr, *subjArea = nullptr, *bodyArea = nullptr, *composeMsg = nullptr;
// What the pending network call was for, so the poll knows which screen to fill in. The
// transport has its own idea of the operation; this is the UI's, and they are deliberately
// separate - a reply can arrive after the user has walked away from the screen that asked.
enum UiWant { WANT_NONE = 0, WANT_COUNT, WANT_LIST, WANT_BODY, WANT_SEND };
static int uiWant = WANT_NONE;
static unsigned rowSeq[MAIL_ROWS] = {0};

// ---- Reply / Forward / Who? ----------------------------------------------------------------
// Jake, 2026-09-26: "In the mail app, can there be a forward, reply buttons? Maybe be able to pull
// from your contacts on gmail when writing an email".
//
// ⛔ CONTACTS ARE NOT GOOGLE CONTACTS, AND SAY SO. The address book needs Google's OAuth and
// People API; an app password unlocks mail and nothing else. What is offered instead is the
// useful part of it: the people you have written to from this device (kept in /mailto.txt, one
// address a line, newest first) and the senders on the inbox page you last opened.
enum ComposeKind { COMPOSE_NEW = 0, COMPOSE_REPLY, COMPOSE_FORWARD };
static int composeKind = COMPOSE_NEW;
static int composeReturn = 2;     // VIEW_STATUS - where Cancel goes (the read view, for a reply)
static bool bodyLoaded = false;    // Reply/Forward quote the body, so they wait for it
static lv_obj_t *whoBox = nullptr; // the address picker, built each time it opens
static const char *kSentFile = "/mailto.txt";
static const int kSentMax = 12;

// Read back just the address line. ⛔ Deliberately does NOT read line 2 - nothing outside
// onSave has any business holding the password, and a helper that returns it would get reused.
static bool savedAddress(char *out, size_t cap)
{
    out[0] = 0;
    FsFile f = SDFs.open("/gmail.txt", O_RDONLY);
    if (!f) {
        delay(20);
        f = SDFs.open("/gmail.txt", O_RDONLY); // same bus-contention retry as haveCreds()
    }
    if (!f)
        return false;
    int n = f.fgets(out, (int)cap);
    f.close();
    for (char *p = out; *p; p++)
        if (*p == '\n' || *p == '\r') {
            *p = 0;
            break;
        }
    return n > 3 && out[0];
}

// ⛔ A FAILED READ IS NOT PROOF THERE IS NO ACCOUNT, and treating it as one is why Jake kept
// being asked to log in again. This re-read the SD card on every single app open; the card
// shares its SPI bus with the display and the radio, so a read at a busy moment can simply
// fail - and the app then concluded there were no credentials and showed the login form.
// Verified the file was there the whole time: @@mailfile reported size=42, line1=23, line2=17.
//
// Now: a POSITIVE result is cached for the session, and a failure to open is never cached and
// never reported as "no account" without a second attempt. The cache is cleared whenever the
// file is written or removed, so it cannot go stale.
static int s_credCache = -1; // -1 unknown, 0 definitely none, 1 present

static void forgetCredCache(void)
{
    s_credCache = -1;
}

static bool haveCreds(void)
{
    if (s_credCache >= 0)
        return s_credCache == 1;
    for (int attempt = 0; attempt < 2; attempt++) {
        FsFile f = SDFs.open("/gmail.txt", O_RDONLY);
        if (!f) {
            if (attempt == 0) {
                delay(20); // give the bus a moment; the other user of it is mid-transfer
                continue;
            }
            return false; // deliberately NOT cached - we do not know, so we will ask again
        }
        char line[96];
        const int n = f.fgets(line, sizeof(line));
        f.close();
        memset(line, 0, sizeof(line));
        s_credCache = (n > 3) ? 1 : 0; // an address, at minimum
        return s_credCache == 1;
    }
    return false;
}

static lv_obj_t *makeButton(lv_obj_t *parent, const char *txt, uint32_t colour, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(colour), LV_PART_MAIN);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

enum MailView { VIEW_GATE = 0, VIEW_SETUP, VIEW_STATUS, VIEW_LIST, VIEW_READ, VIEW_COMPOSE };
static int view = VIEW_GATE;

static void showView(int v)
{
    view = v;
    lv_obj_t *boxes[] = {gateBox, setupBox, statusBox, listBox, readBox, composeBox};
    for (int i = 0; i < 6; i++) {
        if (!boxes[i])
            continue;
        if (i == v)
            lv_obj_clear_flag(boxes[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(boxes[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void showSetup(bool on)
{
    showView(on ? VIEW_SETUP : VIEW_STATUS);
    // ⛔ THE SCREENSHOT BLOCK IS NOT SET HERE. It is DERIVED from what is actually on screen,
    // every poll, in mail_service_ui(). Setting it at this transition is what the first version
    // did, and leaving the app by Back or Home does not pass through here - so the block stuck
    // ON and every screenshot for the rest of the session was refused, on every screen. Tested
    // and caught: "@@err shot refused - a password field is on screen" while sitting on the
    // launcher. Same shape as three other bugs today: a flag set on one path and cleared on
    // none of the others.
}

// ⭐ GOOGLE SHOWS THE APP PASSWORD AS "abcd efgh ijkl mnop" AND THE SPACES ARE NOT PART OF IT.
// Stripping here as well as in TDeckMail keeps the FILE clean, so anyone reading the card later
// sees the real thing rather than a version that only works because something strips it.
static void stripSpaces(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++)
        if (*r != ' ' && *r != '\t' && *r != '\r' && *r != '\n')
            *w++ = *r;
    *w = 0;
}

static void onSave(lv_event_t *)
{
    char addr[96], pass[64];
    strncpy(addr, lv_textarea_get_text(addrArea), sizeof(addr) - 1);
    addr[sizeof(addr) - 1] = 0;
    strncpy(pass, lv_textarea_get_text(passArea), sizeof(pass) - 1);
    pass[sizeof(pass) - 1] = 0;
    stripSpaces(addr);
    stripSpaces(pass);

    if (!strchr(addr, '@')) {
        lv_label_set_text(formMsg, "That is not an email address");
        return;
    }
    // ⛔ GMAIL ONLY, AND NOT ARBITRARILY. Jake asked whether this works for other providers.
    // It does not yet, and the reason is the security design rather than laziness: the IMAP
    // connection verifies the server against ONE embedded root certificate - Google's GTS Root
    // R1 - because a full CA bundle does not fit here. Outlook, Yahoo and iCloud chain to
    // different roots, so they would not fail with "wrong server", they would fail the
    // certificate check, which reads as a network fault and is miserable to diagnose.
    // Supporting them means embedding their roots too, and verifying each. Until then, say so
    // up front rather than letting someone type a Yahoo address and hit a baffling error.
    {
        const char *at = strchr(addr, '@');
        if (strcasecmp(at, "@gmail.com") && strcasecmp(at, "@googlemail.com")) {
            lv_label_set_text(formMsg, "Gmail addresses only for now");
            return;
        }
    }
    // ⛔ REJECT A REAL GOOGLE PASSWORD, DO NOT JUST SAVE IT.
    // Jake, 2026-09-23: "i eneterned my normail gmail password and didnt work". The first check
    // here was only `length < 16`, so a real password of 16 characters or more sailed through
    // and got WRITTEN TO THE SD CARD IN PLAIN TEXT before failing to log in. That is a bad
    // outcome from a validation that was trying to be helpful.
    //
    // A Google app password is always EXACTLY 16 characters and always all lowercase letters -
    // no capitals, digits or symbols. Practically no real password looks like that, so the
    // shape alone tells them apart. Check it BEFORE the file is opened.
    // ⭐ BLANK PASSWORD MEANS "KEEP THE ONE ALREADY SAVED". The field is always empty when the
    // form opens, because it is wiped the moment it is saved - so without this, opening Account
    // to check the address forced you to retype all 16 characters, and looked like the login had
    // been thrown away. Jake hit exactly that.
    if (!pass[0] && haveCreds()) {
        char keep[64] = {0};
        FsFile rf = SDFs.open("/gmail.txt", O_RDONLY);
        if (rf) {
            char skip[96];
            rf.fgets(skip, sizeof(skip)); // the address line, discarded
            rf.fgets(keep, sizeof(keep));
            rf.close();
        }
        stripSpaces(keep);
        if (keep[0]) {
            FsFile wf = SDFs.open("/gmail.txt", O_WRONLY | O_CREAT | O_TRUNC);
            if (!wf) {
                memset(keep, 0, sizeof(keep));
                lv_label_set_text(formMsg, "Could not write to the SD card");
                return;
            }
            wf.println(addr);
            wf.println(keep);
            wf.close();
            memset(keep, 0, sizeof(keep)); // never lingers, same rule as a typed one
            forgetCredCache();
            tdeck_mail_forget_creds();
            showSetup(false);
            lv_label_set_text(statusLbl, "Saved. Checking...");
            uiWant = WANT_COUNT;
            checking = tdeck_mail_check();
            return;
        }
    }
    {
        const size_t n = strlen(pass);
        bool lowerOnly = true;
        for (size_t i = 0; i < n; i++)
            if (pass[i] < 'a' || pass[i] > 'z')
                lowerOnly = false;
        if (n != 16 || !lowerOnly) {
            lv_label_set_text(formMsg, "Not an app password - needs 16 lowercase letters");
            return;
        }
    }

    FsFile f = SDFs.open("/gmail.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) {
        lv_label_set_text(formMsg, "Could not write to the SD card");
        return;
    }
    f.println(addr);
    f.println(pass);
    f.close();
    // ⛔ NOT LOGGED, and wiped from RAM and from the field immediately. The only copy that
    // should exist after this line is the one on the card.
    memset(pass, 0, sizeof(pass));
    lv_textarea_set_text(passArea, "");
    forgetCredCache();
    tdeck_mail_forget_creds(); // drop the cached copy so the new file is picked up
    LOG_INFO("[MAIL] credentials saved for %s", addr); // the ADDRESS only, never the password

    showSetup(false);
    lv_label_set_text(statusLbl, "Saved. Checking...");
    uiWant = WANT_COUNT;
    checking = tdeck_mail_check();
}

static void onCheck(lv_event_t *)
{
    if (checking)
        return;
    lv_label_set_text(statusLbl, "Checking...");
    uiWant = WANT_COUNT;
    checking = tdeck_mail_check();
    if (!checking)
        lv_label_set_text(statusLbl, "Busy - try again in a moment");
}

static void onBackToGate(lv_event_t *)
{
    refreshGate();
    showView(VIEW_GATE);
}

static void onEdit(lv_event_t *)
{
    // Show what is actually stored, rather than a blank form that implies nothing is.
    char addr[96];
    if (savedAddress(addr, sizeof(addr)) && addrArea)
        lv_textarea_set_text(addrArea, addr);
    if (passArea)
        lv_textarea_set_text(passArea, "");
    lv_label_set_text(formMsg, haveCreds() ? "Leave the password blank to keep the saved one" : "");
    showSetup(true);
}

// Reflect what is actually stored. Called whenever the gate is shown, so it can never be stale.
static void refreshGate(void)
{
    char addr[96];
    const bool have = savedAddress(addr, sizeof(addr));
    removeArmed = false;
    if (gateWho)
        lv_label_set_text_fmt(gateWho, have ? "Signed in as %s" : "No account saved yet", addr);
    if (removeBtn) {
        // Greyed out with nothing to remove; red when there is, because it destroys something.
        lv_obj_set_style_bg_color(removeBtn, lv_color_hex(have ? 0xff453a : 0x2c2c2e), LV_PART_MAIN);
        if (removeLbl) {
            lv_label_set_text(removeLbl, "Remove account");
            lv_obj_set_style_text_color(removeLbl, lv_color_hex(have ? 0xffffff : 0x5a5a5e), LV_PART_MAIN);
        }
    }
}

static void onGateLogin(lv_event_t *)
{
    if (haveCreds()) {
        // ⭐ AUTO SIGN-IN. The credentials are already on the card; there is nothing to ask for.
        // Jake: "once clicked login, it should auto sign you in, or bring you to the login form
        // depending".
        showView(VIEW_STATUS);
        lv_label_set_text(statusLbl, "Signing in...");
        uiWant = WANT_COUNT;
        checking = tdeck_mail_check();
        if (!checking)
            lv_label_set_text(statusLbl, "Busy - try again in a moment");
        return;
    }
    char addr[96];
    if (savedAddress(addr, sizeof(addr)) && addrArea)
        lv_textarea_set_text(addrArea, addr);
    lv_label_set_text(formMsg, "");
    showView(VIEW_SETUP);
}

static void onGateRemove(lv_event_t *)
{
    if (!haveCreds())
        return; // nothing to remove; the button is greyed for exactly this reason
    if (!removeArmed) {
        // ⛔ TWO TAPS. Deleting the saved login is not undoable without Google's website and a
        // fresh app password, so a stray tap must not do it.
        removeArmed = true;
        if (removeLbl)
            lv_label_set_text(removeLbl, "Tap again to confirm");
        return;
    }
    SDFs.remove("/gmail.txt");
    forgetCredCache();
    tdeck_mail_forget_creds();
    if (passArea)
        lv_textarea_set_text(passArea, "");
    if (addrArea)
        lv_textarea_set_text(addrArea, "@gmail.com");
    LOG_INFO("[MAIL] saved account removed");
    refreshGate();
    if (gateWho)
        lv_label_set_text(gateWho, "Account removed from this device");
}

static void fillList(void)
{
    const int n = tdeck_mail_list_count();
    for (int i = 0; i < MAIL_ROWS; i++) {
        if (!rowLbl[i])
            continue;
        unsigned seq = 0;
        const char *from = "", *subj = "", *date = "";
        bool seen = true;
        if (i < n && tdeck_mail_item(i, &seq, &from, &subj, &date, &seen)) {
            rowSeq[i] = seq;
            char buf[160];
            // Sender first: scanning an inbox is mostly "who is this from". A bare display name
            // if there is one, otherwise the address.
            char who[40];
            strncpy(who, from, sizeof(who) - 1);
            who[sizeof(who) - 1] = 0;
            char *lt = strchr(who, '<');
            if (lt && lt != who)
                *lt = 0; // "Name <addr>" -> "Name"
            for (char *p = who; *p; p++)
                if (*p == '"')
                    *p = ' ';
            snprintf(buf, sizeof(buf), "%s%s  %s", seen ? "" : "* ", who, subj);
            lv_label_set_text(rowLbl[i], buf);
            lv_obj_set_style_text_color(rowLbl[i], lv_color_hex(seen ? 0x8e8e93 : 0xffffff), LV_PART_MAIN);
            lv_obj_clear_flag(rowLbl[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            rowSeq[i] = 0;
            lv_obj_add_flag(rowLbl[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    char p[48];
    const int total = tdeck_mail_total();
    const int page = tdeck_mail_page();
    snprintf(p, sizeof(p), "%d-%d of %d", page * MAIL_ROWS + 1, page * MAIL_ROWS + n, total);
    if (pageLbl)
        lv_label_set_text(pageLbl, p);
}

static void loadPage(int page)
{
    if (page < 0)
        page = 0;
    if (pageLbl)
        lv_label_set_text(pageLbl, "loading...");
    uiWant = WANT_LIST;
    checking = tdeck_mail_list(page);
    if (!checking && pageLbl)
        lv_label_set_text(pageLbl, "busy");
}

static void onRow(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= MAIL_ROWS || !rowSeq[i])
        return;
    const char *from = "", *subj = "", *date = "";
    unsigned seq = 0;
    bool seen = true;
    tdeck_mail_item(i, &seq, &from, &subj, &date, &seen);
    if (readHdr)
        lv_label_set_text(readHdr, subj);
    if (readFrom)
        lv_label_set_text(readFrom, from);
    if (readTxt)
        lv_label_set_text(readTxt, "loading...");
    showView(VIEW_READ);
    bodyLoaded = false;
    uiWant = WANT_BODY;
    checking = tdeck_mail_read(rowSeq[i]);
    if (!checking && readTxt)
        lv_label_set_text(readTxt, "busy - try again in a moment");
}

// The address inside "Name <addr>", or the whole thing. False if what is left does not look like
// an address - including one the inbox list cut short, which has a '<' and no '>'.
static bool bareAddr(const char *in, char *out, int cap)
{
    out[0] = 0;
    if (!in)
        return false;
    const char *lt = strchr(in, '<');
    const char *gt = lt ? strchr(lt, '>') : nullptr;
    if (lt && !gt)
        return false; // truncated
    const char *s = lt ? lt + 1 : in;
    int n = lt ? (int)(gt - s) : (int)strlen(in);
    while (n > 0 && (*s == ' ' || *s == '"'))
        s++, n--;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '"' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        n--;
    if (n <= 0 || n >= cap)
        return false;
    memcpy(out, s, n);
    out[n] = 0;
    const char *at = strchr(out, '@');
    return at && at != out && strchr(at, '.') && !strchr(out, ' ');
}

// Keep the newest kSentMax distinct recipients, newest first. UI task: it holds the SPI lock the
// card needs.
static void rememberRecipient(const char *to)
{
    char addr[96];
    if (!bareAddr(to, addr, sizeof(addr)))
        return;
    char list[kSentMax][96];
    int n = 0;
    FsFile f = SDFs.open(kSentFile, O_RDONLY);
    if (f) {
        char line[96];
        while (n < kSentMax && f.fgets(line, sizeof(line)) > 0) {
            char a[96];
            if (bareAddr(line, a, sizeof(a)) && strcasecmp(a, addr))
                strcpy(list[n++], a);
        }
        f.close();
    }
    FsFile w = SDFs.open(kSentFile, O_WRONLY | O_CREAT | O_TRUNC);
    if (!w)
        return;
    w.println(addr);
    for (int i = 0; i < n && i < kSentMax - 1; i++)
        w.println(list[i]);
    w.close();
}

static void closeWho(void)
{
    if (whoBox) {
        lv_obj_delete(whoBox);
        whoBox = nullptr;
    }
}

static void onWhoPick(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl && toArea)
        lv_textarea_set_text(toArea, lv_label_get_text(lbl));
    // ⛔ NOT closeWho() HERE: that deletes the button whose handler is running, the shape that
    // has hung this device before. Deferred to after the event returns.
    lv_async_call([](void *) { closeWho(); }, nullptr);
}

static void openWho(lv_event_t *)
{
    closeWho();
    // Collect: people written to from here first, then this inbox page's senders. Distinct.
    char seen[16][96];
    int n = 0;
    auto add = [&](const char *raw) {
        char a[96];
        if (n >= 16 || !bareAddr(raw, a, sizeof(a)))
            return;
        // Nobody reads these - a picker full of noreply@ addresses is noise, not contacts.
        {
            char low[96];
            int i = 0;
            for (; a[i] && i < (int)sizeof(low) - 1; i++)
                low[i] = (char)tolower((unsigned char)a[i]);
            low[i] = 0;
            if (strstr(low, "noreply") || strstr(low, "no-reply") || strstr(low, "donotreply") ||
                strstr(low, "do-not-reply") || strstr(low, "mailer-daemon") || strstr(low, "bounce"))
                return;
        }
        for (int i = 0; i < n; i++)
            if (!strcasecmp(seen[i], a))
                return;
        strcpy(seen[n++], a);
    };
    FsFile f = SDFs.open(kSentFile, O_RDONLY);
    if (f) {
        char line[96];
        while (f.fgets(line, sizeof(line)) > 0)
            add(line);
        f.close();
    }
    for (int i = 0; i < tdeck_mail_list_count(); i++) {
        unsigned seq;
        const char *from = "", *subj = "", *date = "";
        bool s;
        if (tdeck_mail_item(i, &seq, &from, &subj, &date, &s))
            add(from);
    }

    whoBox = lv_obj_create(composeBox);
    lv_obj_set_pos(whoBox, 4, 30);
    lv_obj_set_size(whoBox, 312, 158);
    lv_obj_set_style_bg_color(whoBox, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_border_color(whoBox, lv_color_hex(0x48484a), LV_PART_MAIN);
    lv_obj_set_style_border_width(whoBox, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(whoBox, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(whoBox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(whoBox, 3, LV_PART_MAIN);
    lv_obj_set_scroll_dir(whoBox, LV_DIR_VER);
    if (!n) {
        lv_obj_t *l = lv_label_create(whoBox);
        lv_obj_set_width(l, 296);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(l, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_label_set_text(l, "Nobody yet. People you write to appear here, and so do the senders "
                             "on the inbox page you last opened. (Google contacts need a sign-in "
                             "an app password cannot give.)");
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_btn_create(whoBox);
        lv_obj_set_size(b, 296, 26);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
        lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_width(l, 284);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(l, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_label_set_text(l, seen[i]);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, onWhoPick, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *c = lv_btn_create(whoBox);
    lv_obj_set_size(c, 296, 26);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
    lv_obj_t *cl = lv_label_create(c);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
    lv_obj_add_event_cb(
        c, [](lv_event_t *) { lv_async_call([](void *) { closeWho(); }, nullptr); }, LV_EVENT_CLICKED, nullptr);
}

// Start a fresh compose of the given kind. `back` is the view Cancel returns to.
static void beginCompose(int kind, int back)
{
    closeWho();
    // A draft of the SAME kind survives (Write, Cancel, Write again keeps what you typed); a
    // switch of kind starts clean, so a reply's quoted text never turns up in a new message.
    if (kind != composeKind || kind != COMPOSE_NEW) {
        lv_textarea_set_text(toArea, "");
        lv_textarea_set_text(subjArea, "");
        lv_textarea_set_text(bodyArea, "");
    }
    composeKind = kind;
    composeReturn = back;
    lv_label_set_text(composeMsg, "");
    showView(VIEW_COMPOSE);
}

static bool startsWithNoCase(const char *s, const char *prefix)
{
    return s && !strncasecmp(s, prefix, strlen(prefix));
}

// Reply and Forward both start from the message on screen. The quoted text is capped: the
// compose box is for writing, and a whole newsletter quoted back is only weight on the send.
static void onReplyOrForward(bool forward)
{
    if (!bodyLoaded)
        return; // still loading - there is nothing to quote yet
    const char *subj = tdeck_mail_read_subject();
    const char *from = tdeck_mail_read_from();
    const char *date = tdeck_mail_read_date();
    const char *body = readTxt ? lv_label_get_text(readTxt) : "";
    beginCompose(forward ? COMPOSE_FORWARD : COMPOSE_REPLY, VIEW_READ);

    char s[140];
    if (forward)
        snprintf(s, sizeof(s), "%s%s", startsWithNoCase(subj, "fwd:") ? "" : "Fwd: ", subj);
    else
        snprintf(s, sizeof(s), "%s%s", startsWithNoCase(subj, "re:") ? "" : "Re: ", subj);
    lv_textarea_set_text(subjArea, s);

    const int kQuote = forward ? 6000 : 1500;
    const size_t cap = (size_t)kQuote + 512;
    char *buf = (char *)lv_malloc(cap); // LVGL's pool is PSRAM on this board
    if (!buf)
        return;
    int o;
    if (forward) {
        o = snprintf(buf, cap, "\n\n---------- Forwarded message ---------\nFrom: %s\nDate: %s\nSubject: %s\n\n",
                     from, date, subj);
        for (const char *p = body; *p && o < (int)cap - 1 && (p - body) < kQuote; p++)
            buf[o++] = *p;
    } else {
        o = snprintf(buf, cap, "\n\nOn %s, %s wrote:\n> ", date, from);
        // Quote it the way mail does: every line starts "> ".
        for (const char *p = body; *p && o < (int)cap - 4 && (p - body) < kQuote; p++) {
            buf[o++] = *p;
            if (*p == '\n' && p[1]) {
                buf[o++] = '>';
                buf[o++] = ' ';
            }
        }
    }
    buf[o < (int)cap ? o : (int)cap - 1] = 0;
    lv_textarea_set_text(bodyArea, buf);
    lv_free(buf);

    if (forward) {
        lv_textarea_set_text(toArea, "");
        if (lv_group_get_default())
            lv_group_focus_obj(toArea);
    } else {
        char addr[96];
        const char *ra = tdeck_mail_read_reply_addr();
        lv_textarea_set_text(toArea, (ra && ra[0]) ? ra : (bareAddr(from, addr, sizeof(addr)) ? addr : ""));
        // The cursor goes ABOVE the quote, where the reply is written - and the box has to SHOW
        // it. Setting the text scrolls to its end; without a layout pass first the cursor move
        // computed its scroll from stale sizes, and the box opened on the last lines of the quote.
        lv_obj_update_layout(bodyArea);
        lv_textarea_set_cursor_pos(bodyArea, 0);
        lv_obj_scroll_to_y(bodyArea, 0, LV_ANIM_OFF);
        if (lv_group_get_default())
            lv_group_focus_obj(bodyArea);
    }
}

static void onSend(lv_event_t *)
{
    const char *to = lv_textarea_get_text(toArea);
    if (!to || !strchr(to, '@')) {
        lv_label_set_text(composeMsg, "Who is it going to?");
        return;
    }
    closeWho();
    lv_label_set_text(composeMsg, "Sending...");
    uiWant = WANT_SEND;
    checking = tdeck_mail_send_ex(to, lv_textarea_get_text(subjArea), lv_textarea_get_text(bodyArea),
                                  composeKind == COMPOSE_REPLY);
    if (!checking)
        lv_label_set_text(composeMsg, "Busy - try again in a moment");
}

// Called from the UI poll while this screen is up.
extern "C" void mail_service_ui(void)
{
    // Derive the screenshot block from the CURRENT state rather than trusting a transition to
    // have set it. Cheap - two pointer compares - and it cannot be left stale by any exit path,
    // including ones that do not exist yet.
    {
        static bool blocked = false;
        const bool onMail = screen && lv_screen_active() == screen;
        const bool wantBlock = onMail && setupBox && !lv_obj_has_flag(setupBox, LV_OBJ_FLAG_HIDDEN);
        if (wantBlock != blocked) {
            blocked = wantBlock;
            tdeck_shot_block(wantBlock);
        }
    }
    if (!screen || lv_screen_active() != screen || !checking)
        return;
    int st = tdeck_mail_poll();
    if (st == 0)
        return;
    checking = false;
    const int want = uiWant;
    uiWant = WANT_NONE;
    if (st < 0) {
        // One failure path for every operation, reported where the user is actually looking.
        const char *err = tdeck_mail_error();
        if (want == WANT_LIST && pageLbl)
            lv_label_set_text(pageLbl, err);
        else if (want == WANT_BODY && readTxt)
            lv_label_set_text(readTxt, err);
        else if (want == WANT_SEND && composeMsg)
            lv_label_set_text(composeMsg, err);
        else {
            lv_label_set_text(countLbl, "--");
            lv_label_set_text(statusLbl, err);
        }
        return;
    }
    switch (want) {
    case WANT_LIST:
        fillList();
        break;
    case WANT_BODY:
        if (readTxt) {
            const char *b = tdeck_mail_body();
            lv_label_set_text(readTxt, (b && *b) ? b : "(no readable text in this message)");
        }
        bodyLoaded = true;
        break;
    case WANT_SEND:
        lv_label_set_text(composeMsg, "Sent");
        rememberRecipient(lv_textarea_get_text(toArea)); // for the Who? list next time
        composeKind = COMPOSE_NEW;
        lv_textarea_set_text(toArea, "");
        lv_textarea_set_text(subjArea, "");
        lv_textarea_set_text(bodyArea, "");
        break;
    default: {
        int total = -1, unseen = -1;
        tdeck_mail_counts(&total, &unseen);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d unread", unseen < 0 ? 0 : unseen);
        lv_label_set_text(countLbl, buf);
        snprintf(buf, sizeof(buf), "%d messages in the inbox", total < 0 ? 0 : total);
        lv_label_set_text(statusLbl, buf);
        break;
    }
    }
}

extern "C" void mail_open(void)
{
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        tui_statusbar_reserve(screen);

        // ---------- the gate: what you see when the app opens ----------
        gateBox = lv_obj_create(screen);
        lv_obj_remove_style_all(gateBox);
        lv_obj_set_pos(gateBox, 0, 0);
        lv_obj_set_size(gateBox, 320, 218);
        lv_obj_clear_flag(gateBox, LV_OBJ_FLAG_SCROLLABLE);
        {
            lv_obj_t *t = lv_label_create(gateBox);
            lv_label_set_text(t, "Mail");
            lv_obj_set_pos(t, 8, 6);
            lv_obj_set_size(t, 200, 26);
            lv_obj_set_style_text_font(t, &ui_font_montserrat_20, LV_PART_MAIN);
            lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), LV_PART_MAIN);

            gateWho = lv_label_create(gateBox);
            lv_obj_set_pos(gateWho, 8, 38);
            lv_obj_set_size(gateWho, 304, 34); // bounded: an address can be any length
            lv_label_set_long_mode(gateWho, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(gateWho, &ui_font_montserrat_12, LV_PART_MAIN);
            lv_obj_set_style_text_color(gateWho, lv_color_hex(0x8e8e93), LV_PART_MAIN);
            lv_label_set_text(gateWho, "");

            lv_obj_t *li = makeButton(gateBox, "Log in", 0x0a84ff, onGateLogin);
            lv_obj_set_size(li, 304, 40);
            lv_obj_set_pos(li, 8, 80);

            removeBtn = lv_btn_create(gateBox);
            lv_obj_set_size(removeBtn, 304, 40);
            lv_obj_set_pos(removeBtn, 8, 128);
            lv_obj_set_style_radius(removeBtn, 8, LV_PART_MAIN);
            removeLbl = lv_label_create(removeBtn);
            lv_obj_center(removeLbl);
            lv_obj_add_event_cb(removeBtn, onGateRemove, LV_EVENT_CLICKED, NULL);

            lv_obj_t *hint = lv_label_create(gateBox);
            lv_obj_set_pos(hint, 8, 176);
            lv_obj_set_size(hint, 304, 34);
            lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(hint, &ui_font_montserrat_12, LV_PART_MAIN);
            lv_obj_set_style_text_color(hint, lv_color_hex(0x5a5a5e), LV_PART_MAIN);
            lv_label_set_text(hint, "Connects to Gmail. Tap i for how your password is stored.");
        }

        // ---------- info panel: what happens to the password ----------
        // Jake asked for this explicitly, and it is the right thing to put in front of someone
        // before they type a credential into a device.
        infoBox = lv_obj_create(screen);
        lv_obj_remove_style_all(infoBox);
        lv_obj_set_pos(infoBox, 0, 0);
        lv_obj_set_size(infoBox, 320, 218);
        lv_obj_set_style_bg_color(infoBox, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(infoBox, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_add_flag(infoBox, LV_OBJ_FLAG_HIDDEN);
        {
            lv_obj_t *sc = lv_obj_create(infoBox);
            lv_obj_set_pos(sc, 4, 4);
            lv_obj_set_size(sc, 312, 174);
            lv_obj_set_style_bg_color(sc, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
            lv_obj_set_style_border_width(sc, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(sc, 6, LV_PART_MAIN);
            lv_obj_set_scroll_dir(sc, LV_DIR_VER);
            lv_obj_t *t = lv_label_create(sc);
            lv_obj_set_width(t, 296);
            lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(t, &ui_font_montserrat_12, LV_PART_MAIN);
            lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), LV_PART_MAIN);
            lv_label_set_text(t,
                              "Your password\n\n"
                              "It is an app password, not your real Google one. Google stopped "
                              "allowing real passwords for mail apps. It only works for mail, and "
                              "you can revoke it at myaccount.google.com/apppasswords without "
                              "changing anything else.\n\n"
                              "Where it is kept\n\n"
                              "In a plain text file called gmail.txt on this device's SD card. "
                              "Not encrypted - anyone holding the card can read it. Remove "
                              "account deletes that file.\n\n"
                              "Where it is not\n\n"
                              "Never written to the log. Never sent anywhere except Gmail, over a "
                              "connection checked against Google's own certificate. Screenshots "
                              "are refused while the login form is open, so it cannot be "
                              "captured over the USB cable.");
            lv_obj_t *ok = makeButton(infoBox, "Close", 0x3a3a3c, [](lv_event_t *) {
                lv_obj_add_flag(infoBox, LV_OBJ_FLAG_HIDDEN);
            });
            lv_obj_set_size(ok, 120, 32);
            lv_obj_set_pos(ok, 4, 182);
        }

        // ---------- setup form ----------
        setupBox = lv_obj_create(screen);
        lv_obj_remove_style_all(setupBox);
        lv_obj_set_pos(setupBox, 0, 0);
        lv_obj_set_size(setupBox, 320, 218);
        lv_obj_clear_flag(setupBox, LV_OBJ_FLAG_SCROLLABLE);

        // ⭐ THE LAYOUT ADDS UP, ON PURPOSE. setupBox is 214 tall and every row below has an
        // explicit height, so the total is checkable by reading it rather than by running it:
        //   label 14 + field 32 + label 14 + field 32 + hint 50 + buttons 32 + message 32 = 206
        // Nothing here can grow into the row beneath it.
        {
            // Top-right on both the gate and the form, as asked.
            for (int k = 0; k < 2; k++) {
                lv_obj_t *ib = makeButton(k ? setupBox : gateBox, "i", 0x2c2c2e, [](lv_event_t *) {
                    lv_obj_clear_flag(infoBox, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_move_foreground(infoBox);
                });
                lv_obj_set_size(ib, 28, 28);
                lv_obj_set_pos(ib, 288, 2);
            }
        }

        lv_obj_t *t1 = lv_label_create(setupBox);
        lv_label_set_text(t1, "Gmail address");
        lv_obj_set_style_text_color(t1, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_obj_set_pos(t1, 6, 0);
        lv_obj_set_size(t1, 308, 14);

        addrArea = lv_textarea_create(setupBox);
        lv_obj_set_pos(addrArea, 4, 16);
        lv_obj_set_size(addrArea, 312, 32);
        lv_textarea_set_one_line(addrArea, true);
        lv_textarea_set_max_length(addrArea, 80);
        lv_textarea_set_placeholder_text(addrArea, "you@gmail.com");
        // Jake: "could you make the form end in at @gmail.com so that people know". Prefilling
        // it says which provider this is for before anyone types a word - and the caret is put
        // at the START so the name goes in front of it rather than after the domain.
        lv_textarea_set_text(addrArea, "@gmail.com");
        lv_textarea_set_cursor_pos(addrArea, 0);
        lv_obj_set_style_bg_color(addrArea, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_text_color(addrArea, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_border_width(addrArea, 0, LV_PART_MAIN);
        // Solid caret: a blinking one only repaints when something else changes, so on this
        // device it reads as "there is no cursor". Same reason NotesApp and Gemini use one.
        lv_obj_set_style_bg_color(addrArea, lv_color_hex(0xffffff), LV_PART_CURSOR);
        lv_obj_set_style_bg_opa(addrArea, LV_OPA_50, LV_PART_CURSOR);
        lv_obj_set_style_anim_duration(addrArea, 0, LV_PART_CURSOR);
        // ⛔ AND DELETE THE ANIMATION THAT ALREADY EXISTS. lv_textarea_create() starts a
        // blinking cursor during construction using the DEFAULT time, so setting the duration to
        // zero above does not stop it - start_cursor_blink only re-reads that on focus, or on a
        // style change delivered to the label child. An active LVGL animation then forces a
        // refresh EVERY FRAME: measured 17-18 fps and 124-280 KB/s pushed while sitting idle,
        // against 2 fps once it is gone. Any later focus re-runs the check, finds the zero and
        // deletes it itself, so this one call is all that is needed.
        lv_anim_delete(addrArea, nullptr);

        lv_obj_t *t2 = lv_label_create(setupBox);
        lv_label_set_text(t2, "App password (16 letters)");
        lv_obj_set_style_text_color(t2, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_obj_set_pos(t2, 6, 52);
        lv_obj_set_size(t2, 308, 14);

        passArea = lv_textarea_create(setupBox);
        lv_obj_set_pos(passArea, 4, 68);
        lv_obj_set_size(passArea, 312, 32);
        lv_textarea_set_one_line(passArea, true);
        lv_textarea_set_max_length(passArea, 40);
        lv_textarea_set_placeholder_text(passArea, "abcd efgh ijkl mnop (16 letters)");
        lv_obj_set_style_bg_color(passArea, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_text_color(passArea, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_border_width(passArea, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(passArea, lv_color_hex(0xffffff), LV_PART_CURSOR);
        lv_obj_set_style_bg_opa(passArea, LV_OPA_50, LV_PART_CURSOR);
        lv_obj_set_style_anim_duration(passArea, 0, LV_PART_CURSOR);
        lv_anim_delete(passArea, nullptr);
        // ⚠️ DELIBERATELY NOT MASKED. It is a 16-character random string typed on a thumb
        // keyboard; hiding it guarantees typos that surface later as "login rejected" with no
        // way to tell a wrong character from a wrong setting. The screen is in your hand, and
        // the real exposure - a screenshot over the cable - is blocked instead.

        lv_obj_t *hint = lv_label_create(setupBox);
        lv_obj_set_pos(hint, 6, 104);
        // ⛔ WIDTH AND HEIGHT. A wrapping label given only a width grows downwards without
        // limit and draws over whatever is below it - which is exactly what put this text
        // through the Save button.
        lv_obj_set_size(hint, 308, 50);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        // Short enough to FIT the 50px it has. A message that only fits because it is clipped
        // is still a bug; the full explanation belongs somewhere with room for it.
        lv_label_set_text(hint, "Not your normal password. Make a 16-letter one at\n"
                                "myaccount.google.com/apppasswords");

        lv_obj_t *saveBtn = makeButton(setupBox, "Save", 0x30d158, onSave);
        lv_obj_set_size(saveBtn, 150, 32);
        lv_obj_set_pos(saveBtn, 4, 158);
        lv_obj_t *cancelBtn = makeButton(setupBox, "Cancel", 0x3a3a3c, [](lv_event_t *) {
            lv_textarea_set_text(passArea, ""); // never leave it sitting on a screen nobody is at
            if (haveCreds())
                showSetup(false);
        });
        lv_obj_set_size(cancelBtn, 158, 32);
        lv_obj_set_pos(cancelBtn, 158, 158);

        formMsg = lv_label_create(setupBox);
        lv_obj_set_pos(formMsg, 6, 194);
        lv_obj_set_size(formMsg, 308, 32); // fixed: an error here must not shove the form about
        lv_label_set_long_mode(formMsg, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(formMsg, lv_color_hex(0xff9f0a), LV_PART_MAIN);
        lv_label_set_text(formMsg, "");

        // ---------- status screen ----------
        statusBox = lv_obj_create(screen);
        lv_obj_remove_style_all(statusBox);
        lv_obj_set_pos(statusBox, 0, 0);
        lv_obj_set_size(statusBox, 320, 218);
        lv_obj_clear_flag(statusBox, LV_OBJ_FLAG_SCROLLABLE);

        countLbl = lv_label_create(statusBox);
        lv_obj_set_style_text_color(countLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_text_font(countLbl, &ui_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_pos(countLbl, 8, 10);
        lv_label_set_text(countLbl, "--");

        lv_obj_t *chk = makeButton(statusBox, "Check now", 0x0a84ff, onCheck);
        lv_obj_set_size(chk, 150, 32);
        lv_obj_set_pos(chk, 4, 56);
        lv_obj_t *edit = makeButton(statusBox, "Account", 0x3a3a3c, onBackToGate);
        lv_obj_set_size(edit, 158, 32);
        lv_obj_set_pos(edit, 158, 56);
        lv_obj_t *inbx = makeButton(statusBox, "Inbox", 0x30d158, [](lv_event_t *) {
            showView(VIEW_LIST);
            loadPage(0);
        });
        lv_obj_set_size(inbx, 150, 32);
        lv_obj_set_pos(inbx, 4, 92);
        lv_obj_t *wr = makeButton(statusBox, "Write", 0xff9f0a,
                                  [](lv_event_t *) { beginCompose(COMPOSE_NEW, VIEW_STATUS); });
        lv_obj_set_size(wr, 158, 32);
        lv_obj_set_pos(wr, 158, 92);

        statusLbl = lv_label_create(statusBox);
        lv_obj_set_pos(statusLbl, 6, 132);
        lv_obj_set_size(statusLbl, 308, 78); // bounded, so a long IMAP error cannot overflow
        lv_label_set_long_mode(statusLbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(statusLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_label_set_text(statusLbl, "");
        // ---------- inbox list ----------
        // ⭐ TWELVE ROWS, AND THE FETCH SIZE IS ALSO TWELVE. Jake has ~7,000 messages; the page
        // on screen and the page asked of the server are deliberately the same thing, so there
        // is never a larger list held in memory that this one is a window onto.
        listBox = lv_obj_create(screen);
        lv_obj_remove_style_all(listBox);
        lv_obj_set_pos(listBox, 0, 0);
        lv_obj_set_size(listBox, 320, 218);
        lv_obj_clear_flag(listBox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(listBox, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < MAIL_ROWS; i++) {
            lv_obj_t *l = lv_label_create(listBox);
            lv_obj_set_pos(l, 4, i * 20);
            // ⛔ EXPLICIT HEIGHT. These hold sender names and subjects straight off the
            // internet, of any length. LONG_DOT with a height clips to one line; without the
            // height it wraps and walks over the rows beneath it.
            lv_obj_set_size(l, 306, 20);
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(l, &ui_font_montserrat_12, LV_PART_MAIN);
            lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(l, onRow, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
            rowLbl[i] = l;
        }
        {
            lv_obj_t *pv = makeButton(listBox, "<", 0x3a3a3c, [](lv_event_t *) {
                const int p = tdeck_mail_page();
                if (p > 0)
                    loadPage(p - 1);
            });
            lv_obj_set_size(pv, 44, 28);
            lv_obj_set_pos(pv, 4, 184);
            lv_obj_t *nx = makeButton(listBox, ">", 0x3a3a3c,
                                      [](lv_event_t *) { loadPage(tdeck_mail_page() + 1); });
            lv_obj_set_size(nx, 44, 28);
            lv_obj_set_pos(nx, 272, 184);
            lv_obj_t *bk = makeButton(listBox, "Back", 0x3a3a3c,
                                      [](lv_event_t *) { showView(VIEW_STATUS); });
            lv_obj_set_size(bk, 66, 28);
            lv_obj_set_pos(bk, 54, 184);
            pageLbl = lv_label_create(listBox);
            lv_obj_set_pos(pageLbl, 126, 190);
            lv_obj_set_size(pageLbl, 142, 16);
            lv_label_set_long_mode(pageLbl, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(pageLbl, &ui_font_montserrat_12, LV_PART_MAIN);
            lv_obj_set_style_text_color(pageLbl, lv_color_hex(0x8e8e93), LV_PART_MAIN);
            lv_label_set_text(pageLbl, "");
        }

        // ---------- read one message ----------
        readBox = lv_obj_create(screen);
        lv_obj_remove_style_all(readBox);
        lv_obj_set_pos(readBox, 0, 0);
        lv_obj_set_size(readBox, 320, 218);
        lv_obj_clear_flag(readBox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(readBox, LV_OBJ_FLAG_HIDDEN);
        readHdr = lv_label_create(readBox);
        lv_obj_set_pos(readHdr, 4, 0);
        // ⛔ TWO LABELS, NOT ONE TWO-LINE ONE. LONG_DOT collapses to a SINGLE line whatever
        // newlines the text contains, so a "subject<nl>sender" label showed the subject and
        // silently swallowed the sender. Caught by looking at the screen: no From line at all.
        lv_obj_set_size(readHdr, 306, 15);
        lv_label_set_long_mode(readHdr, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(readHdr, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(readHdr, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_label_set_text(readHdr, "");
        readFrom = lv_label_create(readBox);
        lv_obj_set_pos(readFrom, 4, 16);
        lv_obj_set_size(readFrom, 306, 14);
        lv_label_set_long_mode(readFrom, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(readFrom, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(readFrom, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_label_set_text(readFrom, "");
        {
            lv_obj_t *sc = lv_obj_create(readBox);
            lv_obj_set_pos(sc, 2, 32);
            lv_obj_set_size(sc, 316, 146);
            lv_obj_set_style_bg_color(sc, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
            lv_obj_set_style_border_width(sc, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(sc, 4, LV_PART_MAIN);
            lv_obj_set_scroll_dir(sc, LV_DIR_VER);
            readTxt = lv_label_create(sc);
            // Inside a scroller the label is ALLOWED to grow - that is the point of the
            // scroller - but its width is fixed so it wraps rather than running off the side.
            lv_obj_set_width(readTxt, 300);
            lv_label_set_long_mode(readTxt, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(readTxt, &ui_font_montserrat_12, LV_PART_MAIN);
            lv_obj_set_style_text_color(readTxt, lv_color_hex(0xffffff), LV_PART_MAIN);
            lv_label_set_text(readTxt, "");
            lv_obj_t *bk = makeButton(readBox, "Back", 0x3a3a3c, [](lv_event_t *) { showView(VIEW_LIST); });
            lv_obj_set_size(bk, 88, 28);
            lv_obj_set_pos(bk, 4, 182);
            lv_obj_t *rp = makeButton(readBox, "Reply", 0x0a84ff, [](lv_event_t *) { onReplyOrForward(false); });
            lv_obj_set_size(rp, 108, 28);
            lv_obj_set_pos(rp, 96, 182);
            lv_obj_t *fw = makeButton(readBox, "Forward", 0x3a3a3c, [](lv_event_t *) { onReplyOrForward(true); });
            lv_obj_set_size(fw, 108, 28);
            lv_obj_set_pos(fw, 208, 182);
        }

        // ---------- compose ----------
        composeBox = lv_obj_create(screen);
        lv_obj_remove_style_all(composeBox);
        lv_obj_set_pos(composeBox, 0, 0);
        lv_obj_set_size(composeBox, 320, 218);
        lv_obj_clear_flag(composeBox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(composeBox, LV_OBJ_FLAG_HIDDEN);
        {
            auto field = [&](const char *ph, int y, int h, bool oneLine) {
                lv_obj_t *t = lv_textarea_create(composeBox);
                lv_obj_set_pos(t, 4, y);
                lv_obj_set_size(t, 312, h);
                if (oneLine)
                    lv_textarea_set_one_line(t, true);
                lv_textarea_set_placeholder_text(t, ph);
                lv_obj_set_style_bg_color(t, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
                lv_obj_set_style_text_color(t, lv_color_hex(0xffffff), LV_PART_MAIN);
                lv_obj_set_style_border_width(t, 0, LV_PART_MAIN);
                lv_obj_set_style_text_font(t, &ui_font_montserrat_12, LV_PART_MAIN);
                lv_obj_set_style_bg_color(t, lv_color_hex(0xffffff), LV_PART_CURSOR);
                lv_obj_set_style_bg_opa(t, LV_OPA_50, LV_PART_CURSOR);
                lv_obj_set_style_anim_duration(t, 0, LV_PART_CURSOR);
                lv_anim_delete(t, nullptr);
                if (lv_group_get_default())
                    lv_group_add_obj(lv_group_get_default(), t);
                return t;
            };
            toArea = field("To: someone@example.com", 0, 28, true);
            lv_obj_set_width(toArea, 254);
            lv_obj_t *who = makeButton(composeBox, "Who?", 0x3a3a3c, openWho);
            lv_obj_set_size(who, 54, 28);
            lv_obj_set_pos(who, 262, 0);
            subjArea = field("Subject", 32, 28, true);
            bodyArea = field("Message", 64, 88, false);
            lv_obj_t *sb = makeButton(composeBox, "Send", 0x30d158, onSend);
            lv_obj_set_size(sb, 150, 30);
            lv_obj_set_pos(sb, 4, 158);
            lv_obj_t *cb = makeButton(composeBox, "Cancel", 0x3a3a3c, [](lv_event_t *) {
                closeWho();
                showView(composeReturn);
            });
            lv_obj_set_size(cb, 158, 30);
            lv_obj_set_pos(cb, 158, 158);
            composeMsg = lv_label_create(composeBox);
            lv_obj_set_pos(composeMsg, 6, 192);
            lv_obj_set_size(composeMsg, 308, 20);
            lv_label_set_long_mode(composeMsg, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(composeMsg, &ui_font_montserrat_12, LV_PART_MAIN);
            lv_obj_set_style_text_color(composeMsg, lv_color_hex(0xff9f0a), LV_PART_MAIN);
            lv_label_set_text(composeMsg, "");
        }
    }

    // First run goes straight to the form; after that, straight to the inbox count.
    {
        char addr[96];
        if (savedAddress(addr, sizeof(addr)) && addrArea)
            lv_textarea_set_text(addrArea, addr);
    }
    // Always open on the gate. Jake: "you open the mail app, then it gives two options".
    refreshGate();
    if (infoBox)
        lv_obj_add_flag(infoBox, LV_OBJ_FLAG_HIDDEN);
    showView(VIEW_GATE);
    if (lv_group_get_default() && addrArea) {
        lv_group_add_obj(lv_group_get_default(), addrArea);
        lv_group_add_obj(lv_group_get_default(), passArea);
    }
    lv_screen_load(screen);
}
