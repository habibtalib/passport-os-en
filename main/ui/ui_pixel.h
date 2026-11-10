// Shared 240x320 travel-instrument widgets. Warm paper text, charcoal surfaces,
// open headings and amber focus. No decorative background animation.
#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "ui_palette.h"

// Letter spacing spec
#define UI_LS_TITLE   1
#define UI_LS_BODY    0

// Terminal background; no decorative objects or continuously running timers.
void ui_pixel_background(lv_obj_t *scr);
lv_obj_t *ui_pixel_screen_create(const char *title);
lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color);
lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color);
lv_obj_t *ui_pixel_label_ls(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color,
                            int letter_space);
// Avoid reallocating/invalidation when periodic status text has not changed.
void ui_pixel_label_set_text(lv_obj_t *label, const char *text);
// LVGL style setters invalidate even when the value is unchanged.
void ui_pixel_bg_color(lv_obj_t *obj, uint32_t color);
void ui_pixel_text_color(lv_obj_t *obj, uint32_t color);
// Short, interruptible focus confirmation; no perpetual decorative timer.
void ui_pixel_focus_feedback(lv_obj_t *obj);
// Two bounded footer lines: current action, then navigation/secondary action.
lv_obj_t *ui_pixel_footer(lv_obj_t *parent, const char *action, const char *navigation,
                          const lv_font_t *font);
lv_obj_t *ui_pixel_mascot_create(lv_obj_t *parent, int x, int y);
void ui_pixel_mascot_jump(lv_obj_t *mascot);
void ui_pixel_set_selected(lv_obj_t *panel, bool selected, bool enabled);

// ---- 2026-09-13 new shared components ----

// Open title with a bottom rule, bounded to 216px at y.
// Returns the card object (y can be changed further or children added).
lv_obj_t *ui_pixel_title_plate(lv_obj_t *parent, int y, const char *text,
                               const lv_font_t *font);

// Single, dynamically editable footer label with a bounded width.
lv_obj_t *ui_pixel_hint_pill(lv_obj_t *parent, const char *text,
                             const lv_font_t *font);

// 26px shared status bar. Use LEFT_MID/RIGHT_MID
// Alignment means vertical centering.
lv_obj_t *ui_pixel_top_bar(lv_obj_t *parent);

// Battery text starts at x=160; keep other status content before x=156.
enum { UI_STATUS_CONTENT_RIGHT = 156, UI_STATUS_BATTERY_LEFT = 160 };
void ui_pixel_status_text_align(lv_obj_t *label, int x, int width,
                                lv_text_align_t align);

// Pixel battery: outline frame + charge fill + percentage text.
// frame_color/text color are supplied by the caller (white on the dark top bar, ink on light paper).
typedef struct {
    lv_obj_t *fill;
    lv_obj_t *label;
} ui_pixel_battery_t;
ui_pixel_battery_t ui_pixel_battery_create(lv_obj_t *parent, int x, int y,
                                           uint32_t frame_color,
                                           const lv_font_t *font);
void ui_pixel_battery_set(ui_pixel_battery_t *batt, int soc);  // soc<0 shows "--"
void ui_pixel_battery_set_on_dark(ui_pixel_battery_t *batt, int soc);

// Wi-Fi signal, 3 bars (geometric bars, no dependence on the icon font).
void ui_pixel_wifi_bars_create(lv_obj_t *parent, int x, int y,
                               lv_obj_t *bars[3]);
void ui_pixel_wifi_bars_set(lv_obj_t *bars[3], int level, uint32_t color);

// Clock-theme compatibility. The terminal palette stays dark in both modes;
// no screen pointers are retained and no decorative timers are started.
bool ui_pixel_set_hour_theme(int hour);
bool ui_pixel_theme_is_night(void);
bool ui_pixel_theme_apply(lv_obj_t *scr);

#ifdef __cplusplus
}
#endif
