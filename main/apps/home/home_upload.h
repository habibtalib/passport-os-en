// Phone setup service: the only HTTP server while the hotspot is up; keeps serving profile entry after the network connects.
// The phone joins the device hotspot and opens 192.168.4.1 to set up Wi-Fi, profile and upload images.
// Image scaling and JPEG compression happen in the browser; the device receives uploads through a 512B streaming buffer.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the setup service (idempotent). Call StartConfigAp(true) first; the hotspot address is available after it returns.
esp_err_t home_upload_start(void);

// Stop the service and reclaim httpd memory (must be called when leaving upload mode / leaving home).
void home_upload_stop(void);

bool home_upload_active(void);
bool home_upload_busy(void);
bool home_upload_network_verified(void);
bool home_upload_finished(void);

// "http://192.168.x.x" (only valid while the service is running)
const char *home_upload_url(void);

#ifdef __cplusplus
}
#endif
