// WeType remote: a single screen with link/readiness/PTT telemetry.
#pragma once

#include "app_types.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Builds the single-page microphone remote UI. Call only after bsp_display_init + bsp_lvgl_init succeed,
// and the caller must hold the LVGL lock (bsp_lvgl_lock).
esp_err_t app_ui_init(void);
void app_ui_deinit(void);

void app_ui_show(void);
void app_ui_hide(void);

// Refresh the UI from a snapshot. Also must be called with the lock held (sole writer: app_task).
// Whole-device screen off / wake is managed by app_registry.
void app_ui_render(const app_ui_snapshot_t *snap);

#ifdef __cplusplus
}
#endif
