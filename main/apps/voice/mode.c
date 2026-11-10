// main/apps/voice/mode.c -- link channel implementation
//
// 2026-09-12: the channel moved from BLE to Wi-Fi UDP (contract: docs/voice-udp-channel.md).
// This file degraded from a "BLE/USB dual-channel abstraction" to a "single-channel forwarder":
//   - the old mode_channel_up(APP_CHAN_USB) was always false (the old mode.c just returned false,
//     and the repo never had a usb_link implementation) -> the USB branch was removed entirely, it never worked;
//   - all former ble_audio_* calls became udp_audio_*;
//   - the external signatures stay as in mode.h -- app_state.c's state machine and the host tests need no changes.
#include "mode.h"
#include "audio_streamer.h"
#include "udp_audio.h"

bool mode_link_up(void)
{
    return udp_audio_event_subscribed();
}

bool mode_channel_up(uint8_t chan)
{
    // Single channel: wireless (APP_CHAN_BLE) is the UDP channel. USB was removed, always false.
    return chan == APP_CHAN_BLE && udp_audio_event_subscribed();
}

int mode_send_event_line(uint8_t chan, const char *line, size_t len)
{
    if (chan != APP_CHAN_BLE) return -1;
    return udp_audio_notify_event(line, len);
}

int mode_send_event_line_important(uint8_t chan, const char *line, size_t len,
                                   uint32_t timeout_ms)
{
    if (chan != APP_CHAN_BLE) return -1;
    return udp_audio_notify_event_blocking(line, len, timeout_ms);
}

const char *mode_audio_format(uint8_t chan)
{
    (void)chan;
    // Over UDP each frame is still an independent IMA ADPCM block (4:1, 8KB/s): the block carries its own
    // predictor/index header, so losing a block costs only 100ms and never loses sync -- a natural fit
    // for the UDP loss model, better than the intuition "high bandwidth means no compression" (see docs/voice-udp-channel.md section 6).
    return "ima_adpcm";
}

void mode_select_audio_sender(uint8_t chan)
{
    (void)chan;
    audio_streamer_set_sender(udp_audio_notify_audio);
    audio_streamer_set_compressed(true);
}

bool mode_wired(void)
{
    return false;   // USB channel removed
}
