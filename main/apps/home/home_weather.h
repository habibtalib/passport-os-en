// Home weather: Open-Meteo (no API key) current temperature + WMO weather code, mapped to short English phrases.
// Coordinates are geocoded in the browser by the upload page and stored in the profile; the device only makes one small request.
// The last successful result is cached in NVS, so the latest weather shows even when booting offline.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    int16_t temp_c;    /* Celsius, rounded */
    uint16_t code;     /* WMO weather code */
    uint8_t humidity;  /* Relative humidity % */
    uint8_t wind_kmh;  /* 10 m wind speed km/h (rounded) */
    uint16_t wind_dir; /* 10 m wind direction, degrees (0=north, clockwise) */
    uint32_t updated_at; /* UTC seconds of the successful fetch; 0 = unknown (clock was unsynced) */
} home_weather_t;

// Open-Meteo/CAMS model concentration, not a Chinese AQI or indoor measurement.
// Volatile only: reuse the same city's sample for 30 minutes across app visits.
// Failed requests, city changes, expiry and device restarts invalidate it.
typedef struct {
    bool valid;
    uint32_t pm25_x10; /* tenths of micrograms per cubic metre */
} home_air_quality_t;

bool home_air_quality_get(home_air_quality_t *out);
esp_err_t home_air_quality_refresh(const char *lat, const char *lon);
void home_air_quality_label(bool connected, char *out, size_t capacity);
// Cancel bounded asynchronous HTTPS work before the Home task is stopped.
void home_weather_allow_requests(void);
void home_weather_cancel_requests(void);

// Read the NVS cache. The UI immediately has a "last weather" to show.
void home_weather_init(void);

bool home_weather_get(home_weather_t *out);

// WMO code -> short English phrase.
const char *home_weather_code_text(uint16_t code);

// Beaufort scale: km/h -> 0..12.
int home_weather_beaufort(uint8_t wind_kmh);

// Wind direction -> compass point ("N"/"SE"). Calm wind is handled by the caller.
const char *home_weather_wind_dir_text(uint16_t deg);

// Blocking fetch (lat/lon are decimal strings). Call from the home background task, do not hold the LVGL lock.
esp_err_t home_weather_refresh(const char *lat, const char *lon);

// Data age phrase: "Just now"/"25m ago"/"3h ago"/"2d ago"; when updated_at or now is unknown
// returns "Offline". Pure function, easy to host-test.
void home_weather_age_text(uint32_t updated_at, int64_t now, char *out, size_t capacity);

// Incremented on every successful refresh; the UI uses it to refresh the weather row.
uint32_t home_weather_version(void);

#ifdef __cplusplus
}
#endif
