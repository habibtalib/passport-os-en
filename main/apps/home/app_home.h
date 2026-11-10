// Home (personal page): avatar + name/signature + clock + weather + two stat cards (placeholders, always --).
// This is a full App (not part of the Launcher) and obeys the registry's "stop deletes the screen"
// convention: the Launcher has already released its UI when the app is entered, and on exit this app likewise deletes all its LVGL objects.
#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const passport_app_t g_home_app;

#ifdef __cplusplus
}
#endif
