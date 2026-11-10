// Home profile: name/signature/city coordinates are stored in NVS; portrait and pocket card in SPIFFS.
// The phone scales/compresses; the device streams to disk and, for display, decodes a single image in JPEG chunks.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "apps/home/home_portrait.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOME_SHARE_W 240 /* Pocket card: 3:4 full-screen canvas matching the physical screen */
#define HOME_SHARE_H 320
#define HOME_SHARE_MAX_BYTES (24 * 1024) /* Pocket card JPEG limit 24KB (typically 8~14KB) */
#define HOME_SHARE_MAX_IMAGES 6
#define HOME_SHARE_STORAGE_RESERVE (64 * 1024)
#define HOME_SHARE_PATH_MAX 96
#define HOME_NAME_MAX 32      /* UTF-8 bytes incl. trailing 0 (about 9 CJK characters) */
#define HOME_SIGNATURE_MAX 96 /* UTF-8 bytes incl. trailing 0 (about 31 CJK characters) */
#define HOME_CITY_MAX 48      /* UTF-8 bytes incl. trailing 0 */
#define HOME_ADDRESS_MAX 128
#define HOME_COORD_MAX 16     /* "39.9075" string */
#define HOME_REGION_MAX 32

typedef struct {
    char name[HOME_NAME_MAX];
    char signature[HOME_SIGNATURE_MAX];
    char city[HOME_CITY_MAX];
    char address[HOME_ADDRESS_MAX];
    char lat[HOME_COORD_MAX];
    char lon[HOME_COORD_MAX];
    char region[HOME_REGION_MAX]; /* shared weather/radio province */
    char radio_city[HOME_CITY_MAX]; /* parent city when a county is selected */
    char country_code[4];
} home_profile_t;

// Strict decimal coordinates; empty latitude AND longitude disable weather.
bool home_profile_coordinates_valid(const char *lat, const char *lon);

// Read the profile from NVS. The avatar file is not loaded here; SPIFFS is mounted lazily.
void home_profile_init(void);

// Mount the storage partition; if it is unusable (blank or holding another firmware) reformat it, which can take
// over ten seconds. Called once from a background task at boot so formatting does not land in a phone upload request.
void home_profile_storage_prepare(void);

// Thread-safe copy of the current profile (called from both the httpd task and the home task).
void home_profile_get(home_profile_t *out);

// Incremented whenever any profile field (incl. the avatar) changes; the UI uses it to refresh without polling fields.
uint32_t home_profile_version(void);

// Write name/signature/city/coordinates together (any pointer may be NULL meaning unchanged).
// Over-long content is rejected with ESP_ERR_INVALID_ARG; user input is never silently truncated.
esp_err_t home_profile_set_text(const home_profile_t *in);

bool home_profile_has_avatar(void);

// Read the avatar from SPIFFS into dst (needs HOME_AVATAR_BYTES bytes). The caller must not hold
// the LVGL lock (file reads can take tens of ms). The first call mounts the storage partition.
esp_err_t home_profile_read_avatar(uint8_t *dst, size_t cap);

// Streamed avatar write: arguments are supplied by the /api/avatar handler in packet order. Data goes to a tmp file first,
// compatible with legacy 1bpp / 2bpp uploads. Writes the inactive file; the NVS pointer is committed after it is fully written and verified.
esp_err_t home_profile_write_avatar_chunk(const uint8_t *data, size_t len,
                                          bool first, bool last, size_t total_bytes);
void home_profile_abort_avatar_upload(void);

bool home_profile_has_share_image(void);
const char *home_profile_share_image_path(void);

// The pocket card is a 240x320 color JPEG image. The web page does proportional scaling and JPEG compression,
// the device streams it to disk and, for display, uses Tiny JPEG chunked decoding straight to the screen (MCU 16-line buffer only 7.68KB),
// and firmly avoids allocating a 150KB full-screen RGB buffer in C3 SRAM.
esp_err_t home_profile_write_share_image_chunk(const uint8_t *data, size_t len,
                                               bool first, bool last);

typedef struct {
    uint8_t count;
    uint8_t slots[HOME_SHARE_MAX_IMAGES];
    uint32_t bytes[HOME_SHARE_MAX_IMAGES];
    size_t storage_free;
} home_share_gallery_t;
esp_err_t home_profile_gallery_get(home_share_gallery_t *out);
bool home_profile_share_path_at(uint8_t position, char *path, size_t cap);
bool home_profile_share_path_for_slot(uint8_t slot, char *path, size_t cap);
esp_err_t home_profile_write_card_chunk(uint8_t slot, const uint8_t *data, size_t len,
                                       bool first, bool last);
void home_profile_abort_card_upload(void);
/* A unique subset of existing slot IDs: reorder retained images and delete omitted ones. */
esp_err_t home_profile_gallery_order(const uint8_t *slots, size_t count);

#ifdef __cplusplus
}
#endif
