// main/apps/voice/udp_audio.h -- Wi-Fi UDP audio/event channel (replaces BLE).
//
// Contract: see docs/voice-udp-channel.md -- shared by the device and companion sides, a change on one side
// must be mirrored on the other. Key points:
//
//   port UDP 33333 (bidirectional, same port)
//   companion = server: bind 33333, broadcast a beacon to 255.255.255.255:33333 every 1000ms
//   device    = client: bind 33333 (receives the beacon), after learning the PC IP unicasts to <pc_ip>:33333
//
//   packet = [1B type][payload]
//     0x01 AUDIO  device->PC  [seq][0x80] + 804B IMA ADPCM block  = 807B
//     0x02 EVENT  both ways   UTF-8 JSON line (with trailing '\n')   <= 513B
//     0x03 BEACON PC->device  {"v":1,"role":"pc","port":33333}     ~40B
//     0x04 PING   device->PC  empty payload, sent by this side every 2s, used only for liveness   = 1B
//
// Why PING exists: when the device is idle (not recording, no events) it sends nothing, and the companion
// cannot tell "the device is still in the voice app" from "the device left the voice app". One every 2s is enough to make
// the companion's link liveness check (6s of silence = disconnected) work properly.
//
// AUDIO keeps the BLE-era 2B fragment header ([block seq][fragment seq|0x80 last fragment]): over UDP it is always
// "single fragment + last fragment" -> [seq][0x80]. It is kept so the companion's reassemble_adpcm()
// needs no change at all; seq is ++ mod 256 per block, and duplicate/late blocks are dropped by the companion's existing last_seq logic.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_UDP_PORT      33333
#define VOICE_UDP_T_AUDIO   0x01
#define VOICE_UDP_T_EVENT   0x02
#define VOICE_UDP_T_BEACON  0x03
#define VOICE_UDP_T_PING    0x04   // device->PC liveness (the companion does not dispatch it, it only refreshes the link timestamp)
#define VOICE_UDP_PING_MS   2000u  // liveness period (the companion side treats 6s of silence as disconnected)

// How long peer silence means the link is down (3:1 redundancy against the 1000ms beacon period)
#define VOICE_UDP_PEER_TIMEOUT_MS 3000u

// ---- Lifecycle ----
// Create the UDP socket, bind 33333 and start the RX task. Idempotent: returns 0 immediately if already initialized.
// Returns 0 = success; non-zero = socket/bind/task creation failed.
int udp_audio_init(void);
// Stop the RX task and close the socket (set the stop flag and wait for the task to exit first, then close the socket, avoiding a recvfrom race).
// Idempotent: calling it uninitialized has no side effects.
int udp_audio_deinit(void);

// ---- Uplink (maps one-to-one to the old ble_audio_* semantics) ----
// Audio block: frame = the 804B IMA ADPCM block produced by audio_streamer (not 3200B PCM).
// Assembles [0x01][seq][0x80][block] and unicasts. Returns non-zero if the peer is unknown / the send failed (the caller counts a dropped frame).
int udp_audio_notify_audio(const uint8_t *frame, size_t len);
// Event line (<=512B, with trailing '\n'): assembles [0x02][line] and unicasts. Non-blocking.
int udp_audio_notify_event(const char *line, size_t len);
// UDP has no send queue or backpressure, so this function is equivalent to udp_audio_notify_event -- the signature is kept only
// so the call sites of the session-boundary frames (voice.start/end) in app_voice.cc need no change.
int udp_audio_notify_event_blocking(const char *line, size_t len, uint32_t timeout_ms);

// ---- Status queries ----
bool udp_audio_connected(void);       // peer is known and not timed out
bool udp_audio_event_subscribed(void); // basis for link_up (same meaning as connected, UDP has no subscription concept)
uint32_t udp_audio_audio_drops(void);  // cumulative dropped audio frames (peer unknown / send failed)
uint32_t udp_audio_event_drops(void);  // cumulative dropped event lines
// Dotted-string IP of the learned peer; returns "0.0.0.0" if not learned (for diagnostics / console)
const char *udp_audio_peer_ip(void);

#ifdef __cplusplus
}
#endif
