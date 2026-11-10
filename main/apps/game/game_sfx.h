// "Bark Battle" sound effects (game_sfx): 16kHz square wave, reuses the bsp_audio TX channel.
//
// Why a separate module instead of reusing app_sound from apps/voice:
//   app_sound depends on the voice app_events/app_types, and it runs bsp_audio_init
//   + set_format again -- opening/closing the codec concurrently with the mic task races.
//   This module only does bsp_audio_write (the codec is already opened by the mic task;
//   full duplex, TX/RX are two independent I2S handles) and skips silently when the codec
//
// Cost: one static-priority task with a 2KB stack, alive only while the game is in the
// foreground; play requests use task notifications, no queue RAM, and never block the LVGL main loop (a 60ms square wave would drop 2 frames).
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GS_NONE = 0,
    GS_START,   /* game start: short rising blip */
    GS_SCORE,   /* passed a pipe: one crisp beep */
    GS_DEAD,    /* death: two descending tones */
    GS_BEST,    /* new best: three-tone rising fanfare */
    GS_COUNT,
} game_sfx_t;

// Create the playback task (idempotent). Failure only affects sound, not gameplay.
void game_sfx_start(void);

// Delete the playback task (idempotent). Called on game exit to reclaim the stack.
void game_sfx_stop(void);
bool game_sfx_prepare_stop(void);

// Request one tone asynchronously; silently dropped if not started/busy (sound must never slow the main loop).
void game_sfx_play(game_sfx_t id);

#ifdef __cplusplus
}
#endif
