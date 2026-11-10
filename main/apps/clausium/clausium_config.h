#pragma once

// Clausium connection settings in NVS namespace "clausium": the Mac's LAN
// address and the 12-character feed token shown in Clausium Settings.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLAUSIUM_HOST_MAX 63
#define CLAUSIUM_TOKEN_LEN 12
#define CLAUSIUM_DEFAULT_PORT 8790

typedef struct {
    char host[CLAUSIUM_HOST_MAX + 1];
    uint16_t port;
    char token[CLAUSIUM_TOKEN_LEN + 1];
} clausium_config_t;

// IPv4 (1-223.x.x.x, not loopback) or an RFC 1123 hostname such as mac.local.
bool clausium_host_valid(const char *host);
// Accepts 12 base32 characters (A-Z, 2-7) in any case; writes them upper case.
bool clausium_token_normalize(const char *in, char out[CLAUSIUM_TOKEN_LEN + 1]);

// Reads the stored settings. Returns true only when host and token are both
// set and valid; `out` then holds a usable configuration. Port defaults to 8790.
bool clausium_config_load(clausium_config_t *out);
// Status for the phone page; never exposes the token.
void clausium_config_status(char *host, size_t host_cap, uint16_t *port, bool *token_set);
// Saves host and port. A NULL or empty token keeps the stored one; the first
// save must include a token. An empty host clears host and token.
esp_err_t clausium_config_save(const char *host, uint16_t port, const char *token);

// Background alerts (Settings > Clausium Alerts). Default on.
bool clausium_config_alerts_enabled(void);
esp_err_t clausium_config_set_alerts(bool enabled);

#ifdef __cplusplus
}
#endif
