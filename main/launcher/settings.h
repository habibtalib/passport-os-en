#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call after NVS init and before Launcher creation: restore device-wide settings and apply them immediately.
void settings_bootstrap(void);
void settings_tick(void);

// Shared by Settings and app volume keys; applies and persists only volume.
void settings_set_volume(uint8_t percent);

extern const passport_app_t g_settings_app;

#ifdef __cplusplus
}
#endif
