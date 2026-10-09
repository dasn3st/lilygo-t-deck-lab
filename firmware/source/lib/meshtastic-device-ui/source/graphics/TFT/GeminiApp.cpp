// -----------------------------------------------------------------------------
// Ollama Agent for the colour T-Deck — the screen. The network lives in src/TDeckGemini.cpp.
//
// The original Gemini experiment became the provider-neutral Ollama Agent.
//
// Type a question, press Ask (or Enter), read the answer. That is the whole app, and
// deliberately so: this is a handheld with a thumb keyboard, and every control added is another
// thing to hit by accident while typing.
//
// ⚠️ ASKING DROPS BLUETOOTH until the next reboot — one antenna, and starting wi-fi tears BT
// down. Jake accepted that ("I won't Bluetooth much to my tdeck") but the screen says so anyway,
// because losing a phone link with no explanation is the kind of thing that gets blamed on a
// bug three weeks later.
//
// THREADING: nothing here touches the network. tdeckgemini::ask() records the question and the
// main loop does the work; this polls the state and repaints. The UI task must never block on a
// socket — the rule the whole launcher is built on.
// -----------------------------------------------------------------------------
#include "graphics/view/TFT/TuiStatusBar.h"
#include "graphics/common/SdCard.h"
#include "lvgl.h"
#include "TDeckClipboard.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdint>

extern const lv_font_t ui_font_montserrat_12;
extern const lv_font_t ui_font_montserrat_14;
extern const lv_font_t ui_font_montserrat_16;
extern "C" void ollama_open(void);

// src/TDeckGemini.cpp — the linker joins these across the src/ and lib/ boundary, the same trick
// the mesh kill-switch and the buzzer helpers use.
namespace tdeckgemini
{
enum ConversationMode { MODE_CHAT = 0, MODE_MARKDOWN = 1, MODE_CALENDAR = 2 };
enum State { IDLE = 0, WORKING, DONE, FAILED };
void ask(const char *prompt);
void saveSummary();
bool hasCalendarDraft();
bool saveCalendarDraft();
bool hasConversation();
void restoreSession();
void deleteSession();
const char *conversationText();
void setMode(tdeckgemini::ConversationMode mode);
tdeckgemini::ConversationMode mode();
int state();
const char *reply();
const char *statusText();
bool haveConfig();
void refreshModels();
int modelCount();
const char *modelName();
void nextModel();
void clear();
void release();
} // namespace tdeckgemini

namespace tdeckvoice
{
int state();
const char *statusText();
bool voiceEnabled();
void setVoiceEnabled(bool enabled);
bool startRecording();
void setTarget(lv_obj_t *target);
uint32_t recordingElapsedSeconds();
} // namespace tdeckvoice

namespace
{
lv_obj_t *screen = nullptr;
lv_obj_t *askArea = nullptr;
// True while the box holds a question that has already been sent: drawn grey, and
// wiped by the first keystroke of the next one.
bool s_sentText = false;
lv_obj_t *helpBox = nullptr; // "how do I get a key" overlay, behind the ? in the corner
lv_obj_t *menuBox = nullptr;
lv_obj_t *chatBox = nullptr;
size_t s_sentLen = 0; // how long the sent question was, so the next keystroke can drop exactly it
lv_obj_t *answerLbl = nullptr;
lv_obj_t *statusLbl = nullptr;
lv_obj_t *headerModelLbl = nullptr;
lv_obj_t *headerModeLbl = nullptr;
lv_obj_t *askBtnLbl = nullptr;
lv_obj_t *modelBtnLbl = nullptr;
lv_obj_t *modeBtnLbl = nullptr;
lv_obj_t *voiceBtn = nullptr;
lv_obj_t *voiceBtnLbl = nullptr;
lv_obj_t *voiceToggleLbl = nullptr;
lv_timer_t *pollTimer = nullptr;
lv_timer_t *focusGuard = nullptr;
int lastState = -1;
int lastVoiceState = -1;
uint32_t voiceHeaderUntilMs = 0;
int lastModelCount = -1;
bool calendarScreen = false;
lv_obj_t *calendarScroll = nullptr;
lv_obj_t *calendarContent = nullptr;
lv_obj_t *calendarGrid = nullptr;
lv_obj_t *calendarTitle = nullptr;
lv_obj_t *calendarStatus = nullptr;
lv_obj_t *calendarDetail = nullptr;
lv_obj_t *calendarDateLabel = nullptr;
lv_obj_t *calendarNoteArea = nullptr;
lv_obj_t *calendarAlarmTimeArea = nullptr;
lv_obj_t *calendarMarkLabel = nullptr;
lv_obj_t *calendarAlarmLabel = nullptr;
int calendarYear = 0;
int calendarMonth = 0; // 1..12
int calendarSelectedDay = 0;
bool calendarMarked = false;
bool calendarAlarm = false;
char calendarAlarmTime[6] = "08:00";
char calendarNote[161] = {0};
int chatZoom = 1;      // 0=compact, 1=normal, 2=large

void refreshVoiceControls()
{
    const int st = tdeckvoice::state();
    const bool visible = tdeckvoice::voiceEnabled() || st == 1 || st == 2;
    if (voiceBtn) {
        if (visible) lv_obj_clear_flag(voiceBtn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(voiceBtn, LV_OBJ_FLAG_HIDDEN);
        char label[24];
        if (st == 1)
            snprintf(label, sizeof(label), "%02u:%02u",
                     (unsigned)(tdeckvoice::recordingElapsedSeconds() / 60),
                     (unsigned)(tdeckvoice::recordingElapsedSeconds() % 60));
        else
            snprintf(label, sizeof(label), st == 2 ? "..." : "Mic");
        lv_label_set_text(voiceBtnLbl, label);
        lv_obj_set_style_bg_color(voiceBtn, lv_color_hex(st == 1 ? 0xff453a : st == 2 ? 0x0a84ff : 0x30a46c), LV_PART_MAIN);
        if (st == 2) lv_obj_add_state(voiceBtn, LV_STATE_DISABLED);
        else lv_obj_clear_state(voiceBtn, LV_STATE_DISABLED);
    }
    if (askArea)
        lv_obj_set_width(askArea, visible ? 208 : 252);
    if (voiceToggleLbl)
        lv_label_set_text(voiceToggleLbl, tdeckvoice::voiceEnabled() ? "Spracheingabe AN" : "Spracheingabe AUS");
}

void toggleVoiceEnabled(lv_event_t *)
{
    const int st = tdeckvoice::state();
    if (st == 1 || st == 2)
        return;
    tdeckvoice::setVoiceEnabled(!tdeckvoice::voiceEnabled());
    refreshVoiceControls();
}

void onVoiceButton(lv_event_t *)
{
    tdeckvoice::setTarget(askArea);
    tdeckvoice::startRecording();
    refreshVoiceControls();
}

void destroyUiOnly()
{
    if (pollTimer) {
        lv_timer_delete(pollTimer);
        pollTimer = nullptr;
    }
    if (focusGuard) {
        lv_timer_delete(focusGuard);
        focusGuard = nullptr;
    }
    if (screen) {
        lv_obj_delete(screen);
        screen = nullptr;
    }
    askArea = answerLbl = statusLbl = headerModelLbl = headerModeLbl = askBtnLbl = modelBtnLbl = modeBtnLbl = nullptr;
    voiceBtn = voiceBtnLbl = voiceToggleLbl = nullptr;
    helpBox = menuBox = chatBox = calendarScroll = calendarContent = calendarGrid = calendarTitle = calendarStatus = nullptr;
    calendarDetail = calendarDateLabel = calendarNoteArea = calendarAlarmTimeArea = nullptr;
    calendarMarkLabel = calendarAlarmLabel = nullptr;
    lastState = -1;
    lastVoiceState = -1;
    voiceHeaderUntilMs = 0;
    calendarScreen = false;
}

lv_obj_t *makeButton(lv_obj_t *parent, const char *text, uint32_t colour, lv_event_cb_t cb, lv_obj_t **labelOut)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(colour), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    if (labelOut)
        *labelOut = lbl;
    if (cb)
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

void addHamburgerIcon(lv_obj_t *btn)
{
    for (int i = 0; i < 3; i++) {
        lv_obj_t *line = lv_obj_create(btn);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, 14, 2);
        lv_obj_align(line, LV_ALIGN_CENTER, 0, (i - 1) * 5);
        lv_obj_set_style_bg_color(line, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    }
}

void setStatus(const char *s, uint32_t colour)
{
    if (!statusLbl)
        return;
    lv_label_set_text(statusLbl, s);
    lv_obj_set_style_text_color(statusLbl, lv_color_hex(colour), LV_PART_MAIN);
}

void closeMenu()
{
    if (menuBox)
        lv_obj_add_flag(menuBox, LV_OBJ_FLAG_HIDDEN);
}

void toggleMenu(lv_event_t *)
{
    if (!menuBox)
        return;
    if (lv_obj_has_flag(menuBox, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(menuBox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(menuBox);
    } else {
        closeMenu();
    }
}

void renderConversation()
{
    if (!answerLbl)
        return;
    lv_label_set_text(answerLbl, tdeckgemini::conversationText());
    if (chatBox)
        lv_obj_scroll_to_y(chatBox, LV_COORD_MAX, LV_ANIM_OFF);
}

void applyChatZoom(void)
{
    if (!answerLbl)
        return;
    const lv_font_t *font = chatZoom == 0 ? &ui_font_montserrat_12
                             : chatZoom == 1 ? &ui_font_montserrat_14
                                              : &ui_font_montserrat_16;
    lv_obj_set_style_text_font(answerLbl, font, LV_PART_MAIN);
    renderConversation();
}

void zoomChatOut(lv_event_t *)
{
    if (chatZoom > 0)
        chatZoom--;
    applyChatZoom();
    setStatus(chatZoom == 0 ? "Zoom klein" : "Zoom normal", 0x30d158);
}

void zoomChatIn(lv_event_t *)
{
    if (chatZoom < 2)
        chatZoom++;
    applyChatZoom();
    setStatus(chatZoom == 2 ? "Zoom gross" : "Zoom normal", 0x30d158);
}

void refreshModelLabel()
{
    if (!modelBtnLbl && !headerModelLbl)
        return;
    char text[64];
    if (modelBtnLbl) {
        snprintf(text, sizeof(text), "Modell: %.46s", tdeckgemini::modelName());
        lv_label_set_text(modelBtnLbl, text);
    }
    if (headerModelLbl) {
        snprintf(text, sizeof(text), "%.36s", tdeckgemini::modelName());
        lv_label_set_text(headerModelLbl, text);
    }
}

void refreshModeLabel()
{
    if (!modeBtnLbl && !headerModeLbl)
        return;
    const tdeckgemini::ConversationMode current = tdeckgemini::mode();
    const char *menuText = current == tdeckgemini::MODE_CALENDAR
                               ? "Kalendermodus"
                               : current == tdeckgemini::MODE_MARKDOWN ? "Markdown-Modus" : "Chat-Modus";
    const char *headerText = current == tdeckgemini::MODE_CALENDAR
                                 ? "Kalender"
                                 : current == tdeckgemini::MODE_MARKDOWN ? "Markdown" : "Chat";
    if (modeBtnLbl)
        lv_label_set_text(modeBtnLbl, menuText);
    if (headerModeLbl)
        lv_label_set_text(headerModeLbl, headerText);
}

void chooseMode(lv_event_t *)
{
    if (tdeckgemini::hasConversation()) {
        setStatus("Vor dem Moduswechsel Chat löschen", 0xff9f0a);
        return;
    }
    const tdeckgemini::ConversationMode next =
        tdeckgemini::mode() == tdeckgemini::MODE_MARKDOWN ? tdeckgemini::MODE_CHAT : tdeckgemini::MODE_MARKDOWN;
    tdeckgemini::setMode(next);
    refreshModeLabel();
    setStatus(next == tdeckgemini::MODE_MARKDOWN ? "Markdown-Modus" : "Chat-Modus", 0x30d158);
}

void chooseModel(lv_event_t *)
{
    if (tdeckgemini::modelCount() <= 0) {
        tdeckgemini::refreshModels();
        setStatus("Loading Ollama models...", 0x0a84ff);
        return;
    }
    tdeckgemini::nextModel();
    refreshModelLabel();
    setStatus("Modell ausgewählt", 0x30d158);
}

void doSaveNote(lv_event_t *);

bool handleSlashCommand(const char *q)
{
    if (!q)
        return false;
    if (!strcmp(q, "/markdown")) {
        tdeckgemini::setMode(tdeckgemini::MODE_MARKDOWN);
        refreshModeLabel();
        lv_textarea_set_text(askArea, "");
        s_sentText = false;
        setStatus("Markdown mode", 0x30d158);
        return true;
    }
    if (!strcmp(q, "/kalender")) {
        tdeckgemini::setMode(tdeckgemini::MODE_CALENDAR);
        refreshModeLabel();
        lv_textarea_set_text(askArea, "");
        s_sentText = false;
        setStatus("Kalendermodus", 0x30d158);
        return true;
    }
    if (!strcmp(q, "/save")) {
        lv_textarea_set_text(askArea, "");
        s_sentText = false;
        doSaveNote(nullptr);
        return true;
    }
    return false;
}

void doAsk(lv_event_t *)
{
    if (!askArea)
        return;
    const char *q = lv_textarea_get_text(askArea);
    if (!q || !*q) {
        setStatus("Schreib zuerst eine Frage", 0xff9f0a);
        return;
    }
    if (handleSlashCommand(q))
        return;
    if (!tdeckgemini::haveConfig()) {
        setStatus("API-Schlüssel fehlt: /ollama.txt auf der SD-Karte", 0xff453a);
        return;
    }
    setStatus("Asking... (this turns Bluetooth off)", 0x0a84ff);
    tdeckgemini::ask(q);

    // ⭐ THE SENT QUESTION GOES GREY AND GETS OUT OF THE WAY. Jake: "when I type and send a
    // message, the message retains there. Instead can me last sent message be grayed out in the
    // type box, and automatically be overwritten when I start typing again?"
    //
    // Leaving it black and editable reads as "this has not been sent yet", and the next question
    // has to be deleted character by character first. Grey says "this one is gone", and the first
    // keystroke clears it - see the LV_EVENT_VALUE_CHANGED handler where the textarea is built.
    s_sentText = true;
    s_sentLen = strlen(q);
    lv_obj_set_style_text_color(askArea, lv_color_hex(0x8e8e93), LV_PART_MAIN);
}

void doSaveNote(lv_event_t *)
{
    if (tdeckgemini::mode() == tdeckgemini::MODE_CALENDAR) {
        if (!tdeckgemini::hasCalendarDraft()) {
            setStatus("Make a calendar request first", 0xff9f0a);
            return;
        }
        const bool saved = tdeckgemini::saveCalendarDraft();
        setStatus(saved ? "Saved to calendar" : "Could not save calendar", saved ? 0x30d158 : 0xff453a);
        closeMenu();
        return;
    }
    if (!tdeckgemini::hasConversation()) {
        setStatus("Stell zuerst eine Frage", 0xff9f0a);
        return;
    }
    if (!tdeckgemini::haveConfig()) {
        setStatus("API-Schlüssel fehlt: /ollama.txt auf der SD-Karte", 0xff453a);
        return;
    }
    setStatus("Zusammenfassung für Notizen ...", 0x0a84ff);
    tdeckgemini::saveSummary();
    closeMenu();
}

void onClear(lv_event_t *)
{
    tdeckgemini::deleteSession();
    refreshModeLabel();
    if (askArea) {
        lv_textarea_set_text(askArea, "");
        s_sentText = false;
        lv_obj_set_style_text_color(askArea, lv_color_hex(0xffffff), LV_PART_MAIN);
    }
    if (answerLbl)
        renderConversation();
    setStatus("Chat gelöscht", 0xff9f0a);
    closeMenu();
}

// Poll the state machine and repaint when it moves. Only on a CHANGE: the answer label is long
// and rewriting it every tick would make the list scroll jump under a reading thumb.
void tick(lv_timer_t *)
{
    refreshVoiceControls();
    const int voiceState = tdeckvoice::state();
    if (voiceState != lastVoiceState) {
        lastVoiceState = voiceState;
        const char *voiceStatus = tdeckvoice::statusText();
        if (voiceStatus && voiceStatus[0]) {
            const uint32_t colour = voiceState == 4 ? 0xff453a :
                                    voiceState == 3 ? 0x30d158 :
                                    voiceState == 1 || voiceState == 2 ? 0x0a84ff : 0xc7c7cc;
            setStatus(voiceStatus, colour);
        }
        if (headerModeLbl && (voiceState == 1 || voiceState == 2)) {
            lv_label_set_text(headerModeLbl, voiceState == 1 ? "Mikro aktiv" : "Transkription …");
            lv_obj_set_style_text_color(headerModeLbl, lv_color_hex(voiceState == 1 ? 0xff453a : 0x0a84ff),
                                        LV_PART_MAIN);
            voiceHeaderUntilMs = 0;
        } else if (headerModeLbl && (voiceState == 3 || voiceState == 4)) {
            lv_label_set_text(headerModeLbl, voiceState == 3 ? "Sprache bereit" : "Mikrofehler");
            lv_obj_set_style_text_color(headerModeLbl, lv_color_hex(voiceState == 3 ? 0x30d158 : 0xff9f0a),
                                        LV_PART_MAIN);
            voiceHeaderUntilMs = millis() + 2500;
        }
    }
    if (voiceHeaderUntilMs && (int32_t)(millis() - voiceHeaderUntilMs) >= 0) {
        voiceHeaderUntilMs = 0;
        refreshModeLabel();
    }
    const int models = tdeckgemini::modelCount();
    if (models != lastModelCount) {
        lastModelCount = models;
        refreshModelLabel();
    }
    const int st = tdeckgemini::state();
    if (st != lastState) {
        lastState = st;
        switch (st) {
    case tdeckgemini::WORKING:
        lv_label_set_text(askBtnLbl, "...");
        setStatus(tdeckgemini::statusText(), 0x0a84ff);
        break;
    case tdeckgemini::DONE:
        lv_label_set_text(askBtnLbl, "> ");
        renderConversation();
        setStatus(tdeckgemini::statusText(), tdeckgemini::statusText()[0] ? 0x30d158 : 0xffffff);
        break;
    case tdeckgemini::FAILED:
        lv_label_set_text(askBtnLbl, "> ");
        setStatus(tdeckgemini::statusText(), 0xff453a);
        break;
    default:
        lv_label_set_text(askBtnLbl, "> ");
        break;
        }
    }
}

const char *calendarMonthName(int month)
{
    static const char *names[] = {"", "Januar", "Februar", "März", "April", "Mai", "Juni", "Juli", "August",
                                  "September", "Oktober", "November", "Dezember"};
    return (month >= 1 && month <= 12) ? names[month] : "Kalender";
}

int calendarDaysInMonth(int year, int month)
{
    struct tm last = {};
    last.tm_year = year - 1900;
    last.tm_mon = month; // day zero of the next month = last day of this month
    last.tm_mday = 0;
    mktime(&last);
    return last.tm_mday;
}

int calendarFirstColumn(int year, int month)
{
    struct tm first = {};
    first.tm_year = year - 1900;
    first.tm_mon = month - 1;
    first.tm_mday = 1;
    mktime(&first);
    // tm_wday is Sunday=0; the T-Deck calendar starts on Monday.
    return (first.tm_wday + 6) % 7;
}

void renderCalendar();

void calendarResetDayState()
{
    calendarMarked = false;
    calendarAlarm = false;
    snprintf(calendarAlarmTime, sizeof(calendarAlarmTime), "08:00");
    calendarNote[0] = 0;
}

void calendarDayPath(char *path, size_t pathSize)
{
    if (!path || pathSize == 0 || calendarSelectedDay <= 0) {
        if (path && pathSize)
            path[0] = 0;
        return;
    }
    snprintf(path, pathSize, "/calendar/days/%04d-%02d-%02d.txt", calendarYear, calendarMonth,
             calendarSelectedDay);
}

void calendarLoadDayState()
{
    calendarResetDayState();
    char path[64];
    calendarDayPath(path, sizeof(path));
    if (!path[0] || !SDFs.exists(path))
        return;

    FsFile f = SDFs.open(path, O_RDONLY);
    if (!f)
        return;
    char line[192] = {};
    while (f.fgets(line, sizeof(line)) > 0) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        if (!strncmp(line, "MARKED=", 7))
            calendarMarked = line[7] == '1';
        else if (!strncmp(line, "ALARM=", 6))
            calendarAlarm = line[6] == '1';
        else if (!strncmp(line, "TIME=", 5))
            snprintf(calendarAlarmTime, sizeof(calendarAlarmTime), "%.5s", line + 5);
        else if (!strncmp(line, "NOTE=", 5))
            snprintf(calendarNote, sizeof(calendarNote), "%.160s", line + 5);
    }
    f.close();
}

void calendarUpdateDetails()
{
    if (!calendarDateLabel || !calendarNoteArea || !calendarAlarmTimeArea)
        return;
    if (calendarSelectedDay <= 0) {
        lv_label_set_text(calendarDateLabel, "Tag zum Bearbeiten wählen");
        lv_textarea_set_text(calendarNoteArea, "");
        lv_textarea_set_text(calendarAlarmTimeArea, calendarAlarmTime);
    } else {
        char date[48];
        snprintf(date, sizeof(date), "%02d %s %04d", calendarSelectedDay, calendarMonthName(calendarMonth),
                 calendarYear);
        lv_label_set_text(calendarDateLabel, date);
        lv_textarea_set_text(calendarNoteArea, calendarNote);
        lv_textarea_set_text(calendarAlarmTimeArea, calendarAlarmTime);
    }
    if (calendarMarkLabel)
        lv_label_set_text(calendarMarkLabel, calendarMarked ? "Markiert" : "Tag markieren");
    if (calendarAlarmLabel)
        lv_label_set_text(calendarAlarmLabel, calendarAlarm ? "Wecker an" : "Wecker aus");
}

void calendarSaveDay(lv_event_t *)
{
    if (calendarSelectedDay <= 0 || !calendarNoteArea || !calendarAlarmTimeArea) {
        if (calendarStatus)
            lv_label_set_text(calendarStatus, "Bitte zuerst einen Tag wählen");
        return;
    }
    snprintf(calendarNote, sizeof(calendarNote), "%.160s", lv_textarea_get_text(calendarNoteArea));
    snprintf(calendarAlarmTime, sizeof(calendarAlarmTime), "%.5s", lv_textarea_get_text(calendarAlarmTimeArea));
    SDFs.mkdir("/calendar");
    SDFs.mkdir("/calendar/days");
    char path[64];
    calendarDayPath(path, sizeof(path));
    FsFile f = SDFs.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) {
        lv_label_set_text(calendarStatus, "Tag konnte nicht gespeichert werden");
        return;
    }
    f.print("MARKED=");
    f.println(calendarMarked ? 1 : 0);
    f.print("ALARM=");
    f.println(calendarAlarm ? 1 : 0);
    f.print("TIME=");
    f.println(calendarAlarmTime);
    f.print("NOTE=");
    // Keep the tiny day file line-based. Newlines are flattened instead of corrupting the
    // following fields, while the on-device text area still feels like a normal note box.
    const char *note = calendarNote;
    while (*note) {
        const char c = *note++;
        f.write((uint8_t)((c == '\r' || c == '\n') ? ' ' : c));
    }
    f.println();
    f.close();
    lv_label_set_text(calendarStatus, "Tag auf SD-Karte gespeichert");
}

void calendarToggleMark(lv_event_t *)
{
    if (calendarSelectedDay <= 0)
        return;
    calendarMarked = !calendarMarked;
    calendarUpdateDetails();
    renderCalendar();
}

void calendarToggleAlarm(lv_event_t *)
{
    if (calendarSelectedDay <= 0)
        return;
    calendarAlarm = !calendarAlarm;
    calendarUpdateDetails();
}

void calendarSelectDay(int day)
{
    calendarSelectedDay = day;
    calendarLoadDayState();
    calendarUpdateDetails();
    renderCalendar();
    // The day controls are deliberately below the month grid. Bring them into view as soon as
    // a day is chosen; the user can still drag back up to the grid at any time.
    if (calendarScroll)
        lv_obj_scroll_to_y(calendarScroll, 220, LV_ANIM_ON);
}

void calendarPrevious(lv_event_t *)
{
    if (--calendarMonth < 1) {
        calendarMonth = 12;
        calendarYear--;
    }
    calendarSelectedDay = 0;
    calendarResetDayState();
    renderCalendar();
    calendarUpdateDetails();
    if (calendarScroll)
        lv_obj_scroll_to_y(calendarScroll, 0, LV_ANIM_ON);
}

void calendarNext(lv_event_t *)
{
    if (++calendarMonth > 12) {
        calendarMonth = 1;
        calendarYear++;
    }
    calendarSelectedDay = 0;
    calendarResetDayState();
    renderCalendar();
    calendarUpdateDetails();
    if (calendarScroll)
        lv_obj_scroll_to_y(calendarScroll, 0, LV_ANIM_ON);
}

void calendarOpenAgent(lv_event_t *)
{
    if (tdeckgemini::state() == tdeckgemini::WORKING)
        return;
    destroyUiOnly();
    ollama_open();
    tdeckgemini::setMode(tdeckgemini::MODE_CALENDAR);
    refreshModeLabel();
    setStatus("Beschreibe den Termin", 0x30d158);
}

void renderCalendar()
{
    if (!screen || !calendarScreen)
        return;
    if (calendarGrid) {
        lv_obj_delete(calendarGrid);
        calendarGrid = nullptr;
    }
    char title[48];
    snprintf(title, sizeof(title), "%s %d", calendarMonthName(calendarMonth), calendarYear);
    lv_label_set_text(calendarTitle, title);

    calendarGrid = lv_obj_create(calendarContent);
    lv_obj_set_pos(calendarGrid, 4, 48);
    lv_obj_set_size(calendarGrid, 312, 162);
    lv_obj_set_style_bg_color(calendarGrid, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(calendarGrid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(calendarGrid, 0, LV_PART_MAIN);
    lv_obj_clear_flag(calendarGrid, LV_OBJ_FLAG_SCROLLABLE);

    const int first = calendarFirstColumn(calendarYear, calendarMonth);
    const int count = calendarDaysInMonth(calendarYear, calendarMonth);
    time_t now = time(nullptr);
    struct tm today = {};
    localtime_r(&now, &today);
    const bool validToday = today.tm_year >= 120;
    const int todayYear = today.tm_year + 1900;
    const int todayMonth = today.tm_mon + 1;
    const int todayDay = today.tm_mday;

    for (int day = 1; day <= count; day++) {
        const int index = first + day - 1;
        const int row = index / 7;
        const int col = index % 7;
        const bool isToday = validToday && todayYear == calendarYear && todayMonth == calendarMonth && todayDay == day;
        lv_obj_t *button = lv_button_create(calendarGrid);
        lv_obj_set_pos(button, col * 44, row * 27);
        lv_obj_set_size(button, 42, 25);
        lv_obj_set_style_radius(button, 6, LV_PART_MAIN);
        const bool selected = calendarSelectedDay == day;
        lv_obj_set_style_bg_color(button, lv_color_hex(selected ? 0x0a84ff : (isToday ? 0x30d158 : (col >= 5 ? 0x30252a : 0x2c2c2e))), LV_PART_MAIN);
        lv_obj_set_style_text_color(button, lv_color_hex(selected || isToday ? 0x000000 : 0xffffff), LV_PART_MAIN);
        char number[4];
        snprintf(number, sizeof(number), "%d", day);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, number);
        lv_obj_center(label);
        lv_obj_add_event_cb(
            button,
            [](lv_event_t *event) {
                const int selected = (int)(intptr_t)lv_event_get_user_data(event);
                calendarSelectDay(selected);
            },
            LV_EVENT_CLICKED, (void *)(intptr_t)day);
        if (lv_group_get_default())
            lv_group_add_obj(lv_group_get_default(), button);
    }
}

void showCalendarScreen()
{
    // The calendar is a separate launcher app. It must never leave Ollama's
    // conversation mode set to Calendar for the next visit to Ollama.
    tdeckgemini::setMode(tdeckgemini::MODE_CHAT);
    if (screen && !calendarScreen)
        destroyUiOnly();
    if (screen && calendarScreen) {
        renderCalendar();
        lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        return;
    }

    time_t now = time(nullptr);
    struct tm current = {};
    localtime_r(&now, &current);
    calendarYear = current.tm_year >= 120 ? current.tm_year + 1900 : 2026;
    calendarMonth = current.tm_year >= 120 ? current.tm_mon + 1 : 1;
    calendarSelectedDay = 0;
    calendarResetDayState();
    calendarScreen = true;
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    tui_statusbar_reserve(screen);

    calendarScroll = lv_obj_create(screen);
    lv_obj_set_pos(calendarScroll, 0, 0);
    lv_obj_set_size(calendarScroll, 320, 220);
    lv_obj_set_style_bg_color(calendarScroll, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(calendarScroll, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(calendarScroll, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(calendarScroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(calendarScroll, LV_SCROLLBAR_MODE_AUTO);

    calendarContent = lv_obj_create(calendarScroll);
    lv_obj_set_pos(calendarContent, 0, 0);
    lv_obj_set_size(calendarContent, 320, 470);
    lv_obj_set_style_bg_color(calendarContent, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(calendarContent, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(calendarContent, 0, LV_PART_MAIN);
    lv_obj_clear_flag(calendarContent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *previous = makeButton(calendarContent, "<", 0x2c2c2e, calendarPrevious, nullptr);
    lv_obj_set_pos(previous, 4, 0);
    lv_obj_set_size(previous, 34, 28);
    calendarTitle = lv_label_create(calendarContent);
    lv_obj_set_pos(calendarTitle, 42, 4);
    lv_obj_set_size(calendarTitle, 192, 22);
    lv_obj_set_style_text_color(calendarTitle, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_align(calendarTitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_t *next = makeButton(calendarContent, ">", 0x2c2c2e, calendarNext, nullptr);
    lv_obj_set_pos(next, 238, 0);
    lv_obj_set_size(next, 34, 28);
    lv_obj_t *agent = makeButton(calendarContent, "AI", 0x30d158, calendarOpenAgent, nullptr);
    lv_obj_set_pos(agent, 276, 0);
    lv_obj_set_size(agent, 40, 28);

    lv_obj_t *weekdays = lv_label_create(calendarContent);
    lv_obj_set_pos(weekdays, 8, 31);
    lv_obj_set_size(weekdays, 304, 16);
    lv_label_set_text(weekdays, "Mo       Di       Mi       Do       Fr       Sa       So");
    lv_obj_set_style_text_color(weekdays, lv_color_hex(0x8e8e93), LV_PART_MAIN);

    calendarStatus = lv_label_create(calendarContent);
    lv_obj_set_pos(calendarStatus, 8, 214);
    lv_obj_set_size(calendarStatus, 304, 18);
    lv_label_set_text(calendarStatus, "Tag antippen · nach unten für Details");
    lv_obj_set_style_text_color(calendarStatus, lv_color_hex(0xc7c7cc), LV_PART_MAIN);

    calendarDetail = lv_obj_create(calendarContent);
    lv_obj_set_pos(calendarDetail, 4, 240);
    lv_obj_set_size(calendarDetail, 312, 218);
    lv_obj_set_style_bg_color(calendarDetail, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_border_width(calendarDetail, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(calendarDetail, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
    lv_obj_set_style_radius(calendarDetail, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(calendarDetail, 6, LV_PART_MAIN);
    lv_obj_clear_flag(calendarDetail, LV_OBJ_FLAG_SCROLLABLE);

    calendarDateLabel = lv_label_create(calendarDetail);
    lv_obj_set_pos(calendarDateLabel, 0, 0);
    lv_obj_set_size(calendarDateLabel, 180, 20);
    lv_obj_set_style_text_color(calendarDateLabel, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(calendarDateLabel, &ui_font_montserrat_12, LV_PART_MAIN);

    lv_obj_t *mark = makeButton(calendarDetail, "Tag markieren", 0x2c2c2e, calendarToggleMark, &calendarMarkLabel);
    lv_obj_set_pos(mark, 188, 0);
    lv_obj_set_size(mark, 104, 25);

    calendarNoteArea = lv_textarea_create(calendarDetail);
    lv_textarea_set_one_line(calendarNoteArea, false);
    lv_textarea_set_max_length(calendarNoteArea, 160);
    lv_textarea_set_placeholder_text(calendarNoteArea, "Notiz für diesen Tag …");
    lv_obj_set_pos(calendarNoteArea, 0, 29);
    lv_obj_set_size(calendarNoteArea, 292, 54);
    lv_obj_set_style_bg_color(calendarNoteArea, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
    lv_obj_set_style_text_color(calendarNoteArea, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_border_width(calendarNoteArea, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(calendarNoteArea, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(calendarNoteArea, 4, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(calendarNoteArea, 0, LV_PART_CURSOR);
    lv_anim_delete(calendarNoteArea, nullptr);
    if (lv_group_get_default())
        lv_group_add_obj(lv_group_get_default(), calendarNoteArea);

    lv_obj_t *alarm = makeButton(calendarDetail, "Wecker aus", 0x2c2c2e, calendarToggleAlarm, &calendarAlarmLabel);
    lv_obj_set_pos(alarm, 0, 91);
    lv_obj_set_size(alarm, 104, 27);

    calendarAlarmTimeArea = lv_textarea_create(calendarDetail);
    lv_textarea_set_one_line(calendarAlarmTimeArea, true);
    lv_textarea_set_max_length(calendarAlarmTimeArea, 5);
    lv_textarea_set_placeholder_text(calendarAlarmTimeArea, "HH:MM");
    lv_obj_set_pos(calendarAlarmTimeArea, 112, 91);
    lv_obj_set_size(calendarAlarmTimeArea, 84, 27);
    lv_obj_set_style_bg_color(calendarAlarmTimeArea, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
    lv_obj_set_style_text_color(calendarAlarmTimeArea, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_border_width(calendarAlarmTimeArea, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(calendarAlarmTimeArea, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(calendarAlarmTimeArea, 3, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(calendarAlarmTimeArea, 0, LV_PART_CURSOR);
    lv_anim_delete(calendarAlarmTimeArea, nullptr);
    if (lv_group_get_default())
        lv_group_add_obj(lv_group_get_default(), calendarAlarmTimeArea);

    lv_obj_t *save = makeButton(calendarDetail, "Tag speichern", 0x30d158, calendarSaveDay, nullptr);
    lv_obj_set_pos(save, 204, 91);
    lv_obj_set_size(save, 88, 27);

    lv_obj_t *info = lv_label_create(calendarDetail);
    lv_obj_set_pos(info, 0, 126);
    lv_obj_set_size(info, 292, 48);
    lv_label_set_text(info, "Markierung, Notiz und Wecker werden\nfür diesen Tag auf der SD-Karte gespeichert.");
    lv_obj_set_style_text_color(info, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_set_style_text_font(info, &ui_font_montserrat_12, LV_PART_MAIN);

    calendarUpdateDetails();
    renderCalendar();
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}
} // namespace

// ⭐ GIVE THE MEMORY BACK WHEN THE APP IS NOT IN USE. Jake: "preferably Gemini closes and
// clears ram when not running". The textarea, buttons, scrollable answer box and two timers are
// INTERNAL RAM - the scarcest thing here - and were being held for the rest of the session after
// a single visit. gemini_open() already rebuilds everything from scratch, so there is nothing to
// preserve.
//
// ⛔ NOT WHILE A REQUEST IS IN FLIGHT. Deleting the screen mid-request would destroy the label
// the reply is about to be written into. Five seconds of grace as well, so paging past Gemini
// on the launcher does not tear it down and immediately rebuild it.
extern "C" void ollama_idle_check(void)
{
    static uint32_t idleSince = 0;
    if (!screen)
        return;
    if (lv_screen_active() == screen || tdeckgemini::state() == tdeckgemini::WORKING) {
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
    destroyUiOnly();
    tdeckgemini::release();
}

extern "C" void ollama_open(void)
{
    // Every fresh entry into Ollama starts as a normal chat. Calendar has its
    // own screen and must not leak its mode or its LVGL screen into this app.
    //
    // The launcher intentionally keeps this app alive for a short grace period
    // after leaving it, so a quick return does not lose the conversation. That
    // is useful for Ollama, but it used to make a just-visited calendar screen
    // get reused here. A calendar page is not a chat page: discard it before
    // deciding whether the existing screen can be reused.
    if (screen && calendarScreen)
        destroyUiOnly();
    tdeckgemini::setMode(tdeckgemini::MODE_CHAT);
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        tui_statusbar_reserve(screen); // the shared top bar, which also makes room for itself

        // Keep the main surface quiet: only the menu button remains above the conversation.
        // The app name, model, mode and transient status all live inside the menu.
        headerModelLbl = lv_label_create(screen);
        lv_obj_set_pos(headerModelLbl, 6, 5);
        lv_obj_set_width(headerModelLbl, 154);
        lv_label_set_long_mode(headerModelLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(headerModelLbl, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(headerModelLbl, lv_color_hex(0xc7c7cc), LV_PART_MAIN);
        lv_label_set_text(headerModelLbl, "Ollama");
        lv_obj_t *headerSlashLbl = lv_label_create(screen);
        lv_obj_set_pos(headerSlashLbl, 164, 5);
        lv_obj_set_style_text_font(headerSlashLbl, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(headerSlashLbl, lv_color_hex(0x636366), LV_PART_MAIN);
        lv_label_set_text(headerSlashLbl, "/");
        headerModeLbl = lv_label_create(screen);
        lv_obj_set_pos(headerModeLbl, 178, 5);
        lv_obj_set_width(headerModeLbl, 104);
        lv_label_set_long_mode(headerModeLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(headerModeLbl, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(headerModeLbl, lv_color_hex(0x30d158), LV_PART_MAIN);
            lv_label_set_text(headerModeLbl, "Chat");
        lv_obj_t *menuBtn = makeButton(screen, "", 0x2c2c2e, toggleMenu, nullptr);
        lv_obj_set_size(menuBtn, 28, 28);
        lv_obj_set_pos(menuBtn, 288, 0);
        addHamburgerIcon(menuBtn);

        // A multi-line prompt with room for a complete instruction before the user sends it.
        askArea = lv_textarea_create(screen);
        lv_obj_set_pos(askArea, 4, 184);
        lv_obj_set_size(askArea, 252, 48);
        lv_obj_set_style_bg_color(askArea, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_text_color(askArea, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_border_width(askArea, 0, LV_PART_MAIN);
        lv_textarea_set_max_length(askArea, 4096);
        lv_textarea_set_text_selection(askArea, true);
        lv_textarea_set_placeholder_text(askArea, "Nachricht schreiben …");
        // Solid non-blinking caret, for the same reason NotesApp uses one: a blinking thin line
        // only repaints when something else changes, so it looks like there is no cursor at all.
        lv_obj_set_style_bg_color(askArea, lv_color_hex(0xffffff), LV_PART_CURSOR);
        lv_obj_set_style_bg_opa(askArea, LV_OPA_50, LV_PART_CURSOR);
        lv_obj_set_style_anim_duration(askArea, 0, LV_PART_CURSOR);
        // ⛔ AND DELETE THE ANIMATION THAT ALREADY EXISTS. lv_textarea_create() starts a
        // blinking cursor during construction using the DEFAULT time, so setting the duration to
        // zero above does not stop it - start_cursor_blink only re-reads that on focus, or on a
        // style change delivered to the label child. An active LVGL animation then forces a
        // refresh EVERY FRAME: measured 17-18 fps and 124-280 KB/s pushed while sitting idle,
        // against 2 fps once it is gone. Any later focus re-runs the check, finds the zero and
        // deletes it itself, so this one call is all that is needed.
        lv_anim_delete(askArea, nullptr);
        if (lv_group_get_default())
            lv_group_add_obj(lv_group_get_default(), askArea);
        // Enter inserts a newline; the blue arrow is the explicit send action.

        // The first keystroke after sending throws the old question away rather than appending to
        // it. Checked on VALUE_CHANGED because that is the only event that fires for a character
        // arriving from the keyboard, the trackball or a paste alike.
        lv_obj_add_event_cb(
            askArea,
            [](lv_event_t *) {
                if (!s_sentText || !askArea)
                    return;
                s_sentText = false;
                lv_obj_set_style_text_color(askArea, lv_color_hex(0xffffff), LV_PART_MAIN);
                // Keep only what was just typed: everything before it belonged to the old
                // question. One character in practice, but a paste is handled the same way.
                const char *t = lv_textarea_get_text(askArea);
                const size_t keep = t ? strlen(t) : 0;
                if (keep > s_sentLen && s_sentLen > 0) {
                    char *tail = strdup(t + s_sentLen);
                    if (tail) {
                        lv_textarea_set_text(askArea, tail);
                        free(tail);
                    }
                }
                s_sentLen = 0;
            },
            LV_EVENT_VALUE_CHANGED, NULL);

        // The field scrolls vertically while composing; the visible portion stays compact.
        lv_textarea_set_one_line(askArea, false);

        voiceBtn = makeButton(screen, "Mic", 0x30a46c, onVoiceButton, &voiceBtnLbl);
        lv_obj_set_size(voiceBtn, 40, 32);
        lv_obj_set_pos(voiceBtn, 212, 195);

        helpBox = lv_obj_create(screen);
        lv_obj_set_pos(helpBox, 0, 0);
        lv_obj_set_size(helpBox, 320, 218);
        lv_obj_set_style_bg_color(helpBox, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_border_width(helpBox, 0, LV_PART_MAIN);
        lv_obj_add_flag(helpBox, LV_OBJ_FLAG_HIDDEN);
        {
            lv_obj_t *h = lv_label_create(helpBox);
            lv_label_set_text(h, "Ollama-API-Schlüssel einrichten");
            lv_obj_set_style_text_color(h, lv_color_hex(0xffffff), LV_PART_MAIN);
            lv_obj_set_pos(h, 4, 2);

            lv_obj_t *b = lv_label_create(helpBox);
            lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(b, 296);      // ⛔ WRAP NEEDS AN EXPLICIT WIDTH AND HEIGHT or it
            lv_obj_set_height(b, 150);     //    draws straight over the button below it
            lv_obj_set_pos(b, 4, 24);
            lv_label_set_text(b, "Ollama-Agent: /ollama.txt\n"
                                 "key=OLLAMA_API_KEY\n"
                                 "model=Modellname (optional)\n"
                                 "\n"
                                 "Spracheingabe: /deepgram.txt\n"
                                 "key=DEEPGRAM_API_KEY\n"
                                 "model=nova-3 (optional)\n"
                                 "language=de (optional)\n"
                                 "\n"
                                 "Getrennte Dateien und Schlüssel.\n"
                                 "Beide bleiben auf der SD-Karte.");
            lv_obj_set_style_text_color(b, lv_color_hex(0xc7c7cc), LV_PART_MAIN);

            lv_obj_t *ok = makeButton(helpBox, "Schließen", 0x3a3a3c, [](lv_event_t *) {
                if (helpBox)
                    lv_obj_add_flag(helpBox, LV_OBJ_FLAG_HIDDEN);
            }, nullptr);
            lv_obj_set_size(ok, 100, 30);
            lv_obj_set_pos(ok, 108, 182);
        }

        menuBox = lv_obj_create(screen);
        lv_obj_set_pos(menuBox, 72, 22);
        lv_obj_set_size(menuBox, 244, 210);
        lv_obj_set_style_bg_color(menuBox, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_border_width(menuBox, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(menuBox, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
        lv_obj_set_style_radius(menuBox, 8, LV_PART_MAIN);
        lv_obj_add_flag(menuBox, LV_OBJ_FLAG_HIDDEN);
        {
            lv_obj_t *menuTitle = lv_label_create(menuBox);
            lv_label_set_text(menuTitle, "Menü");
            lv_obj_set_pos(menuTitle, 10, 6);
            lv_obj_set_style_text_color(menuTitle, lv_color_hex(0xffffff), LV_PART_MAIN);
            lv_obj_t *voiceToggle = makeButton(menuBox, "", 0x2c2c2e, toggleVoiceEnabled, &voiceToggleLbl);
            lv_obj_set_size(voiceToggle, 116, 24);
            lv_obj_set_pos(voiceToggle, 116, 2);

            lv_obj_t *modeBtn = makeButton(menuBox, "Chat-Modus", 0x2c2c2e, chooseMode, &modeBtnLbl);
            lv_obj_set_size(modeBtn, 220, 28);
            lv_obj_set_pos(modeBtn, 10, 26);
            lv_obj_t *modelBtn = makeButton(menuBox, "Modell", 0x2c2c2e, chooseModel, &modelBtnLbl);
            lv_obj_set_size(modelBtn, 220, 28);
            lv_obj_set_pos(modelBtn, 10, 58);

            lv_obj_t *saveBtn = makeButton(menuBox, "Speichern", 0x30d158, doSaveNote, nullptr);
            lv_obj_set_size(saveBtn, 106, 28);
            lv_obj_set_pos(saveBtn, 10, 90);
            lv_obj_t *deleteBtn = makeButton(menuBox, "Löschen", 0x3a3a3c, onClear, nullptr);
            lv_obj_set_size(deleteBtn, 106, 28);
            lv_obj_set_pos(deleteBtn, 124, 90);

            lv_obj_t *helpBtn = makeButton(menuBox, "API-Hilfe", 0x2c2c2e, [](lv_event_t *) {
                closeMenu();
                if (helpBox) {
                    lv_obj_clear_flag(helpBox, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_move_foreground(helpBox);
                }
            }, nullptr);
            lv_obj_set_size(helpBtn, 106, 28);
            lv_obj_set_pos(helpBtn, 10, 122);
            lv_obj_t *closeBtn = makeButton(menuBox, "Schließen", 0x2c2c2e, [](lv_event_t *) { closeMenu(); }, nullptr);
            lv_obj_set_size(closeBtn, 106, 28);
            lv_obj_set_pos(closeBtn, 124, 122);

            lv_obj_t *copyReplyBtn = makeButton(menuBox, "Antwort kopieren", 0x2c2c2e,
                                                [](lv_event_t *) {
                                                    const bool copied = tdeckclipboard::copyText(tdeckgemini::reply());
                                                    setStatus(copied ? "Antwort kopiert" : "Keine Antwort zum Kopieren",
                                                              copied ? 0x30d158 : 0xff9f0a);
                                                }, nullptr);
            lv_obj_set_size(copyReplyBtn, 220, 20);
            lv_obj_set_pos(copyReplyBtn, 10, 148);

            lv_obj_t *zoomTitle = lv_label_create(menuBox);
            lv_label_set_text(zoomTitle, "Schriftgröße");
            lv_obj_set_pos(zoomTitle, 10, 176);
            lv_obj_set_style_text_color(zoomTitle, lv_color_hex(0xc7c7cc), LV_PART_MAIN);
            lv_obj_t *zoomMinus = makeButton(menuBox, "−", 0x2c2c2e, zoomChatOut, nullptr);
            lv_obj_set_size(zoomMinus, 40, 24);
            lv_obj_set_pos(zoomMinus, 132, 170);
            lv_obj_t *zoomPlus = makeButton(menuBox, "+", 0x2c2c2e, zoomChatIn, nullptr);
            lv_obj_set_size(zoomPlus, 40, 24);
            lv_obj_set_pos(zoomPlus, 176, 170);

            statusLbl = lv_label_create(menuBox);
            lv_obj_set_pos(statusLbl, 10, 196);
            lv_obj_set_width(statusLbl, 220);
            lv_label_set_long_mode(statusLbl, LV_LABEL_LONG_DOT);
            lv_obj_set_height(statusLbl, 14);
            lv_obj_set_style_text_color(statusLbl, lv_color_hex(0xc7c7cc), LV_PART_MAIN);
            lv_label_set_text(statusLbl, "Bereit");
        }
        refreshVoiceControls();

        // The conversation is the main surface: every completed turn is rendered here and the
        // box scrolls upward as the chat grows.
        chatBox = lv_obj_create(screen);
        lv_obj_set_pos(chatBox, 2, 28);
        lv_obj_set_size(chatBox, 316, 154);
        lv_obj_set_style_bg_color(chatBox, lv_color_hex(0x121214), LV_PART_MAIN);
        lv_obj_set_style_border_width(chatBox, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(chatBox, 6, LV_PART_MAIN);
        lv_obj_set_scrollbar_mode(chatBox, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(chatBox, LV_DIR_VER);
        answerLbl = lv_label_create(chatBox);
        lv_obj_set_width(answerLbl, 296);
        lv_label_set_long_mode(answerLbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(answerLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_label_set_text(answerLbl, "");
        applyChatZoom();

        lv_obj_t *sendBtn = makeButton(screen, ">", 0x0a84ff, doAsk, &askBtnLbl);
        lv_obj_set_size(sendBtn, 56, 32);
        lv_obj_set_pos(sendBtn, 260, 192);
        lv_obj_set_pos(askArea, 4, 184);

        pollTimer = lv_timer_create(tick, 250, NULL);
        // Keep the physical keyboard aimed at the question box. Same guard NotesApp needs:
        // anything that touches the focus group steals the keys otherwise.
        focusGuard = lv_timer_create(
            [](lv_timer_t *) {
                lv_group_t *g = lv_group_get_default();
                if (g && askArea && lv_group_get_focused(g) != askArea)
                    lv_group_focus_obj(askArea);
            },
            300, NULL);
    }

    lastState = -1; // force the next tick to repaint whatever state we are resuming into
    lastModelCount = -1;
    if (pollTimer)
        lv_timer_resume(pollTimer);
    if (focusGuard)
        lv_timer_resume(focusGuard);
    tdeckgemini::restoreSession();
    refreshModeLabel();
    refreshModelLabel();
    renderConversation();
    tdeckgemini::refreshModels();
    if (!tdeckgemini::haveConfig())
        setStatus("API-Schlüssel fehlt: in /ollama.txt auf der SD-Karte eintragen", 0xff9f0a);
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// The launcher has a real month grid. The AI button opens the existing Ollama calendar mode
// for natural-language event creation; the visual calendar itself works fully offline.
extern "C" void calendar_open(void)
{
    if (tdeckgemini::state() == tdeckgemini::WORKING)
        return;
    tdeckgemini::setMode(tdeckgemini::MODE_CALENDAR);
    showCalendarScreen();
}
