// main/audio_streamer.h -- streaming audio pipeline.
// Session-owned ring buffer (NOSPLIT, two slots) + two workers: audio capture -> ring -> channel send.
// init allocates the buffers and worker stacks once, with no allocation per recording; deinit frees them after the tasks have stopped.
// On congestion the source drops frames and reports AUDIO_DROP_* events; the drop count feeds the status frame after voice.end for reconciliation.
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Audio frame send function (channel-agnostic): BLE -> ble_audio_notify_audio; USB -> usb_link_send_audio.
// Payload: in compressed mode (ble) an 804B IMA ADPCM block (4B header + 800B data); uncompressed
// (usb) a raw 3200B PCM block. The 4B session token of an in-ring item is in-ring metadata and
// does not go on the link. The App dispatches parsing on the audio field of voice.start.
// Returns 0 = sent; non-zero = send failed (dropped-frame counting path). When unregistered (NULL) it is always treated as a failure.
typedef int (*audio_send_fn_t)(const uint8_t *data, size_t len);

// Register the send function. Call sites: main.c boot and after a mode switch, registering for the current channel; before a switch the link is already down
// and the pipeline stopped, so there is no concurrent pointer change (design in mode.c).
void audio_streamer_set_sender(audio_send_fn_t fn);
// Compression switch (mode-driven, BLE only): true -> every 3200B PCM block is encoded into one 804B ADPCM
// block and sent (4:1, 8KB/s); false -> sent as is (USB bandwidth is ample, no compression).
void audio_streamer_set_compressed(bool on);

esp_err_t audio_streamer_init(void);   // build the ring buffer and two workers (idle, waiting)
bool audio_streamer_deinit(void);      // after both workers stop, return the whole session's memory
// Start capture and sending (clears the session drop count). A session is identified by an incrementing token, in-ring item headers carry a
// snapshot token; leftovers from an old session (in ring / in flight / late blocks) are silently invalidated and
// dropped by ble_worker according to the token -- start is never refused because of leftovers and can reopen at once without draining.
// Returns: ESP_OK on success; ESP_ERR_INVALID_STATE if the pipeline is not ready. The caller must check the return value:
// on failure recording did not start and the UI must not show recording (REVIEW P2-C: go back to READY + toast).
esp_err_t audio_streamer_start(void);
void audio_streamer_stop(void);        // stop capture (the send worker keeps draining, drain is required before voice.end)
// Cancel / link lost: invalidate the session token + stop capture; leftovers in the ring / in flight / late frames are silently invalidated by ble_worker
// according to the token -- returns fast (does not wait for the ring to empty, no drain gate).
// Idempotent: equally valid when capture is already stopped (link lost after STOP; the leftover token is already invalidated).
void audio_streamer_cancel(void);
// Wait until the data left in the ring has been sent (at most ms). Call before voice.end to guarantee frame order.
void audio_streamer_drain(uint32_t ms);
bool audio_streamer_active(void);
void audio_streamer_release_audio(void);
uint16_t audio_streamer_peak(void);    // peak sample of the most recent block (for the UI volume bar)
// Take away this session's (since the last start) cumulative dropped-frame count and reset it to zero. main.c takes it after the drain of SEND_VOICE_END
// (capture stopped + ring drained, count stable) and reports it to the Mac in the status frame after voice.end (dropped-frame reconciliation).
uint32_t audio_streamer_take_drops(void);

#ifdef __cplusplus
}
#endif
