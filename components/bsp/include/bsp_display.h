// components/bsp/include/bsp_display.h
// ST7789P3 240x320 display: SPI panel init + vendor-specific registers + LEDC backlight dimming.
#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the SPI bus, panel, vendor registers and backlight LEDC. On success the screen is powered but its content is undefined.
esp_err_t bsp_display_init(void);

// Get the underlying panel handle. Use it to call esp_lcd_panel_draw_bitmap directly, or to attach a GUI other than LVGL.
// Returns NULL if not initialized.
esp_lcd_panel_handle_t bsp_display_panel(void);

// Get the underlying panel io handle (needed for LVGL integration). Returns NULL if not initialized.
esp_lcd_panel_io_handle_t bsp_display_io(void);

// Without LVGL, synchronously fill the whole GRAM with one solid color (LVGL RGB565 representation, e.g. 0x03120B).
// For cases that "must guarantee the whole screen is already a definite color" - typically: lay the base before lighting the backlight,
// otherwise the areas LVGL single-buffer chunked async refresh has not pushed will show the uninitialized white screen.
// Blocks until written. Needs about 7.5KB of internal DMA memory; returns ESP_ERR_NO_MEM on failure.
esp_err_t bsp_display_fill(uint16_t color);

// Backlight brightness 0..100 (%). LEDC PWM, 0 = fully off.
void bsp_display_backlight(uint8_t percent);

// Device-wide brightness cap 0..100 (%). Apps may still turn the screen off or dim it temporarily, but cannot exceed the user setting.
void bsp_display_set_master_brightness(uint8_t percent);
uint8_t bsp_display_get_master_brightness(void);

// Panel power: off -> ST7789 SLPIN (0x10) sleep - the internal oscillator stops, power drops to the uA level;
// on -> SLPOUT (0x11) + 120ms wait + DISPON (0x29). DRAM content is retained, so waking shows it immediately,
// no redraw needed. No-op when uninitialized.
void bsp_display_power(bool on);

// ---------------------------------------------------------------------------
// LVGL integration (optional layer). Call only after bsp_display_init() has succeeded.
// Developers who do not want LVGL can ignore this section and draw directly with bsp_display_panel().
// ---------------------------------------------------------------------------
// Forward declaration: in LVGL lv_display_t is `typedef struct _lv_display_t lv_display_t;`,
// so the struct form is enough here, avoiding forcing this header to include lvgl.h.
struct _lv_display_t;

// Start LVGL and its render task, returns lv_display_t*. Returns NULL on failure.
struct _lv_display_t *bsp_lvgl_init(void);

// LVGL is not thread-safe: before and after touching any lv_* object from a [non-LVGL task], lock and unlock are required.
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);

#ifdef __cplusplus
}
#endif
