// Voice-controlled mic capture: reads the mic at 16kHz/16bit/mono, outputs a smoothed loudness of 0..4095.
// A small independent task (3KB stack); it shares only one atomic variable with LVGL -- the game loop has no locks or waits.
//
// 2026-09-13 optimization:
//   * Added game_mic_ready() -- tells "quiet room" apart from "mic not warmed up yet", so the upper layer can
//     decide whether to show a hint instead of misreading the warm-up 0 as "player is not barking";
//   * game_mic_start() is now reliably re-entrant: when re-entering the app right after exit, if the old task is
//     still winding down (s_task non-null) it waits for it to exit before creating a new task; otherwise task creation
//     was silently skipped -- leaving the round with no voice control at all (hit before).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the capture task (idempotent; if the previous task has not exited yet, wait for it first).
// Internal bsp_audio_init/set_format(16k) use the same config as the voice app;
// apps run mutually exclusively, so there is no I2S contention.
void game_mic_start(void);

// Request exit and wait for the task to finish (each read takes up to 16ms; usually returns in <50ms).
void game_mic_stop(void);
bool game_mic_prepare_stop(void);

// Smoothed loudness (mean-rectified x EMA). A silent room reads a few hundred, normal speech a few thousand.
// Constantly 0 while not ready (warm-up).
uint16_t game_mic_level(void);

// Whether the mic has passed the power-on transient and its output is trustworthy. Warm-up takes ~160ms.
bool game_mic_ready(void);

#ifdef __cplusplus
}
#endif
