#pragma once

#include <lvgl.h>

namespace tdeckclipboard
{
bool copyText(const char *text);
bool copySelection(lv_obj_t *textarea);
bool cutSelection(lv_obj_t *textarea);
bool deleteSelection(lv_obj_t *textarea);
bool selectAll(lv_obj_t *textarea);
bool paste(lv_obj_t *textarea);
bool hasText();
} // namespace tdeckclipboard
