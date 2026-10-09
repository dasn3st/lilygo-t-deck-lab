// -----------------------------------------------------------------------------
// T-UI Notes — a self-contained notes app for the launcher.
//
// Notes are plain .txt files in /notes on the SD card (created on first save;
// nothing is ever scanned outside that one small folder). Two screens:
//   • List — every note, newest first, previewed by its first line. "+ New"
//     starts a fresh note; tapping a row opens it.
//   • Editor — a full-screen text box typed into with the physical keyboard.
//     Leaving the screen through the persistent top bar SAVES and returns to the launcher
//     (a brand-new empty note is discarded);
//     Delete removes the note for good.
// The textarea is focused via LVGL's default input group (same way the mesh
// chat input gets typed into), with a periodic refocus guard because the
// trackball encoder can wander focus. Trackball double-click = Home still works.
// -----------------------------------------------------------------------------
#include "graphics/view/TFT/TuiStatusBar.h" // the persistent top bar
#include "graphics/view/TFT/TuiLabel.h" // tui_one_line: LONG_DOT needs a height
#include "lvgl.h"
#include "TDeckClipboard.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if HAS_SDCARD && !HAS_SD_MMC && !ARCH_PORTDUINO
#include "graphics/common/SdCard.h" // SdFat instance (SDFs) shared with maps/files
#define NOTES_HAVE_SD 1
#include <esp_heap_caps.h> // the note scratch buffer lives in PSRAM, not internal RAM
#else
#define NOTES_HAVE_SD 0
#endif

extern "C" void notes_open(void);
extern "C" void notes_open_file(const char *path); // Files app: open any .txt in the editor
extern "C" void tdeck_open_note_download(const char *path);
namespace tdeckvoice
{
void setTarget(lv_obj_t *target);
bool startRecording();
bool voiceEnabled();
void setVoiceEnabled(bool enabled);
uint32_t recordingElapsedSeconds();
uint32_t recordingRemainingSeconds();
int state();
const char *statusText();
} // namespace tdeckvoice

namespace
{
const int kMaxNotes = 40;
const size_t kMaxNoteBytes = 4000;

lv_obj_t *listScreen = nullptr;
lv_obj_t *listPanel = nullptr; // scrollable column of note rows
lv_obj_t *editScreen = nullptr;
lv_obj_t *editArea = nullptr;  // the textarea (body)
lv_obj_t *titleArea = nullptr; // one-line editable title, top-middle (was a "type away" hint)
lv_obj_t *editHint = nullptr;  // replaces the title box for view-only files
lv_obj_t *confirmPanel = nullptr; // "Delete this note?" overlay
lv_obj_t *focusTarget = nullptr;  // which box the keyboard is aimed at (title or body)
lv_obj_t *favoriteBtn = nullptr;
lv_obj_t *micBtn = nullptr;
lv_obj_t *micBtnLabel = nullptr;
lv_obj_t *micStatusLabel = nullptr;
lv_obj_t *notesMenu = nullptr;
lv_obj_t *voiceToggleLabel = nullptr;
lv_obj_t *notesMenuDelete = nullptr;
lv_obj_t *notesMenuShare = nullptr;
lv_obj_t *notesMenuFavorite = nullptr;
lv_obj_t *selectionMenu = nullptr;
lv_timer_t *selectionMenuTimer = nullptr;
lv_obj_t *selectionTarget = nullptr;
bool noteMicFlow = false;
lv_obj_t *prevScreen = nullptr;
lv_timer_t *focusGuard = nullptr;
lv_timer_t *micButtonTimer = nullptr;

unsigned noteIds[kMaxNotes];
// ⛔ PSRAM. 1,440 bytes of internal RAM for preview strings shown only on the Notes list.
// A helper rather than a raw array so every use goes through the allocation check.
static char (*notePreviewBuf)[36] = nullptr;

static char (*notePreviewArr(void))[36]
{
    if (!notePreviewBuf) {
        notePreviewBuf = (char (*)[36])heap_caps_calloc(kMaxNotes, 36, MALLOC_CAP_SPIRAM);
        if (!notePreviewBuf)
            notePreviewBuf = (char (*)[36])calloc(kMaxNotes, 36);
    }
    return notePreviewBuf;
}
#define notePreview (notePreviewArr())
int noteCount = 0;
unsigned curId = 0;        // note being edited (list mode)
bool curIsNew = false;     // brand-new note: discard if left empty
char curPath[96] = {};     // file behind the editor (works for any .txt, not just /notes)
bool curFromFiles = false; // opened via the Files app: Back returns there, no Delete
bool curReadOnly = false;  // file was too big to load fully: never save it back truncated
lv_obj_t *filesReturnScreen = nullptr;
bool skipUnloadSave = false;

void notePath(char *buf, size_t sz, unsigned id) { snprintf(buf, sz, "/notes/n%03u.txt", id); }

#if NOTES_HAVE_SD
const char *kNoteFavoritesPath = "/notes/.favorites";

bool noteIsFavorite(unsigned id)
{
    FsFile f = SDFs.open(kNoteFavoritesPath, O_RDONLY);
    if (!f)
        return false;
    char line[20] = {};
    bool found = false;
    while (f.fgets(line, sizeof(line)) > 0) {
        if ((unsigned)strtoul(line, nullptr, 10) == id) {
            found = true;
            break;
        }
    }
    f.close();
    return found;
}

void setNoteFavorite(unsigned id, bool on)
{
    if (!id)
        return;
    unsigned ids[80] = {};
    int count = 0;
    FsFile old = SDFs.open(kNoteFavoritesPath, O_RDONLY);
    if (old) {
        char line[20] = {};
        while (count < (int)(sizeof(ids) / sizeof(ids[0])) && old.fgets(line, sizeof(line)) > 0) {
            unsigned v = (unsigned)strtoul(line, nullptr, 10);
            if (v)
                ids[count++] = v;
        }
        old.close();
    }
    int at = -1;
    for (int i = 0; i < count; i++)
        if (ids[i] == id) {
            at = i;
            break;
        }
    if (on && at < 0 && count < (int)(sizeof(ids) / sizeof(ids[0])))
        ids[count++] = id;
    else if (!on && at >= 0) {
        for (int i = at + 1; i < count; i++)
            ids[i - 1] = ids[i];
        count--;
    }
    SDFs.mkdir("/notes");
    const char *tmp = "/notes/.favorites.tmp";
    FsFile out = SDFs.open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (!out)
        return;
    for (int i = 0; i < count; i++)
        out.println(ids[i]);
    out.close();
    SDFs.remove(kNoteFavoritesPath);
    SDFs.rename(tmp, kNoteFavoritesPath);
}
#else
bool noteIsFavorite(unsigned) { return false; }
void setNoteFavorite(unsigned, bool) {}
#endif

#if NOTES_HAVE_SD
// scan /notes (and only /notes) for n###.txt files; preview = first line
void scanNotes(void)
{
    noteCount = 0;
    FsFile dir = SDFs.open("/notes", O_RDONLY);
    if (!dir || !dir.isDirectory())
        return;
    FsFile entry;
    while ((entry = dir.openNextFile()) && noteCount < kMaxNotes) {
        char name[32];
        entry.getName(name, sizeof(name));
        unsigned id;
        if (!entry.isDirectory() && sscanf(name, "n%u.txt", &id) == 1) {
            char head[40] = {};
            int n = entry.read(head, sizeof(head) - 1);
            if (n < 0)
                n = 0;
            head[n] = 0;
            // First line = the note's title. If it was left blank, fall through to the first
            // line of the body so the row still says something useful.
            char *line = head;
            for (char *c = head; *c; c++)
                if (*c == '\r' || *c == '\n') {
                    bool blank = (c == line);
                    *c = 0;
                    if (!blank)
                        break;
                    line = c + 1; // skip the empty title and try the next line
                }
            noteIds[noteCount] = id;
            snprintf(notePreview[noteCount], sizeof(notePreview[0]), "%s%s", noteIsFavorite(id) ? "* " : "",
                     line[0] ? line : "(empty note)");
            noteCount++;
        }
        entry.close();
    }
    dir.close();
    // newest (highest id) first
    for (int i = 0; i < noteCount; i++)
        for (int j = i + 1; j < noteCount; j++)
            if (noteIds[j] > noteIds[i]) {
                unsigned t = noteIds[i];
                noteIds[i] = noteIds[j];
                noteIds[j] = t;
                char tmp[36];
                strcpy(tmp, notePreview[i]);
                strcpy(notePreview[i], notePreview[j]);
                strcpy(notePreview[j], tmp);
            }
}

unsigned nextFreeId(void)
{
    unsigned maxId = 0;
    for (int i = 0; i < noteCount; i++)
        if (noteIds[i] > maxId)
            maxId = noteIds[i];
    return maxId + 1;
}

// Scratch space for reading a note off the card. ONE buffer, shared by both loaders, and out
// in PSRAM: at 4KB apiece these are far too big for internal RAM, which on this device has been
// measured down to under 2KB free. Freed the moment the read is done.
char *readNote(void)
{
    char *buf = (char *)heap_caps_malloc(kMaxNoteBytes + 1, MALLOC_CAP_SPIRAM);
    if (!buf)
        return nullptr;
    buf[0] = 0;
    curReadOnly = false;
    FsFile f = SDFs.open(curPath, O_RDONLY);
    if (f) {
        curReadOnly = f.size() > kMaxNoteBytes; // too big: show the start, never save back
        int n = f.read(buf, kMaxNoteBytes);
        if (n < 0)
            n = 0;
        buf[n] = 0;
        f.close();
    }
    return buf;
}

// pull curPath into the title + body boxes
void loadCurPathIntoEditor(void)
{
    char *buf = readNote();
    if (!buf) {
        lv_textarea_set_text(titleArea, "");
        lv_textarea_set_text(editArea, "");
        return;
    }
    // The first line IS the title — it always was, in the sense that the list preview shows it.
    // Now it gets its own box at the top instead of sitting invisibly at the start of the body.
    char *nl = strchr(buf, '\n');
    if (nl) {
        *nl = 0;
        lv_textarea_set_text(titleArea, buf);
        lv_textarea_set_text(editArea, nl + 1);
    } else {
        lv_textarea_set_text(titleArea, buf);
        lv_textarea_set_text(editArea, "");
    }
    heap_caps_free(buf);
}

// Files app: the whole file goes in the body. Splitting a first line off an arbitrary .txt
// would silently move a line of somebody's file into a title box it can't even see.
void loadFileIntoEditor(void)
{
    char *buf = readNote();
    lv_textarea_set_text(editArea, buf ? buf : "");
    if (buf)
        heap_caps_free(buf);
}

void saveCurrentNote(void)
{
    if (curReadOnly)
        return; // never write a truncated copy over the original
    const char *text = lv_textarea_get_text(editArea);
    const char *title = titleArea ? lv_textarea_get_text(titleArea) : "";
    if (!text)
        text = "";
    if (!title)
        title = "";
    if (curIsNew && !*text && !*title)
        return; // never-typed-in new note: don't create a file
    if (!curFromFiles)
        SDFs.mkdir("/notes");
    FsFile f = SDFs.open(curPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f)
        return;
    if (curFromFiles) { // not a note: write back exactly what is in the body, nothing prepended
        f.print(text);
        f.close();
        return;
    }
    // Title goes back as the first line, which is exactly where it came from — so the file
    // stays a plain .txt anyone can read, and the list preview keeps working unchanged.
    f.print(title);
    f.print("\n");
    f.print(text);
    f.close();
}

void deleteCurrentNote(void)
{
    if (SDFs.exists(curPath))
        SDFs.remove(curPath);
    if (curId)
        setNoteFavorite(curId, false);
}
#else
void scanNotes(void) { noteCount = 0; }
unsigned nextFreeId(void) { return 1; }
void loadCurPathIntoEditor(void)
{
    lv_textarea_set_text(editArea, "");
    if (titleArea)
        lv_textarea_set_text(titleArea, "");
}
void loadFileIntoEditor(void) { lv_textarea_set_text(editArea, ""); }
void saveCurrentNote(void) {}
void deleteCurrentNote(void) {}
#endif

void updateFavoriteButton(void)
{
    if (!favoriteBtn || !curId)
        return;
    lv_obj_t *label = lv_obj_get_child(favoriteBtn, 0);
    if (label)
        lv_label_set_text(label, noteIsFavorite(curId) ? "*" : "-");
    lv_obj_set_style_bg_color(favoriteBtn, noteIsFavorite(curId) ? lv_color_hex(0xffd60a) : lv_color_hex(0x48484a),
                              LV_PART_MAIN);
}

void toggleCurrentFavorite(lv_event_t *)
{
    if (!curId)
        return;
    setNoteFavorite(curId, !noteIsFavorite(curId));
    updateFavoriteButton();
}

void refreshMicButton(lv_timer_t *)
{
    if (!micBtn || !micBtnLabel)
        return;
    const int state = tdeckvoice::state();
    const bool showResult = noteMicFlow && (state == 3 || state == 4) && tdeckvoice::statusText()[0];
    char recordingLabel[24];
    snprintf(recordingLabel, sizeof(recordingLabel), "Noch %02u:%02u",
             (unsigned)(tdeckvoice::recordingRemainingSeconds() / 60),
             (unsigned)(tdeckvoice::recordingRemainingSeconds() % 60));
    const char *label = state == 1 ? recordingLabel : state == 2 ? "Transkribiere ..." :
                        state == 4 && noteMicFlow ? "Mic Fehler" : "Mic starten";
    if (state != 1 && state != 2 && tdeckvoice::voiceEnabled())
        label = state == 4 && noteMicFlow ? "Mic Fehler" : "Mic starten";
    if (tdeckvoice::voiceEnabled() || state == 1 || state == 2)
        lv_obj_clear_flag(micBtn, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(micBtn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(micBtnLabel, label);
    lv_obj_set_style_bg_color(micBtn, lv_color_hex(state == 1 ? 0xff453a : state == 2 ? 0x0a84ff :
                                                   state == 4 && noteMicFlow ? 0xff453a : 0x30a46c),
                              LV_PART_MAIN);
    if (micStatusLabel) {
        if (showResult) {
            lv_label_set_text(micStatusLabel, tdeckvoice::statusText());
            lv_obj_set_style_text_color(micStatusLabel, lv_color_hex(state == 4 ? 0xff6961 : 0x30d158), LV_PART_MAIN);
            lv_obj_clear_flag(micStatusLabel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(editArea, 148);
        } else {
            lv_obj_add_flag(micStatusLabel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(editArea, 176);
        }
    }
    if (state == 2)
        lv_obj_add_state(micBtn, LV_STATE_DISABLED);
    else
        lv_obj_clear_state(micBtn, LV_STATE_DISABLED);
}

void refreshVoiceToggle()
{
    if (!voiceToggleLabel)
        return;
    lv_label_set_text(voiceToggleLabel, tdeckvoice::voiceEnabled() ? "Sprache: AN" : "Sprache: AUS");
}

void toggleNotesMenu(lv_event_t *)
{
    if (!notesMenu)
        return;
    if (lv_obj_has_flag(notesMenu, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(notesMenu, LV_OBJ_FLAG_HIDDEN);
        // The note editor's textarea is created after the menu and otherwise paints over it.
        lv_obj_move_foreground(notesMenu);
    } else {
        lv_obj_add_flag(notesMenu, LV_OBJ_FLAG_HIDDEN);
    }
}

void refreshSelectionMenu(lv_timer_t *)
{
    if (!selectionMenu || !editScreen || lv_scr_act() != editScreen)
        return;

    lv_obj_t *selected = nullptr;
    lv_obj_t *candidates[] = {titleArea, editArea};
    for (lv_obj_t *candidate : candidates) {
        if (candidate && lv_obj_is_valid(candidate) && lv_textarea_text_is_selected(candidate)) {
            lv_obj_t *label = lv_textarea_get_label(candidate);
            if (label && lv_label_get_text_selection_start(label) != LV_LABEL_TEXT_SELECTION_OFF &&
                lv_label_get_text_selection_end(label) != LV_LABEL_TEXT_SELECTION_OFF &&
                lv_label_get_text_selection_start(label) != lv_label_get_text_selection_end(label)) {
                selected = candidate;
                break;
            }
        }
    }

    if (!selected) {
        selectionTarget = nullptr;
        lv_obj_add_flag(selectionMenu, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    selectionTarget = selected;
    lv_obj_t *label = lv_textarea_get_label(selected);
    lv_obj_t *pasteButton = lv_obj_get_child(selectionMenu, 2);
    if (pasteButton) {
        if (tdeckclipboard::hasText())
            lv_obj_clear_state(pasteButton, LV_STATE_DISABLED);
        else
            lv_obj_add_state(pasteButton, LV_STATE_DISABLED);
    }

    lv_area_t labelArea;
    lv_obj_get_coords(label, &labelArea);
    uint32_t start = lv_label_get_text_selection_start(label);
    uint32_t end = lv_label_get_text_selection_end(label);
    if (start > end) {
        uint32_t tmp = start;
        start = end;
        end = tmp;
    }
    lv_point_t first{}, last{};
    lv_label_get_letter_pos(label, start, &first);
    lv_label_get_letter_pos(label, end, &last);
    const int32_t centerX = labelArea.x1 + (first.x + last.x) / 2;
    const int32_t centerY = labelArea.y1 + (first.y + last.y) / 2;
    constexpr int32_t menuWidth = 312;
    constexpr int32_t menuHeight = 30;
    int32_t x = centerX - menuWidth / 2;
    if (x < 4) x = 4;
    if (x > 320 - menuWidth - 4) x = 320 - menuWidth - 4;
    int32_t y = centerY - menuHeight - 5;
    if (y < 27) y = centerY + 12;
    if (y > 208 - menuHeight) y = 208 - menuHeight;
    lv_obj_set_pos(selectionMenu, x, y);
    lv_obj_clear_flag(selectionMenu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(selectionMenu);
}

void handleTextareaKey(lv_event_t *e)
{
    const uint32_t key = lv_event_get_key(e);
    if ((key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) &&
        tdeckclipboard::deleteSelection((lv_obj_t *)lv_event_get_current_target(e))) {
        lv_event_stop_processing(e);
        if (selectionMenu)
            lv_obj_add_flag(selectionMenu, LV_OBJ_FLAG_HIDDEN);
        selectionTarget = nullptr;
    }
}

void addTextareaSelectionHandlers(lv_obj_t *textarea)
{
    // PREPROCESS runs before LVGL's textarea handler, so Backspace/Delete can replace
    // the selected range instead of deleting only the character beside the caret.
    lv_obj_add_event_cb(textarea, handleTextareaKey,
                        (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), nullptr);
    lv_obj_add_event_cb(textarea, [](lv_event_t *) { refreshSelectionMenu(nullptr); },
                        LV_EVENT_RELEASED, nullptr);
}

void onMicButtonClicked(lv_event_t *)
{
    if (tdeckvoice::state() == 2 || curReadOnly)
        return;
    noteMicFlow = true;
    tdeckvoice::setTarget(editArea);
    tdeckvoice::startRecording();
    refreshMicButton(nullptr);
}

// ---------------- UI ----------------
lv_obj_t *barBtn(lv_obj_t *parent, const char *txt, int w, lv_align_t align, int xofs, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, 24);
    lv_obj_align(btn, align, xofs, 4);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);
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

void rebuildList(void);
void openEditor(unsigned id, bool isNew);

// "Delete this note?" — a child of editScreen, hidden until Delete is tapped. A child rather
// than lv_layer_top on purpose: the layer is shared with the mesh pop-ups, and the launcher's
// "am I typing?" test has been caught out by it before.
void buildConfirmPanel(void)
{
    confirmPanel = lv_obj_create(editScreen);
    lv_obj_set_size(confirmPanel, 250, 104);
    lv_obj_center(confirmPanel);
    lv_obj_set_style_bg_color(confirmPanel, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
    lv_obj_set_style_border_color(confirmPanel, lv_color_hex(0x636366), LV_PART_MAIN);
    lv_obj_set_style_border_width(confirmPanel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(confirmPanel, 12, LV_PART_MAIN);
    lv_obj_clear_flag(confirmPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(confirmPanel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *q = lv_label_create(confirmPanel);
    lv_label_set_text(q, "Notiz löschen?");
    lv_obj_set_style_text_color(q, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t *sub = lv_label_create(confirmPanel);
    lv_label_set_text(sub, "Das kann nicht rückgängig gemacht werden.");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t *cancel = barBtn(confirmPanel, "Abbrechen", 92, LV_ALIGN_BOTTOM_LEFT, 0, 0x48484a,
                              [](lv_event_t *) { lv_obj_add_flag(confirmPanel, LV_OBJ_FLAG_HIDDEN); });
    lv_obj_t *del = barBtn(confirmPanel, "Löschen", 92, LV_ALIGN_BOTTOM_RIGHT, 0, 0xff453a, [](lv_event_t *) {
    lv_obj_add_flag(confirmPanel, LV_OBJ_FLAG_HIDDEN);
        skipUnloadSave = true;
        deleteCurrentNote();
        rebuildList();
        lv_screen_load_anim(listScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    });
    // barBtn aligns with a +4 y-offset meant for a top bar; pull both back onto the panel floor.
    lv_obj_set_y(cancel, -6);
    lv_obj_set_y(del, -6);
}

void buildListScreen(void)
{
    listScreen = lv_obj_create(NULL);
    tui_statusbar_reserve(listScreen); // note item A4: the persistent top bar
    lv_obj_set_style_bg_color(listScreen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(listScreen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(listScreen);
    lv_label_set_text(title, "Notizen");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffd60a), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 9);

    barBtn(listScreen, "+ New", 64, LV_ALIGN_TOP_RIGHT, -4, 0x30d158,
           [](lv_event_t *) { openEditor(nextFreeId(), true); });

    listPanel = lv_obj_create(listScreen);
    lv_obj_remove_style_all(listPanel);
    lv_obj_set_pos(listPanel, 0, 32);
    lv_obj_set_size(listPanel, 320, 208);
    lv_obj_set_flex_flow(listPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(listPanel, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(listPanel, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(listPanel, LV_DIR_VER);
}

void rebuildList(void)
{
    lv_obj_clean(listPanel);
    scanNotes();

#if NOTES_HAVE_SD
    if (noteCount == 0) {
        lv_obj_t *empty = lv_label_create(listPanel);
        lv_label_set_text(empty, "Noch keine Notizen – tippe auf + Neu");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    }
#else
    lv_obj_t *empty = lv_label_create(listPanel);
    lv_label_set_text(empty, "Notizen benötigen eine SD-Karte");
    lv_obj_set_style_text_color(empty, lv_color_hex(0x8e8e93), LV_PART_MAIN);
#endif

    for (int i = 0; i < noteCount; i++) {
        lv_obj_t *row = lv_btn_create(listPanel);
        lv_obj_set_size(row, LV_PCT(100), 36);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
        lv_obj_add_event_cb(
            row,
            [](lv_event_t *e) {
                int idx = (int)(intptr_t)lv_event_get_user_data(e);
                if (idx < noteCount)
                    openEditor(noteIds[idx], false);
            },
            LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, notePreview[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        tui_one_line(lbl); // LONG_DOT needs a pinned height - see TuiLabel.h
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

void buildEditScreen(void)
{
    editScreen = lv_obj_create(NULL);
    tui_statusbar_reserve(editScreen); // note item A4: the persistent top bar
    lv_obj_set_style_bg_color(editScreen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(editScreen, LV_OBJ_FLAG_SCROLLABLE);

    // The note's TITLE, editable, where the grey "type away" hint used to be. It is the first
    // line of the file, so nothing about the on-card format changes and the list preview still
    // shows exactly what this box shows.
    titleArea = lv_textarea_create(editScreen);
    lv_textarea_set_one_line(titleArea, true);
    lv_textarea_set_text_selection(titleArea, true);
    // The hamburger menu is now the only top-right control, so let the title use the full row
    // and stop just before it instead of leaving a black gap after the field.
    lv_obj_set_size(titleArea, 276, 26);
    lv_obj_align(titleArea, LV_ALIGN_TOP_LEFT, 4, 3);
    lv_obj_set_style_bg_color(titleArea, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_text_color(titleArea, lv_color_hex(0xffd60a), LV_PART_MAIN);
    lv_obj_set_style_border_width(titleArea, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(titleArea, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(titleArea, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(titleArea, lv_color_hex(0xffffff), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(titleArea, LV_OPA_50, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(titleArea, 0, LV_PART_CURSOR);
    // ⛔ AND DELETE THE ANIMATION THAT ALREADY EXISTS. lv_textarea_create() starts a
    // blinking cursor during construction using the DEFAULT time, so setting the duration to
    // zero above does not stop it - start_cursor_blink only re-reads that on focus, or on a
    // style change delivered to the label child. An active LVGL animation then forces a
    // refresh EVERY FRAME: measured 17-18 fps and 124-280 KB/s pushed while sitting idle,
    // against 2 fps once it is gone. Any later focus re-runs the check, finds the zero and
    // deletes it itself, so this one call is all that is needed.
    lv_anim_delete(titleArea, nullptr);
    lv_textarea_set_max_length(titleArea, 40);
    lv_textarea_set_placeholder_text(titleArea, "Titel");
    if (lv_group_get_default())
        lv_group_add_obj(lv_group_get_default(), titleArea);
    // Tapping a box aims the keyboard at it. Without this the focus guard below would drag
    // focus straight back to the body and the title could never be typed into.
    lv_obj_add_event_cb(
        titleArea, [](lv_event_t *) { focusTarget = titleArea; }, LV_EVENT_CLICKED, NULL);
    addTextareaSelectionHandlers(titleArea);

    // Kept for view-only files, which have no editable title (see notes_open_file).
    editHint = lv_label_create(editScreen);
    lv_label_set_text(editHint, "");
    lv_obj_set_style_text_color(editHint, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(editHint, LV_ALIGN_TOP_MID, 0, 9);
    lv_obj_add_flag(editHint, LV_OBJ_FLAG_HIDDEN);

    // only shown for notes opened from the list; Files has its own Trash button.
    // Deleting is permanent, so it asks first (Jake, 2026-09-18).
    lv_obj_t *menuBtn = barBtn(editScreen, "", 28, LV_ALIGN_TOP_RIGHT, -4, 0x2c2c2e, toggleNotesMenu);
    addHamburgerIcon(menuBtn);
    notesMenu = lv_obj_create(editScreen);
    lv_obj_set_pos(notesMenu, 158, 30);
    lv_obj_set_size(notesMenu, 158, 130);
    lv_obj_set_style_bg_color(notesMenu, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_border_width(notesMenu, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(notesMenu, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
    lv_obj_set_style_radius(notesMenu, 8, LV_PART_MAIN);
    lv_obj_add_flag(notesMenu, LV_OBJ_FLAG_HIDDEN);
    voiceToggleLabel = lv_label_create(notesMenu);
    lv_obj_set_pos(voiceToggleLabel, 12, 10);
    lv_obj_set_style_text_color(voiceToggleLabel, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_t *voiceToggle = lv_btn_create(notesMenu);
    lv_obj_set_pos(voiceToggle, 8, 4);
    lv_obj_set_size(voiceToggle, 142, 28);
    lv_obj_add_event_cb(voiceToggle, [](lv_event_t *) {
        if (tdeckvoice::state() == 1 || tdeckvoice::state() == 2)
            return;
        tdeckvoice::setVoiceEnabled(!tdeckvoice::voiceEnabled());
        refreshVoiceToggle();
        refreshMicButton(nullptr);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_move_foreground(voiceToggleLabel);
    lv_label_set_text(voiceToggleLabel, "Sprache: AUS");
    // Delete remains available from this compact menu.
    notesMenuDelete = lv_btn_create(notesMenu);
    lv_obj_set_pos(notesMenuDelete, 8, 60);
    lv_obj_set_size(notesMenuDelete, 142, 28);
    lv_obj_add_event_cb(notesMenuDelete, [](lv_event_t *) {
        if (confirmPanel)
            lv_obj_clear_flag(confirmPanel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(notesMenu, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *menuDeleteLabel = lv_label_create(notesMenuDelete);
    lv_label_set_text(menuDeleteLabel, "Notiz löschen");
    lv_obj_center(menuDeleteLabel);

    // Favorite and delete actions are kept in the hamburger menu to leave only the title
    // and menu control in the editor's top row.
    favoriteBtn = notesMenuFavorite = lv_btn_create(notesMenu);
    lv_obj_set_pos(favoriteBtn, 8, 32);
    lv_obj_set_size(favoriteBtn, 142, 28);
    lv_obj_add_event_cb(favoriteBtn, [](lv_event_t *) {
        toggleCurrentFavorite(nullptr);
        lv_obj_add_flag(notesMenu, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *favoriteLabel = lv_label_create(favoriteBtn);
    lv_label_set_text(favoriteLabel, "Favorit umschalten");
    lv_obj_center(favoriteLabel);

    notesMenuShare = lv_btn_create(notesMenu);
    lv_obj_set_pos(notesMenuShare, 8, 60);
    lv_obj_set_size(notesMenuShare, 142, 28);
    lv_obj_add_event_cb(notesMenuShare, [](lv_event_t *) {
        if (curFromFiles || !curPath[0])
            return;
        saveCurrentNote();
        lv_obj_add_flag(notesMenu, LV_OBJ_FLAG_HIDDEN);
        tdeck_open_note_download(curPath);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *shareLabel = lv_label_create(notesMenuShare);
    lv_label_set_text(shareLabel, "QR-Download");
    lv_obj_center(shareLabel);

    lv_obj_set_pos(notesMenuDelete, 8, 92);

    // Notes can be starred independently from Meshtastic node favourites. The star is stored in
    // /notes/.favorites and the Favorites launcher shows these note entries alongside nodes.

    buildConfirmPanel();

    editArea = lv_textarea_create(editScreen);
    lv_textarea_set_text_selection(editArea, true);
    lv_obj_set_pos(editArea, 0, 32);
    lv_obj_set_size(editArea, 320, 176);
    lv_obj_set_style_bg_color(editArea, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_text_color(editArea, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_border_width(editArea, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(editArea, 0, LV_PART_MAIN);
    // Visible caret. A blinking thin line proved unreliable — it only repainted
    // when the screen changed (so it seemed to appear only on Back). Use a SOLID,
    // non-blinking, semi-transparent white block over the character position: it's
    // clearly visible and doesn't depend on the blink timer firing a repaint.
    // (anim_duration 0 => LVGL keeps the cursor shown solid whenever focused.)
    lv_obj_set_style_bg_color(editArea, lv_color_hex(0xffffff), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(editArea, LV_OPA_50, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(editArea, 0, LV_PART_CURSOR);
    lv_anim_delete(editArea, nullptr);
    lv_textarea_set_max_length(editArea, kMaxNoteBytes);
    lv_textarea_set_placeholder_text(editArea, "Hier auf der Tastatur schreiben …");
    if (lv_group_get_default())
        lv_group_add_obj(lv_group_get_default(), editArea);
    lv_obj_add_event_cb(
        editArea, [](lv_event_t *) { focusTarget = editArea; }, LV_EVENT_CLICKED, NULL);
    addTextareaSelectionHandlers(editArea);

    micBtn = lv_btn_create(editScreen);
    lv_obj_set_size(micBtn, 132, 24);
    lv_obj_align(micBtn, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_radius(micBtn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(micBtn, lv_color_hex(0x30a46c), LV_PART_MAIN);
    lv_obj_add_event_cb(micBtn, onMicButtonClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(micBtn, [](lv_event_t *) { focusTarget = micBtn; }, LV_EVENT_FOCUSED, nullptr);
    if (lv_group_get_default())
        lv_group_add_obj(lv_group_get_default(), micBtn);
    micBtnLabel = lv_label_create(micBtn);
    lv_label_set_text(micBtnLabel, "Mic starten");
    lv_obj_center(micBtnLabel);
    micStatusLabel = lv_label_create(editScreen);
    lv_obj_set_pos(micStatusLabel, 5, 184);
    lv_obj_set_size(micStatusLabel, 310, 22);
    lv_obj_set_style_bg_color(micStatusLabel, lv_color_hex(0x202024), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(micStatusLabel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(micStatusLabel, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(micStatusLabel, 4, LV_PART_MAIN);
    lv_label_set_long_mode(micStatusLabel, LV_LABEL_LONG_DOT);
    tui_one_line(micStatusLabel);
    lv_obj_add_flag(micStatusLabel, LV_OBJ_FLAG_HIDDEN);

    // Phone-style actions that appear automatically next to selected text.
    selectionMenu = lv_obj_create(editScreen);
    lv_obj_remove_style_all(selectionMenu);
    lv_obj_set_size(selectionMenu, 312, 30);
    lv_obj_set_style_bg_color(selectionMenu, lv_color_hex(0x25252a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(selectionMenu, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(selectionMenu, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(selectionMenu, lv_color_hex(0x5a5a62), LV_PART_MAIN);
    lv_obj_set_style_radius(selectionMenu, 7, LV_PART_MAIN);
    lv_obj_set_style_pad_all(selectionMenu, 2, LV_PART_MAIN);
    lv_obj_clear_flag(selectionMenu, LV_OBJ_FLAG_SCROLLABLE);
    const char *selectionLabels[] = {"Kopieren", "Aussch.", "Einfügen", "Löschen"};
    lv_event_cb_t selectionActions[] = {
        [](lv_event_t *) {
            if (selectionTarget) tdeckclipboard::copySelection(selectionTarget);
        },
        [](lv_event_t *) {
            if (selectionTarget) tdeckclipboard::cutSelection(selectionTarget);
            refreshSelectionMenu(nullptr);
        },
        [](lv_event_t *) {
            if (selectionTarget) tdeckclipboard::paste(selectionTarget);
            refreshSelectionMenu(nullptr);
        },
        [](lv_event_t *) {
            if (selectionTarget) tdeckclipboard::deleteSelection(selectionTarget);
            refreshSelectionMenu(nullptr);
        }};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *button = lv_btn_create(selectionMenu);
        lv_obj_set_pos(button, 2 + i * 77, 2);
        lv_obj_set_size(button, 75, 24);
        lv_obj_add_event_cb(button, selectionActions[i], LV_EVENT_CLICKED, nullptr);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, selectionLabels[i]);
        lv_obj_center(label);
    }
    lv_obj_add_flag(selectionMenu, LV_OBJ_FLAG_HIDDEN);
    if (!selectionMenuTimer)
        selectionMenuTimer = lv_timer_create(refreshSelectionMenu, 120, nullptr);
    lv_timer_pause(selectionMenuTimer);
    if (!micButtonTimer)
        micButtonTimer = lv_timer_create(refreshMicButton, 250, nullptr);
    refreshVoiceToggle();
    refreshMicButton(nullptr);

    // keep the physical keyboard aimed at the textarea while the editor is up
    focusGuard = lv_timer_create(
        [](lv_timer_t *) {
            lv_group_t *g = lv_group_get_default();
            lv_obj_t *want = focusTarget ? focusTarget : editArea;
            if (g && lv_group_get_focused(g) != want)
                lv_group_focus_obj(want);
        },
        300, NULL);
    lv_timer_pause(focusGuard);

    lv_obj_add_event_cb(
        editScreen,
        [](lv_event_t *) {
            noteMicFlow = false;
            lv_timer_resume(focusGuard);
            lv_timer_resume(micButtonTimer);
            lv_timer_resume(selectionMenuTimer);
            refreshMicButton(nullptr);
            if (lv_group_get_default())
                lv_group_focus_obj(focusTarget ? focusTarget : editArea);
        },
        LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(
        editScreen,
        [](lv_event_t *) {
            lv_timer_pause(focusGuard);
            lv_timer_pause(micButtonTimer);
            lv_timer_pause(selectionMenuTimer);
            if (skipUnloadSave)
                skipUnloadSave = false;
            else
                saveCurrentNote();
        },
        LV_EVENT_SCREEN_UNLOADED, NULL);
}

void openEditor(unsigned id, bool isNew)
{
    if (!editScreen)
        buildEditScreen();
    curId = id;
    curIsNew = isNew;
    curFromFiles = false;
    curReadOnly = false;
    notePath(curPath, sizeof(curPath), id);
    lv_obj_add_flag(notesMenu, LV_OBJ_FLAG_HIDDEN);
    if (selectionMenu)
        lv_obj_add_flag(selectionMenu, LV_OBJ_FLAG_HIDDEN);
    selectionTarget = nullptr;
    lv_obj_clear_flag(notesMenuDelete, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(notesMenuFavorite, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(notesMenuShare, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(editHint, LV_OBJ_FLAG_HIDDEN);
    if (tdeckvoice::voiceEnabled())
        lv_obj_clear_flag(micBtn, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(micBtn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(titleArea, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(confirmPanel, LV_OBJ_FLAG_HIDDEN);
    // A new note starts with the cursor in the title; an existing one in the body, where you are
    // far more likely to be adding to it.
    focusTarget = isNew ? titleArea : editArea;
    if (isNew) {
        lv_textarea_set_text(titleArea, "");
        lv_textarea_set_text(editArea, "");
    } else {
        loadCurPathIntoEditor();
    }
    updateFavoriteButton();
    lv_screen_load_anim(editScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}
} // namespace

extern "C" void notes_open(void)
{
    lv_obj_t *active = lv_screen_active();
    if (active != listScreen && active != editScreen)
        prevScreen = active; // remember the launcher, not our own screens
    if (!listScreen)
        buildListScreen();
    rebuildList();
    lv_screen_load_anim(listScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// Files app hook: open any .txt on the SD card in the editor. Leaving through the
// persistent top bar saves and returns to the launcher; oversized files open view-only.
extern "C" void notes_open_file(const char *path)
{
#if NOTES_HAVE_SD
    if (!path || !*path)
        return;
    if (!editScreen)
        buildEditScreen();
    snprintf(curPath, sizeof(curPath), "%s", path);
    curId = 0;
    curIsNew = false;
    curFromFiles = true;
    filesReturnScreen = lv_screen_active();
    lv_obj_add_flag(notesMenu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notesMenuDelete, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notesMenuFavorite, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notesMenuShare, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(confirmPanel, LV_OBJ_FLAG_HIDDEN);
    // A file from the Files app is not a note: its first line is just its first line, so hide the
    // title box and load the whole thing into the body, exactly as before.
    lv_obj_add_flag(titleArea, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(editHint, LV_OBJ_FLAG_HIDDEN);
    focusTarget = editArea;
    loadFileIntoEditor();
    if (curReadOnly || !tdeckvoice::voiceEnabled())
        lv_obj_add_flag(micBtn, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(micBtn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(editHint, curReadOnly ? "Nur Ansicht (große Datei)" : "Bearbeiten");
    lv_screen_load_anim(editScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
#else
    (void)path;
#endif
}

// Small cross-app bridge used by Favorites. Keeping the note storage here means the Notes app
// remains the single owner of its SD-card format; Favorites only asks for a list and opens an id.
extern "C" int tdeck_notes_favorite_count(void)
{
#if NOTES_HAVE_SD
    scanNotes();
    int count = 0;
    for (int i = 0; i < noteCount; i++)
        if (noteIsFavorite(noteIds[i]))
            count++;
    return count;
#else
    return 0;
#endif
}

extern "C" unsigned tdeck_notes_favorite_id(int index)
{
#if NOTES_HAVE_SD
    scanNotes();
    if (index < 0)
        return 0;
    int seen = 0;
    for (int i = 0; i < noteCount; i++) {
        if (!noteIsFavorite(noteIds[i]))
            continue;
        if (seen++ == index)
            return noteIds[i];
    }
#else
    (void)index;
#endif
    return 0;
}

extern "C" const char *tdeck_note_title(unsigned id)
{
    static char title[48];
    title[0] = 0;
#if NOTES_HAVE_SD
    scanNotes();
    for (int i = 0; i < noteCount; i++) {
        if (noteIds[i] == id) {
            snprintf(title, sizeof(title), "%s", notePreview[i]);
            break;
        }
    }
#else
    (void)id;
#endif
    return title;
}

extern "C" void notes_open_id(unsigned id)
{
    if (!id)
        return;
    openEditor(id, false);
}
