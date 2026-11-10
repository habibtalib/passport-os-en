// main/app_protocol.h -- device<->Mac JSON protocol (line-delimited).
// Plain C + cJSON (bundled with ESP-IDF; host tests build $IDF_PATH/components/json directly).
#pragma once

#include "app_types.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_PROTO_TX_CAP 512   // serialization buffer limit
#define APP_PROTO_RX_CAP 2048  // parsed line limit

// Parse a Mac->device JSON line. On success fills ev and returns true; unknown type / malformed / invalid field -> false.
bool app_protocol_parse(const char *json, size_t len, app_event_t *ev);

// Protocol event posting (the single posting point for all channels): APPROVAL_REQUEST uses the important path
// (waits 100ms when the queue is full, not dropped -- losing a security approval = the device has no UI to act on), other events
// are zero-blocking and droppable (see the event queue semantics in app_events.h). Review P2-1: fixes an approval request
// being dropped by a full queue during a key storm.
void app_protocol_dispatch_event(const app_event_t *ev);

// Device->Mac serialization. Returns bytes written (excluding NUL); returns 0 on failure.
size_t app_protocol_device_hello(char *buf, size_t cap, int proto);
size_t app_protocol_voice_start(char *buf, size_t cap, const char *audio_format);
size_t app_protocol_voice_end(char *buf, size_t cap);
// Upstream key action (enter/clear, injected by the PC client; see the key-remap task)
size_t app_protocol_key_action(char *buf, size_t cap, app_key_action_t action);
// decision: app_approval_decision_t (approve/reject/details)
size_t app_protocol_agent_action(char *buf, size_t cap, const char *task_id,
                                 uint8_t decision);
// Reconciliation frame sent after voice.end: {"event":"status","drop":n} (dropped-frame reconciliation, see design.md)
size_t app_protocol_device_status(char *buf, size_t cap, uint32_t drop_count);

#ifdef __cplusplus
}
#endif
