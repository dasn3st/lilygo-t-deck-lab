#include "graphics/view/TFT/TDeckT9.h"

#include <Arduino.h>
#include <Preferences.h>
#include "lvgl.h"
#include "ui.h"

namespace {

constexpr int kSuggestionCount = 3;
constexpr int kMaxWord = 31;

// Kept in frequency order so the first suggestion is usually the useful one.
// This is intentionally firmware-sized. A card-backed dictionary can be added
// later without changing the UI or the setting.
static const char *const kWords[] = {
    "aber", "alle", "allem", "allen", "alles", "also", "am", "an", "auch", "auf", "aus",
    "beim", "bei", "bitte", "bis", "bist", "brauche", "danke", "das", "dass", "dein", "dem",
    "den", "der", "des", "die", "dies", "dir", "doch", "dort", "du", "durch", "ein", "eine",
    "einen", "einer", "es", "etwas", "für", "ganz", "gehen", "geht", "gibt", "habe", "haben",
    "hat", "heute", "hier", "ich", "immer", "in", "ist", "ja", "jetzt", "kann", "keine", "kleine",
    "machen", "man", "mehr", "mein", "meine", "mit", "mich", "mir", "morgen", "nach", "nicht",
    "noch", "nur", "oder", "ohne", "schon", "sehr", "sein", "seine", "sich", "sie", "sind", "so",
    "soll", "sollte", "später", "über", "um", "und", "uns", "unter", "vom", "von", "vor", "war",
    "warum", "was", "welche", "wenn", "wer", "werden", "wie", "wieder", "will", "wir", "wird", "wo",
    "zu", "zum", "zur"
};

lv_obj_t *suggestBar = nullptr;
lv_obj_t *suggestButtons[kSuggestionCount] = {nullptr, nullptr, nullptr};
lv_obj_t *suggestLabels[kSuggestionCount] = {nullptr, nullptr, nullptr};
lv_obj_t *suggestTarget = nullptr;
char currentCandidates[kSuggestionCount][kMaxWord + 1] = {{0}};
bool settingLoaded = false;
bool settingEnabled = true;

bool isVisibleTextarea(lv_obj_t *obj)
{
    if (!obj || !lv_obj_is_valid(obj) || !lv_obj_check_type(obj, &lv_textarea_class))
        return false;
    lv_obj_t *screen = lv_obj_get_screen(obj);
    return screen == lv_screen_active() || screen == lv_layer_top() || screen == lv_layer_sys();
}

void loadSetting()
{
    if (settingLoaded)
        return;
    settingLoaded = true;
    Preferences prefs;
    if (prefs.begin("tdeckt9", true)) {
        settingEnabled = prefs.getBool("enabled", true);
        prefs.end();
    }
}

void hideSuggestions()
{
    if (suggestBar && lv_obj_is_valid(suggestBar))
        lv_obj_add_flag(suggestBar, LV_OBJ_FLAG_HIDDEN);
    suggestTarget = nullptr;
    for (int i = 0; i < kSuggestionCount; ++i)
        currentCandidates[i][0] = '\0';
}

bool asciiLetter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

char asciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool startsWithIgnoreCase(const char *word, const char *prefix, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (!word[i] || asciiLower(word[i]) != asciiLower(prefix[i]))
            return false;
    }
    return word[len] != '\0';
}

bool getCurrentPrefix(lv_obj_t *textarea, char *prefix, size_t capacity)
{
    const char *text = lv_textarea_get_text(textarea);
    if (!text || !*text || capacity < 2)
        return false;

    // The first version intentionally handles the normal end-of-line typing path.
    // If the cursor is in the middle of a sentence, no popup is shown rather than
    // replacing the wrong word.
    const size_t length = strlen(text);
    size_t start = length;
    while (start > 0 && asciiLetter(text[start - 1]))
        --start;
    const size_t wordLen = length - start;
    if (wordLen == 0 || wordLen >= capacity)
        return false;
    for (size_t i = 0; i < wordLen; ++i) {
        if (!asciiLetter(text[start + i]))
            return false;
        prefix[i] = asciiLower(text[start + i]);
    }
    prefix[wordLen] = '\0';
    return true;
}

void applyCandidate(int index)
{
    if (index < 0 || index >= kSuggestionCount || !suggestTarget || !lv_obj_is_valid(suggestTarget))
        return;
    const char *candidate = currentCandidates[index];
    if (!candidate[0])
        return;

    const char *oldText = lv_textarea_get_text(suggestTarget);
    if (!oldText)
        return;
    char result[256] = {0};
    size_t length = strlen(oldText);
    size_t start = length;
    while (start > 0 && asciiLetter(oldText[start - 1]))
        --start;
    if (start >= sizeof(result))
        return;
    memcpy(result, oldText, start);
    result[start] = '\0';
    strncat(result, candidate, sizeof(result) - strlen(result) - 1);
    if (strlen(result) + 1 < sizeof(result))
        strcat(result, " ");
    lv_textarea_set_text(suggestTarget, result);
    lv_textarea_set_cursor_pos(suggestTarget, LV_TEXTAREA_CURSOR_LAST);
    hideSuggestions();
}

void suggestionClicked(lv_event_t *event)
{
    lv_obj_t *button = (lv_obj_t *)lv_event_get_target(event);
    const int index = (int)(intptr_t)lv_obj_get_user_data(button);
    applyCandidate(index);
}

void createBar()
{
    if (suggestBar)
        return;
    suggestBar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(suggestBar);
    lv_obj_set_size(suggestBar, 320, 28);
    lv_obj_set_style_bg_color(suggestBar, lv_color_hex(0x20242a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(suggestBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(suggestBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(suggestBar, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < kSuggestionCount; ++i) {
        suggestButtons[i] = lv_button_create(suggestBar);
        lv_obj_set_size(suggestButtons[i], 100, 24);
        lv_obj_set_pos(suggestButtons[i], 4 + i * 105, 2);
        lv_obj_set_style_radius(suggestButtons[i], 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(suggestButtons[i], lv_color_hex(0x343a43), LV_PART_MAIN);
        lv_obj_set_style_bg_color(suggestButtons[i], lv_color_hex(0x0a84ff), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_pad_all(suggestButtons[i], 0, LV_PART_MAIN);
        lv_obj_set_user_data(suggestButtons[i], (void *)(intptr_t)i);
        suggestLabels[i] = lv_label_create(suggestButtons[i]);
        lv_obj_set_style_text_font(suggestLabels[i], &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(suggestLabels[i], lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_label_set_long_mode(suggestLabels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(suggestLabels[i], 94);
        lv_label_set_text(suggestLabels[i], "");
        lv_obj_center(suggestLabels[i]);
        // On the T-Deck the completed CLICKED event can arrive late when the
        // touch/trackball press is short. Apply immediately on press so a
        // suggestion feels like a normal one-tap keyboard key. The existing
        // CLICKED handler remains as a fallback; applyCandidate() hides the
        // bar, so it cannot insert the word twice.
        lv_obj_add_event_cb(suggestButtons[i], suggestionClicked, LV_EVENT_PRESSED, nullptr);
        lv_obj_add_event_cb(suggestButtons[i], suggestionClicked, LV_EVENT_CLICKED, nullptr);
    }
}

} // namespace

bool tdeck_t9_enabled(void)
{
    loadSetting();
    return settingEnabled;
}

void tdeck_t9_set_enabled(bool enabled)
{
    loadSetting();
    settingEnabled = enabled;
    Preferences prefs;
    if (prefs.begin("tdeckt9", false)) {
        prefs.putBool("enabled", enabled);
        prefs.end();
    }
    if (!enabled)
        hideSuggestions();
}

void tdeck_t9_tick(void)
{
    loadSetting();
    createBar();
    if (!settingEnabled) {
        hideSuggestions();
        return;
    }

    lv_group_t *group = lv_group_get_default();
    lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
    if (!isVisibleTextarea(focused)) {
        hideSuggestions();
        return;
    }

    char prefix[kMaxWord + 1] = {0};
    if (!getCurrentPrefix(focused, prefix, sizeof(prefix))) {
        hideSuggestions();
        return;
    }

    int found = 0;
    for (const char *word : kWords) {
        if (!startsWithIgnoreCase(word, prefix, strlen(prefix)))
            continue;
        bool duplicate = false;
        for (int i = 0; i < found; ++i)
            duplicate = duplicate || strcmp(currentCandidates[i], word) == 0;
        if (duplicate)
            continue;
        strncpy(currentCandidates[found], word, kMaxWord);
        currentCandidates[found][kMaxWord] = '\0';
        ++found;
        if (found == kSuggestionCount)
            break;
    }
    if (found == 0) {
        hideSuggestions();
        return;
    }
    for (int i = found; i < kSuggestionCount; ++i)
        currentCandidates[i][0] = '\0';

    suggestTarget = focused;
    lv_obj_clear_flag(suggestBar, LV_OBJ_FLAG_HIDDEN);
    lv_area_t area;
    lv_obj_get_coords(focused, &area);
    int y = area.y1 - 30;
    if (y < 21)
        y = area.y2 + 2;
    if (y > 210)
        y = 210;
    lv_obj_set_pos(suggestBar, 0, y);
    for (int i = 0; i < kSuggestionCount; ++i) {
        if (currentCandidates[i][0]) {
            lv_label_set_text(suggestLabels[i], currentCandidates[i]);
            lv_obj_clear_flag(suggestButtons[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(suggestLabels[i], "");
            lv_obj_add_flag(suggestButtons[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}
