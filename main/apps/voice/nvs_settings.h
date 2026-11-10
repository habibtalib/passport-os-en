// main/apps/voice/nvs_settings.h -- persistent settings (NVS namespace "app").
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_settings_init(void);

// Time zone offset in hours (int8, +-12; default 8)
esp_err_t nvs_settings_get_tz_hour(int8_t *hour);
esp_err_t nvs_settings_set_tz_hour(int8_t hour);
// Minute-precision time zone (given by the weather API from coordinates, includes +5:30 etc.); returns ESP_ERR_NVS_NOT_FOUND if never stored
esp_err_t nvs_settings_get_tz_min(int16_t *minutes);
esp_err_t nvs_settings_set_tz_min(int16_t minutes);

void nvs_settings_factory_reset(void);   // clear the "app" namespace

#ifdef __cplusplus
}
#endif
