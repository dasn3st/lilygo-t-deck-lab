#include "TDeckClipboard.h"

#include <cstdio>
#include <cstring>

namespace tdeckclipboard
{
namespace
{
constexpr size_t kClipboardCapacity = 4096;
char s_clipboard[kClipboardCapacity] = {};

size_t utf8Length(const char *text)
{
    size_t count = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        if ((*p & 0xc0) != 0x80)
            ++count;
    return count;
}

size_t utf8ByteOffset(const char *text, size_t characterIndex)
{
    size_t count = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p && count < characterIndex) {
        if ((*p & 0xc0) != 0x80)
            ++count;
        ++p;
    }
    return (size_t)(p - (const unsigned char *)text);
}

bool selectionRange(lv_obj_t *textarea, size_t &start, size_t &end)
{
    if (!textarea || !lv_obj_is_valid(textarea) || !lv_obj_check_type(textarea, &lv_textarea_class) ||
        !lv_textarea_text_is_selected(textarea))
        return false;
    lv_obj_t *label = lv_textarea_get_label(textarea);
    if (!label)
        return false;
    start = lv_label_get_text_selection_start(label);
    end = lv_label_get_text_selection_end(label);
    if (start > end) {
        size_t tmp = start;
        start = end;
        end = tmp;
    }
    const size_t length = utf8Length(lv_textarea_get_text(textarea));
    if (end > length)
        end = length;
    return start < end;
}

bool eraseSelection(lv_obj_t *textarea, size_t start, size_t end)
{
    const char *text = lv_textarea_get_text(textarea);
    if (!text)
        return false;
    const size_t first = utf8ByteOffset(text, start);
    const size_t last = utf8ByteOffset(text, end);
    const size_t length = strlen(text);
    char result[kClipboardCapacity];
    if (length >= sizeof(result))
        return false;
    memcpy(result, text, first);
    memcpy(result + first, text + last, length - last + 1);
    lv_textarea_set_text(textarea, result);
    lv_textarea_set_cursor_pos(textarea, (int32_t)start);
    lv_textarea_clear_selection(textarea);
    return true;
}
} // namespace

bool copyText(const char *text)
{
    if (!text || !*text)
        return false;
    snprintf(s_clipboard, sizeof(s_clipboard), "%s", text);
    return s_clipboard[0] != 0;
}

bool copySelection(lv_obj_t *textarea)
{
    size_t start, end;
    if (!selectionRange(textarea, start, end))
        return false;
    const char *text = lv_textarea_get_text(textarea);
    const size_t first = utf8ByteOffset(text, start);
    const size_t last = utf8ByteOffset(text, end);
    size_t bytes = last - first;
    if (bytes >= sizeof(s_clipboard))
        bytes = sizeof(s_clipboard) - 1;
    memcpy(s_clipboard, text + first, bytes);
    s_clipboard[bytes] = 0;
    return bytes > 0;
}

bool cutSelection(lv_obj_t *textarea)
{
    size_t start, end;
    if (!selectionRange(textarea, start, end) || !copySelection(textarea))
        return false;
    return eraseSelection(textarea, start, end);
}

bool deleteSelection(lv_obj_t *textarea)
{
    size_t start, end;
    if (!selectionRange(textarea, start, end))
        return false;
    return eraseSelection(textarea, start, end);
}

bool selectAll(lv_obj_t *textarea)
{
    if (!textarea || !lv_obj_is_valid(textarea) || !lv_obj_check_type(textarea, &lv_textarea_class))
        return false;
    lv_obj_t *label = lv_textarea_get_label(textarea);
    const char *text = lv_textarea_get_text(textarea);
    const size_t length = text ? utf8Length(text) : 0;
    if (!label || !length)
        return false;
    lv_textarea_set_text_selection(textarea, true);
    lv_label_set_text_selection_start(label, 0);
    lv_label_set_text_selection_end(label, (uint32_t)length);
    lv_textarea_set_cursor_pos(textarea, LV_TEXTAREA_CURSOR_LAST);
    return true;
}

bool paste(lv_obj_t *textarea)
{
    if (!s_clipboard[0] || !textarea || !lv_obj_is_valid(textarea) ||
        !lv_obj_check_type(textarea, &lv_textarea_class))
        return false;
    size_t start, end;
    if (selectionRange(textarea, start, end))
        eraseSelection(textarea, start, end);
    lv_textarea_add_text(textarea, s_clipboard);
    return true;
}

bool hasText() { return s_clipboard[0] != 0; }
} // namespace tdeckclipboard
