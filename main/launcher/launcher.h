#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize Launcher UI resources
void launcher_ui_init(void);

// Show the Launcher main screen (hide sub-app screens)
void launcher_ui_show(void);

// Hide the Launcher main screen (call before entering a sub-app)
void launcher_ui_hide(void);

// Launcher key handling (UP/DOWN flip cards, OK enters)
void launcher_ui_on_key(uint8_t btn, app_btn_event_t event);

// Update status bar info (battery, time)
void launcher_ui_update_status(void);

#ifdef __cplusplus
}
#endif
