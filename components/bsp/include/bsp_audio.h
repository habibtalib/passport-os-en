// components/bsp/include/bsp_audio.h
// ES8311 audio codec: I2C uses the control port (reusing the shared bsp_i2c bus), I2S uses the full-duplex data port.
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the codec and I2S. Calls bsp_i2c_init() internally (idempotent); no need to call it first.
esp_err_t bsp_audio_init(void);

// RX-only, compact DMA ring for 16 kHz microphone streaming with TLS.
// Deinit before changing profiles; bsp_audio_init retains duplex compatibility.
esp_err_t bsp_audio_init_capture(void);
// TX-only, normal playback DMA ring. No microphone channel is allocated.
esp_err_t bsp_audio_init_playback(void);

// Short system feedback may initialize only an idle codec. It must not reuse
// another owner's device. The registry must await its shutdown before an app starts.
esp_err_t bsp_audio_init_feedback(void);

// Close and unload the codec/I2S, returning the heap held by DMA and the codec (for one-shot uses such as the boot sound).
// A later bsp_audio_init() can fully rebuild it. A running App must not call it (it would tear down its audio).
void bsp_audio_deinit(void);
// Final power-off step only: close codec and retain its control lock until sleep.
void bsp_audio_prepare_sleep(void);

// Set the sample format. Repeated calls with the same format are cheap (reuses the already opened codec directly).
//
// WARNING: there is a pitfall to work around: esp_codec_dev_open() returns OK directly when the codec is [already open] and
//   [does not reconfigure the sample rate]. Without a close first, after 16kHz playback an 8kHz stream would go out on a 16k clock -
//   pitch and speed both double. So this function closes then opens when the format changes.
esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch);

// Playback / recording. bytes is the byte count (for 16-bit mono = samples x 2).
esp_err_t bsp_audio_write(const void *pcm, size_t bytes);
esp_err_t bsp_audio_read(void *pcm, size_t bytes);

// Output volume 0..100 (%).
void bsp_audio_set_volume(uint8_t percent);

// Device-wide master volume 0..100 (%). Every volume an App requests is scaled by it, so an App
// cannot overwrite the user device-wide setting with its own default at startup.
void bsp_audio_set_master_volume(uint8_t percent);
uint8_t bsp_audio_get_master_volume(void);

#ifdef __cplusplus
}
#endif
