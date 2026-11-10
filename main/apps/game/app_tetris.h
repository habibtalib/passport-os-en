// Tetris app.
// Memory policy (same discipline as the voice-controlled flyer):
//   1. The only big object is the 80x160 RGB565 board canvas = 25.6KB, heap-allocated in start() / freed in stop()
//      (largest contiguous block before entry ~106KB, 4x headroom); zero heap allocation during play;
//   2. Game loop = 50ms lv_timer, no extra task; logic lives in tetris_core (pure integers);
//   3. The game-over panel is shown/hidden by "moving off-screen" -- the HIDDEN flag path has a dead-loop landmine in LVGL 9.3 (see app_game);
//   4. After stop() the heap returns to its pre-entry level.
#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const passport_app_t g_tetris_app;

#ifdef __cplusplus
}
#endif
