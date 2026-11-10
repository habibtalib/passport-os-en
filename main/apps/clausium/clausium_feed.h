#pragma once

// Clausium -> Passport feed v1 (see Clausium PassportStatus.swift).
// Pure C: validates the compact JSON into fixed arrays. Host-testable.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLAUSIUM_FEED_MAX_BYTES 3072
#define CLAUSIUM_NEED_MAX 6
#define CLAUSIUM_LIVE_MAX 8
#define CLAUSIUM_PLAN_MAX 4
// Maximum string lengths in bytes, excluding the terminating NUL.
#define CLAUSIUM_PROJECT_MAX 24
#define CLAUSIUM_PROFILE_MAX 16
#define CLAUSIUM_ASK_MAX 60
#define CLAUSIUM_LABEL_MAX 32

typedef enum {
    CLAUSIUM_NEED_PERMISSION = 0,
    CLAUSIUM_NEED_QUESTION,
    CLAUSIUM_NEED_DONE,
    CLAUSIUM_NEED_ERROR,
} clausium_need_kind_t;

typedef enum {
    CLAUSIUM_LIVE_WORKING = 0,
    CLAUSIUM_LIVE_IDLE,
    CLAUSIUM_LIVE_NEEDS,
    CLAUSIUM_LIVE_DONE,
    CLAUSIUM_LIVE_ERROR,
} clausium_live_status_t;

typedef struct {
    char project[CLAUSIUM_PROJECT_MAX + 1];
    char profile[CLAUSIUM_PROFILE_MAX + 1];
    char ask[CLAUSIUM_ASK_MAX + 1]; // empty when the feed omits `a`
    uint8_t kind;                   // clausium_need_kind_t
} clausium_need_t;

typedef struct {
    char project[CLAUSIUM_PROJECT_MAX + 1];
    char profile[CLAUSIUM_PROFILE_MAX + 1];
    char label[CLAUSIUM_LABEL_MAX + 1];
    uint8_t status;                 // clausium_live_status_t
} clausium_live_t;

typedef struct {
    char profile[CLAUSIUM_PROFILE_MAX + 1];
    int8_t h5_used;                 // percent used 0..100, -1 unknown
    int8_t week_used;               // percent used 0..100, -1 unknown
    uint32_t h5_reset;              // epoch seconds, 0 unknown
    uint32_t week_reset;            // epoch seconds, 0 unknown
} clausium_plan_t;

typedef struct {
    uint32_t built_at;              // `t`, Mac clock epoch seconds
    uint16_t wait, work, live_total; // `n`: full counts before caps
    uint8_t need_count, live_count, plan_count;
    clausium_need_t need[CLAUSIUM_NEED_MAX];
    clausium_live_t live[CLAUSIUM_LIVE_MAX];
    clausium_plan_t plan[CLAUSIUM_PLAN_MAX];
} clausium_state_t;

typedef enum {
    CLAUSIUM_FEED_OK = 0,
    CLAUSIUM_FEED_TOO_LARGE,  // body over CLAUSIUM_FEED_MAX_BYTES
    CLAUSIUM_FEED_SYNTAX,     // not one complete JSON object
    CLAUSIUM_FEED_VERSION,    // `v` missing or not 1
    CLAUSIUM_FEED_INVALID,    // a field has a wrong type, range or length
} clausium_feed_result_t;

// `body` must hold `len` bytes followed by a NUL. `out` is fully written on
// success and zeroed on failure. Unknown keys are ignored. Array items beyond
// the caps are ignored without validation.
clausium_feed_result_t clausium_feed_parse(const char *body, size_t len, clausium_state_t *out);

// Alert endpoint (/passport/v1/alert?since=<seq>): tiny event log.
#define CLAUSIUM_ALERT_MAX_BYTES 511
#define CLAUSIUM_ALERT_EVENTS_MAX 3
#define CLAUSIUM_BOOT_LEN 6

typedef struct {
    uint32_t seq;                   // `q`
    char project[CLAUSIUM_PROJECT_MAX + 1];
    char profile[CLAUSIUM_PROFILE_MAX + 1];
    uint8_t kind;                   // clausium_need_kind_t
} clausium_event_t;

typedef struct {
    char boot[CLAUSIUM_BOOT_LEN + 1]; // 6 lowercase hex chars per Mac app launch
    uint32_t seq;
    uint16_t wait;
    uint8_t event_count;
    clausium_event_t event[CLAUSIUM_ALERT_EVENTS_MAX]; // oldest first
} clausium_alert_msg_t;

clausium_feed_result_t clausium_alert_parse(const char *body, size_t len, clausium_alert_msg_t *out);

#ifdef __cplusplus
}
#endif
