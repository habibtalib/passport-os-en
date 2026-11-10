// Boot Easter egg beyond the Home profile: retro 8-bit boot sound (E5-G5-B5-E6 square-wave arpeggio).
// Purely synthesized, no audio assets; plays in a dedicated short-lived task and calls bsp_audio_deinit right after playback,
// returning codec/DMA memory to the system - boot only borrows it, never holds it.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create a one-shot task to play the boot sound (async, does not block startup). Repeated calls are harmless.
void boot_chime_start(void);
bool boot_chime_busy(void);

#ifdef __cplusplus
}
#endif
