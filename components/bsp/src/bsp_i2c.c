// components/bsp/src/bsp_i2c.c
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"

static const char *TAG = "bsp_i2c";

static i2c_master_bus_handle_t s_bus;

esp_err_t bsp_i2c_init(void) {
    if (s_bus) return ESP_OK;                 // idempotent
    i2c_master_bus_config_t cfg = {
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&cfg, &s_bus);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus creation failed (%s) - check whether SDA=GPIO%d / SCL=GPIO%d is taken by another peripheral",
                 esp_err_to_name(e), BSP_I2C_SDA, BSP_I2C_SCL);
        s_bus = NULL;
        return e;
    }
    ESP_LOGI(TAG, "I2C ready SDA=GPIO%d SCL=GPIO%d", BSP_I2C_SDA, BSP_I2C_SCL);
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_bus(void) { return s_bus; }

esp_err_t bsp_i2c_scan(void) {
    // WARNING: never open a temporary extra bus for scanning just to "isolate NACKs" - it would kill the whole I2C:
    //   a second i2c_new_master_bus() on the same port necessarily fails, but the "already acquired" branch in i2c_common.c
    //   still hands the [real bus] bus object out of *i2c_new_bus;
    //   right after, the error cleanup path i2c_master_bus_destroy() takes it and calls i2c_common_deinit_pins(),
    //   unbinding SDA/SCL from the I2C peripheral. After that every transaction hangs on "clear bus failed",
    //   and the ES8311 and CW2017 both go missing together.
    //   (Measured on IDF v5.5.3; the symptom is a screen full of "reset hardware failed", extremely hard to trace back to the scan step.)
    //
    // Scanning directly on the real bus is safe: i2c_master_probe() resets status to
    // I2C_STATUS_DONE at the end ("in order not influence next time transaction"), NACK leaves no residue;
    // the 100kHz timing it sets temporarily is also overridden by the next device transaction according to its own scl_speed_hz.
    if (!s_bus) {
        ESP_LOGE(TAG, "Call bsp_i2c_init() successfully first");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "I2C scan start:");
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            const char *who = (addr == BSP_I2C_ES8311_ADDR) ? "  <- ES8311 audio codec"
                            : (addr == BSP_I2C_CW2017_ADDR) ? "  <- CW2017 fuel gauge"
                            : "";
            ESP_LOGI(TAG, "  found device @ 0x%02X%s", addr, who);
            found++;
        }
    }
    if (found == 0) ESP_LOGW(TAG, "  no I2C device found - check wiring, pull-up resistors and power");
    else            ESP_LOGI(TAG, "I2C scan done, %d device(s)", found);
    return ESP_OK;
}
