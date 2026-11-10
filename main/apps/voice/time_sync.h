// main/time_sync.h -- wall-clock time sync (the time source is the PC client only, dual channel BLE/USB).
// Design notes (see design.md):
//   - s_epoch (monotonic cache) is the single source of truth: settimeofday also syncs system time (for a future log
//     timestamp option), UI/formatting always read the cache -- host tests have zero system-time side effects;
//   - time zone is a minute offset: the home weather gives it automatically from the city coordinates (follows you abroad / across zones),
//     NVS stores tz_min; the old whole-hour tz_hour is still supported. No TZ env;
//   - not synced after reboot: valid=false -> UI shows "--:--", restored once the relay connects.
#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t time_sync_init(void);                // read NVS time zone (default +8); sync state = not synced
void time_sync_set_epoch(int64_t epoch_sec);   // sync time (UTC seconds); sets valid + syncs system time
bool time_sync_valid(void);                    // whether time is synced (UI "--:--" check)
int64_t time_sync_last_epoch(void);
int time_sync_tz_hour(void);                   // current time zone offset in hours (default 8)
esp_err_t time_sync_set_tz(int hour);          // set the time zone offset (+-12 validated, persisted to NVS)
int time_sync_tz_offset_sec(void);             // current time zone offset in seconds (local = UTC + this value)
// Offset given by the weather API (minutes, -720..840); not written to NVS if equal to the current one.
esp_err_t time_sync_set_tz_offset_min(int minutes);
int time_sync_format_local(char *buf, size_t cap); // "HH:MM" (synced) or "--:--" (not synced)

#ifdef __cplusplus
}
#endif

#endif /* TIME_SYNC_H */
