// components/bsp/src/bsp_display.c
// Ported from trae_card/components/platform/platform_esp32/src/disp_st7789.c
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_pins.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_disp";

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_io;
static bool                      s_bl_ready;
static uint8_t                   s_requested_brightness = 100;
static uint8_t                   s_master_brightness = 80;

// ---------------------------------------------------------------------------
// ST7789P3 vendor-specific init sequence (porch / power / gamma).
// These are [values from the panel vendor reference routine TFT_init()], not generic ST7789 defaults -
// when changing panels you must ask the vendor for a new set; copying this one will most likely misdisplay.
//
// The following four are done by the built-in esp_lcd driver, so they are not repeated here:
//   0x11 SLPOUT / 0x3A COLMOD → esp_lcd_panel_init()
//   0x21 INVON                → esp_lcd_panel_invert_color()
//   0x29 DISPON               → esp_lcd_panel_disp_on_off()
//   0x36 MADCTL               -> esp_lcd_panel_mirror() (WARNING: do not write 0x36 manually again, it would be overridden)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  cmd;
    uint8_t  data[16];
    uint8_t  len;
    uint16_t delay_ms;
} st_init_cmd_t;

static const st_init_cmd_t ST7789P3_CMDS[] = {
    {0xB2, {0x05, 0x05, 0x00, 0x33, 0x33}, 5, 0},   // PORCTRL frame rate porch
    {0xB7, {0x35}, 1, 0},                            // GCTRL gate
    {0xBB, {0x21}, 1, 0},                            // VCOMS
    {0xC0, {0x2C}, 1, 0},                            // LCMCTRL
    {0xC2, {0x01}, 1, 0},                            // VDVVRHEN
    {0xC3, {0x0B}, 1, 0},                            // VRHS
    {0xC4, {0x20}, 1, 0},                            // VDVSET
    {0xC6, {0x0F}, 1, 0},                            // FRCTRL2 60Hz dot inversion
    {0xD0, {0xA7, 0xA1}, 2, 0},                      // PWCTRL1
    {0xD0, {0xA4, 0xA1}, 2, 0},                      // PWCTRL1 (resent from the reference routine, overrides the previous one)
    {0xD6, {0xA1}, 1, 0},
    {0xE0, {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
            0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}, 14, 0},   // PVGAMCTRL positive gamma
    {0xE1, {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
            0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}, 14, 10},  // NVGAMCTRL negative gamma
};

static void backlight_init(void) {
    if (BSP_LCD_BL < 0) { ESP_LOGW(TAG, "Backlight pin not wired to the MCU, brightness is not adjustable"); return; }
    ledc_timer_config_t t = {
        .speed_mode      = BSP_BL_LEDC_MODE,
        .timer_num       = BSP_BL_LEDC_TIMER,
        .duty_resolution = BSP_BL_LEDC_RES,
        .freq_hz         = BSP_BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_USE_XTAL_CLK,
    };
    esp_err_t e = ledc_timer_config(&t);
    if (e != ESP_OK) { ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(e)); return; }

    ledc_channel_config_t ch = {
        .gpio_num   = BSP_LCD_BL,
        .speed_mode = BSP_BL_LEDC_MODE,
        .channel    = BSP_BL_LEDC_CHANNEL,
        .timer_sel  = BSP_BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    e = ledc_channel_config(&ch);
    if (e != ESP_OK) { ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(e)); return; }

    s_bl_ready = true;
    ESP_LOGI(TAG, "Backlight LEDC ready gpio=%d", BSP_LCD_BL);
}

esp_err_t bsp_display_init(void) {
    if (s_panel) return ESP_OK;

    spi_bus_config_t bus = {
        .mosi_io_num = BSP_LCD_MOSI,
        .sclk_io_num = BSP_LCD_SCLK,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_W * 80 * 2,
    };
    esp_err_t e = spi_bus_initialize(BSP_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed (%s) - check whether MOSI=GPIO%d / SCLK=GPIO%d conflict",
                 esp_err_to_name(e), BSP_LCD_MOSI, BSP_LCD_SCLK);
        return e;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BSP_LCD_CS,
        .dc_gpio_num = BSP_LCD_DC,
        .pclk_hz = BSP_LCD_PCLK_HZ,
        .spi_mode = BSP_LCD_SPI_MODE,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .trans_queue_depth = 10,
    };
    e = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST, &io_cfg, &s_io);
    if (e != ESP_OK) { ESP_LOGE(TAG, "panel_io creation failed: %s", esp_err_to_name(e)); return e; }

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = BSP_LCD_RST,          // -1 -> SWRESET soft reset
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    e = esp_lcd_new_panel_st7789(s_io, &dev, &s_panel);
    if (e != ESP_OK) { ESP_LOGE(TAG, "Panel creation failed: %s", esp_err_to_name(e)); return e; }

    esp_lcd_panel_reset(s_panel);   // with rst=-1 this uses SWRESET
    esp_lcd_panel_init(s_panel);    // SLPOUT / COLMOD / RAMCTRL

    for (size_t i = 0; i < sizeof(ST7789P3_CMDS) / sizeof(ST7789P3_CMDS[0]); i++) {
        const st_init_cmd_t *c = &ST7789P3_CMDS[i];
        esp_err_t r = esp_lcd_panel_io_tx_param(s_io, c->cmd, c->data, c->len);
        if (r != ESP_OK) ESP_LOGE(TAG, "Vendor init command 0x%02X failed: %s", c->cmd, esp_err_to_name(r));
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }

    esp_lcd_panel_invert_color(s_panel, BSP_LCD_INVERT_COLOR);   // 0x21 / 0x20
    esp_lcd_panel_mirror(s_panel, false, false);                 // 0x36 MADCTL: this board needs no mirroring (XY double mirror = image rotated 180 deg)
    esp_lcd_panel_set_gap(s_panel, 0, 0);
    esp_lcd_panel_disp_on_off(s_panel, true);                    // 0x29 DISPON

    // After DISPON and before lighting the backlight, first fill the whole GRAM with black.
    // The ST7789 GRAM is uninitialized at power-up and shows white; if at any moment the backlight comes on even slightly earlier than
    // "first frame done", white flashes through. Pin it to black here to remove the source of this kind of white flash.
    {
        esp_err_t fr = bsp_display_fill(0x0000);
        if (fr != ESP_OK) ESP_LOGW(TAG, "Clear screen failed: %s", esp_err_to_name(fr));
    }

    backlight_init();
    ESP_LOGI(TAG, "Display ready %dx%d", BSP_LCD_W, BSP_LCD_H);
    return ESP_OK;
}

esp_lcd_panel_handle_t bsp_display_panel(void) { return s_panel; }

esp_lcd_panel_io_handle_t bsp_display_io(void) { return s_io; }

// Without LVGL, synchronously fill the whole GRAM with one solid color.
//
// Why it is needed: the LVGL side is a **12-row single buffer** (bsp_display_lvgl.c), the full 320 rows must
// be pushed in 27 blocks, and flush is asynchronous. So lv_refr_now() may not finish pushing the first frame - when
// the backlight is lit, the areas not yet pushed show "the uninitialized white GRAM at power-up / the LVGL default white background screen",
// which is the white flash at the instant the boot backlight lights up.
//
// Here esp_lcd_panel_draw_bitmap writes block by block synchronously (no callback -> polling send, returns only after the write),
// guaranteeing the whole screen is one definite color on return. Call it before lighting the backlight and the white flash is gone.
//
// color uses the LVGL RGB565 representation (same order as lv_color_to_u16); the panel direction is swap_bytes,
// so the high/low bytes are swapped here ourselves, with the same effect as .swap_bytes = true in esp_lvgl_port.
esp_err_t bsp_display_fill(uint16_t color) {
    if (!s_panel) return ESP_ERR_INVALID_STATE;

    const int    chunk = 16;                       // 16 rows/block: 16*240*2 = 7.5KB
    const size_t n     = (size_t)BSP_LCD_W * chunk;
    uint16_t    *buf   = (uint16_t *)heap_caps_malloc(n * sizeof(uint16_t),
                                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) return ESP_ERR_NO_MEM;

    const uint16_t be = (uint16_t)((color >> 8) | (color << 8));   // to big-endian
    for (size_t i = 0; i < n; ++i) buf[i] = be;

    esp_err_t ret = ESP_OK;
    for (int y = 0; y < BSP_LCD_H; y += chunk) {
        const int h = (y + chunk <= BSP_LCD_H) ? chunk : (BSP_LCD_H - y);
        esp_err_t r = esp_lcd_panel_draw_bitmap(s_panel, 0, y, BSP_LCD_W, y + h, buf);
        if (r != ESP_OK) { ret = r; break; }
    }
    // Wait for all queued SPI DMA transfers to fully land, to prevent memory freed afterwards from being read by DMA and corrupting data
    if (s_io) {
        esp_lcd_panel_io_tx_param(s_io, 0x00, NULL, 0);
    }
    heap_caps_free(buf);
    return ret;
}

void bsp_display_backlight(uint8_t percent) {
    if (percent > 100) percent = 100;
    s_requested_brightness = percent;
    percent = (uint8_t)(((uint16_t)percent * s_master_brightness) / 100U);
    if (!s_bl_ready) return;
    uint32_t max_duty = (1u << BSP_BL_LEDC_RES) - 1u;
    uint32_t duty = (max_duty * percent) / 100u;
    ledc_set_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL);
}

void bsp_display_set_master_brightness(uint8_t percent) {
    if (percent > 100) percent = 100;
    s_master_brightness = percent;
    bsp_display_backlight(s_requested_brightness);
}

uint8_t bsp_display_get_master_brightness(void) {
    return s_master_brightness;
}

void bsp_display_power(bool on) {
    if (!s_panel || !s_io) return;      // no-op when uninitialized (the init sequence already did DISPON)
    if (on) {
        // Panel power-on SPI activity couples into ADC1_CH0 and corrupts key readings (false UP press -> false PTT).
        // The window covers SLPOUT+120ms+DISPON+LVGL full-screen refresh; during it key detection returns "not pressed".
        bsp_button_suppress_panel_glitch();
        esp_lcd_panel_io_tx_param(s_io, 0x11, NULL, 0);    // SLPOUT wake
        vTaskDelay(pdMS_TO_TICKS(120));                     // wait for the internal clock to settle (required after SLPIN)
        esp_lcd_panel_disp_on_off(s_panel, true);           // DISPON resume display
    } else {
        // The order is critical: first DISPOFF(0x28) to stop the pixel drive, then SLPIN(0x10) to stop the internal oscillator.
        // On the P3 panel, sending only SLPIN was measured not to blank the screen - the liquid crystal is still scanned and driven, leaving image retention.
        esp_lcd_panel_disp_on_off(s_panel, false);          // DISPOFF all pixels off
        esp_lcd_panel_io_tx_param(s_io, 0x10, NULL, 0);     // SLPIN sleep
    }
}
