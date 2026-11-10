#pragma once

// Plain-HTTP GET to the Clausium Mac app on the LAN (no TLS).

#include <stdbool.h>
#include <stddef.h>
#include "clausium_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAUSIUM_HTTP_OK = 0,
    CLAUSIUM_HTTP_UNREACHABLE,   // DNS, connect, timeout or reset
    CLAUSIUM_HTTP_UNAUTHORIZED,  // 401: wrong token
    CLAUSIUM_HTTP_BAD_RESPONSE,  // other status, truncated or empty body
    CLAUSIUM_HTTP_TOO_LARGE,     // body over max_body
} clausium_http_result_t;

bool clausium_net_connected(void);

// GET http://host:port<path> with "Authorization: Bearer <token>". `body`
// must hold max_body + 2 bytes; on OK it is NUL-terminated and *len is set.
// The HTTP client is always released before this returns.
clausium_http_result_t clausium_http_get(const clausium_config_t *config, const char *path,
                                         char *body, size_t max_body, size_t *len, int timeout_ms);

#ifdef __cplusplus
}
#endif
