// Compact To-do list opened by the launcher's left quick-access strip.
#include "graphics/view/TFT/TuiStatusBar.h"
#include "lvgl.h"
#include <cstdio>
#include <cstring>

#if HAS_SDCARD && !HAS_SD_MMC && !ARCH_PORTDUINO
#include "graphics/common/SdCard.h"
#define TODO_HAVE_SD 1
#else
#define TODO_HAVE_SD 0
#endif

extern "C" void todo_open(void);

namespace {
constexpr int kTodoMax = 24;
constexpr int kTodoText = 72;
constexpr const char *kTodoPath = "/todo/tasks.txt";
struct Todo { char text[kTodoText]; bool done; };
Todo todos[kTodoMax] = {};
int todoCount = 0;
lv_obj_t *screen = nullptr;
lv_obj_t *list = nullptr;
lv_obj_t *entry = nullptr;

void loadTodos()
{
    todoCount = 0;
#if TODO_HAVE_SD
    FsFile f = SDFs.open(kTodoPath, O_RDONLY);
    if (!f) return;
    char line[kTodoText + 8] = {};
    while (todoCount < kTodoMax && f.fgets(line, sizeof(line)) > 0) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n < 3 || line[1] != '|') continue;
        Todo &t = todos[todoCount++];
        t.done = line[0] == '1';
        snprintf(t.text, sizeof(t.text), "%s", line + 2);
    }
    f.close();
#endif
}

void saveTodos()
{
#if TODO_HAVE_SD
    SDFs.mkdir("/todo");
    FsFile f = SDFs.open(kTodoPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) return;
    for (int i = 0; i < todoCount; ++i) {
        f.print(todos[i].done ? "1|" : "0|");
        f.println(todos[i].text);
    }
    f.sync();
    f.close();
#endif
}

void rebuildList();

void addTodo(lv_event_t *)
{
    if (!entry || todoCount >= kTodoMax) return;
    const char *text = lv_textarea_get_text(entry);
    if (!text || !*text) return;
    Todo &t = todos[todoCount++];
    snprintf(t.text, sizeof(t.text), "%s", text);
    for (char *p = t.text; *p; ++p) if (*p == '|' || *p == '\n' || *p == '\r') *p = ' ';
    t.done = false;
    lv_textarea_set_text(entry, "");
    saveTodos();
    rebuildList();
    if (lv_group_get_default()) lv_group_focus_obj(entry);
}

void toggleTodo(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= todoCount) return;
    todos[i].done = !todos[i].done;
    saveTodos();
    rebuildList();
}

void deleteTodo(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= todoCount) return;
    for (int j = i + 1; j < todoCount; ++j) todos[j - 1] = todos[j];
    --todoCount;
    saveTodos();
    rebuildList();
}

void rebuildList()
{
    if (!list) return;
    lv_obj_clean(list);
    if (!todoCount) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "Noch keine Aufgaben. Oben eine To-do eingeben.");
        lv_obj_set_width(empty, 300);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        return;
    }
    for (int i = 0; i < todoCount; ++i) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 308, 29);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(0x28282a), LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_t *check = lv_btn_create(row);
        lv_obj_set_size(check, 25, 25);
        lv_obj_set_pos(check, 0, 1);
        lv_obj_set_style_bg_color(check, lv_color_hex(todos[i].done ? 0x30d158 : 0x2c2c2e), LV_PART_MAIN);
        lv_obj_add_event_cb(check, toggleTodo, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *mark = lv_label_create(check);
        lv_label_set_text(mark, todos[i].done ? "✓" : "○");
        lv_obj_center(mark);
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, todos[i].text);
        lv_obj_set_width(label, 240);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 32, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(todos[i].done ? 0x77777a : 0xffffff), LV_PART_MAIN);
        lv_obj_t *del = lv_btn_create(row);
        lv_obj_set_size(del, 25, 25);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(del, lv_color_hex(0x512b2b), LV_PART_MAIN);
        lv_obj_add_event_cb(del, deleteTodo, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *x = lv_label_create(del);
        lv_label_set_text(x, "×");
        lv_obj_center(x);
    }
}

void buildScreen()
{
    screen = lv_obj_create(NULL);
    tui_statusbar_reserve(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "To-do");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    entry = lv_textarea_create(screen);
    lv_textarea_set_one_line(entry, true);
    lv_textarea_set_max_length(entry, kTodoText - 1);
    lv_textarea_set_placeholder_text(entry, "Aufgabe eingeben …");
    lv_obj_set_size(entry, 244, 28);
    lv_obj_align(entry, LV_ALIGN_TOP_LEFT, 5, 31);
    lv_obj_set_style_bg_color(entry, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_text_color(entry, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_border_width(entry, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(entry, 6, LV_PART_MAIN);
    if (lv_group_get_default()) lv_group_add_obj(lv_group_get_default(), entry);
    lv_obj_t *add = lv_btn_create(screen);
    lv_obj_set_size(add, 62, 28);
    lv_obj_align(add, LV_ALIGN_TOP_RIGHT, -5, 31);
    lv_obj_set_style_radius(add, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(add, lv_color_hex(0x30d158), LV_PART_MAIN);
    lv_obj_add_event_cb(add, addTodo, LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus = lv_label_create(add);
    lv_label_set_text(plus, "+ Hinzufügen");
    lv_obj_center(plus);

    list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 5, 64);
    lv_obj_set_size(list, 310, 172);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 2, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
}
} // namespace

extern "C" void todo_open(void)
{
    if (!screen) buildScreen();
    loadTodos();
    rebuildList();
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    if (lv_group_get_default()) lv_group_focus_obj(entry);
}
