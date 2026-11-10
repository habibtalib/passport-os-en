// "Bark Battle": a voice-controlled, addictive game app.
// Memory policy (hard launch constraint):
//   1. Zero big buffers -- no canvas/framebuffer; the character and pipes are all small resident object pools (~40
//      lv_obj, ~5KB), with zero heap allocation during play;
//   2. No separate render task -- the game loop is a 33ms lv_timer running in the LVGL task;
//      the only extra task is mic capture (3KB stack, compute as it reads, one atomic variable shared with the UI);
//   3. stop() reclaims everything: delete the timer first, then tear down the screen, return after the mic task is joined, heap back to its pre-entry level.
#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const passport_app_t g_game_app;

#ifdef __cplusplus
}
#endif
