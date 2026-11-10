// Home clock: once Wi-Fi is ready, sync the system clock via SNTP; the UI formats the date from
// system time + local timezone offset. The timezone reuses the NVS setting of time_sync (voice app), consistent system-wide.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Idempotent SNTP start (network must be ready). Repeated calls are harmless.
void home_clock_start(void);

// Stop SNTP and release the network dependency (call when leaving home). Already-synced system time keeps running.
void home_clock_stop(void);

// Whether the system clock is synced (after 2023 counts as valid; either PC time sync or SNTP works).
bool home_clock_valid(void);

// "12:34" / "--:--"
void home_clock_format_time(char *buf, size_t cap);

// "Sep 12"
void home_clock_format_date(char *buf, size_t cap);

// "Sat".."Sun"
void home_clock_format_weekday(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
