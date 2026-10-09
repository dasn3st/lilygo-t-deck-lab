#include "graphics/view/TFT/TuiLabel.h"

// See TuiLabel.h for what was wrong and how it was found.
//
// The height comes from the label's own font rather than a hard-coded 16, because this UI
// mixes montserrat_12 and the default font and a wrong number would either clip the
// descenders or leave room for half of a second line.
void tui_one_line(lv_obj_t *label)
{
    if (!label)
        return;
    const lv_font_t *f = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_obj_set_height(label, f ? lv_font_get_line_height(f) : 16);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}
