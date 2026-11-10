// main/app_sound.h -- tone module.
// Square-wave tones are output through bsp_audio_write (ported from the play_tone pattern in demo_audio.c).
// Strictly time-multiplexed with recording: sound_worker plays the START tone then posts TONE_DONE, and app_task only opens the stream on receiving it
// (S3 event-driven, app_task no longer blocks synchronously); other tones also go asynchronously through the queue.
#pragma once

#include "app_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Builds the static queue + sound_worker task (prio 3, 2KB stack) and initializes the codec to 16k/16/1.
esp_err_t app_sound_init(void);

// Async playback: enqueued, played by sound_worker; returns whether it was enqueued.
// After the START tone finishes it posts APP_EV_TONE_DONE (the open-stream signal); on false the caller must fall back itself.
bool app_sound_play(app_tone_t tone);

// Sync playback: blocks in the caller's context until done. Kept only for deterministic playback in non-S3 scenarios
// (no callers now, kept to prevent regressions; START is now async).
void app_sound_play_sync(app_tone_t tone);

#ifdef __cplusplus
}
#endif
