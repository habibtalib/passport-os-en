// components/bsp/src/bsp_display_lvgl.c
// LVGL integration lives in its own file: developers not using LVGL can delete this file + the two dependencies in idf_component.yml.
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_timer.h"

static const char *TAG = "bsp_lvgl";

static lv_display_t *s_disp;

static uint32_t monotonic_tick_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

lv_display_t *bsp_lvgl_init(void) {
    if (s_disp) return s_disp;
    if (!bsp_display_panel()) {
        ESP_LOGE(TAG, "Call bsp_display_init() successfully first");
        return NULL;
    }

    lvgl_port_cfg_t pc = ESP_LVGL_PORT_INIT_CONFIG();
    // The default 7168 is too small: weather icon JPEGs go through LVGL TJPEG decoder_info/open (file source),
    // TJPGD_WORKBUFF_SIZE alone is 4KB on the stack (lv_tjpgd.c:110); stacked with the render call chain it was measured to
    // overflow the stack (Stack protection fault, 2026-09-13, the crashing task was taskLVGL).
    pc.task_stack = 12288;
    if (lvgl_port_init(&pc) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed");
        return NULL;
    }

    // The port's default 5ms tick wakes ESP32-C3 200 times/second even asleep.
    // Read the monotonic clock on demand, but leave the handler enabled:
    // a disabled lv_timer_handler returns a 1ms polling delay.
    lvgl_port_lock(0);
    if (lvgl_port_stop() == ESP_OK) lv_tick_set_cb(monotonic_tick_ms);
    lv_timer_enable(true);
    lvgl_port_unlock();

    const lvgl_port_display_cfg_t dc = {
        .panel_handle = bsp_display_panel(),
        .io_handle    = bsp_display_io(),
        // 12-row single buffer (~5.6 KiB) leaves enough internal RAM for the
        // full-screen I4 Codex canvas and NimBLE on ESP32-C3 without PSRAM.
        .buffer_size   = (uint32_t)BSP_LCD_W * 12,
        .double_buffer = false,
        .hres = BSP_LCD_W, .vres = BSP_LCD_H,
        // Rotation/mirroring must be configured here: esp_lvgl_port re-sends MADCTL when it registers the display,
        // overriding the esp_lcd_panel_mirror() setting in bsp_display.c.
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        // swap_bytes: LVGL outputs little-endian RGB565, the ST7789 over SPI wants big-endian -> the high/low bytes must be swapped.
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    s_disp = lvgl_port_add_disp(&dc);
    if (!s_disp) { ESP_LOGE(TAG, "lvgl_port_add_disp failed"); return NULL; }

    // Paint the LVGL [default screen] black.
    // The LVGL default theme screen background is near white; during boot (before the boot animation / during transitions)
    // whenever the default screen briefly becomes the active screen and is drawn into GRAM, turning the backlight on gives a white flash.
    // Pin it to black uniformly to eliminate this whole class of white flash - same color as the app_registry transition screen.
    lv_obj_t *scr = lv_screen_active();
    if (scr) {
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    }

    ESP_LOGI(TAG, "LVGL ready");
    return s_disp;
}

bool bsp_lvgl_lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }
void bsp_lvgl_unlock(void)         { lvgl_port_unlock(); }
