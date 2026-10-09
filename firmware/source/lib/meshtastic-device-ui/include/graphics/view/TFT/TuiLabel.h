#pragma once
#include "lvgl.h"

// Pin a label to exactly one line and make LV_LABEL_LONG_DOT actually work.
//
// THE BUG THIS EXISTS FOR. LVGL clips LV_LABEL_LONG_DOT to the label's HEIGHT, and a label's
// default height is LV_SIZE_CONTENT. So a label with a width and LONG_DOT does NOT end in
// dots when the text is too long - it grows a second line and draws it on top of whatever
// sits underneath. Nineteen labels across this UI were written that way, including the two
// on every row of the Chats list, which is the "weird overlap of text" Jake screenshotted on
// 2026-09-19. Caught by auditing every LONG_DOT in the tree after the PC renderer showed the
// same thing happening on the new lock-screen widget.
//
// Call it AFTER lv_obj_set_style_text_font(), because the height comes from the font in use.
void tui_one_line(lv_obj_t *label);
