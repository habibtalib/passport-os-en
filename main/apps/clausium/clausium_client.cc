#include "clausium_client.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "wifi_manager.h"

#include <cstdio>
#include <cstring>

namespace {
constexpr char kTag[] = "clausium_http";
}

bool clausium_net_connected(void) { return WifiManager::GetInstance().IsConnected(); }

clausium_http_result_t clausium_http_get(const clausium_config_t *config, const char *path,
                                         char *body, size_t max_body, size_t *len, int timeout_ms) {
    *len = 0;
    body[0] = 0;
    char url[CLAUSIUM_HOST_MAX + 64];
    std::snprintf(url, sizeof(url), "http://%s:%u%s", config->host, (unsigned)config->port, path);
    char auth[8 + CLAUSIUM_TOKEN_LEN + 1];
    std::snprintf(auth, sizeof(auth), "Bearer %s", config->token);

    esp_http_client_config_t http = {};
    http.url = url;
    http.timeout_ms = timeout_ms;
    http.buffer_size = 1024;
    http.buffer_size_tx = 512;
    http.disable_auto_redirect = true;
    http.keep_alive_enable = false;
    http.transport_type = HTTP_TRANSPORT_OVER_TCP;
    esp_http_client_handle_t client = esp_http_client_init(&http);
    std::memset(url, 0, sizeof(url));
    if (!client) {
        std::memset(auth, 0, sizeof(auth));
        return CLAUSIUM_HTTP_UNREACHABLE;
    }
    esp_http_client_set_header(client, "Authorization", auth);
    std::memset(auth, 0, sizeof(auth));

    clausium_http_result_t result = CLAUSIUM_HTTP_UNREACHABLE;
    int status = 0;
    size_t total = 0;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        const int64_t length = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (length < 0) {
            result = CLAUSIUM_HTTP_UNREACHABLE;
        } else if (status == 401) {
            result = CLAUSIUM_HTTP_UNAUTHORIZED;
        } else if (status != 200) {
            result = CLAUSIUM_HTTP_BAD_RESPONSE;
        } else if (length > static_cast<int64_t>(max_body)) {
            result = CLAUSIUM_HTTP_TOO_LARGE;
        } else {
            result = CLAUSIUM_HTTP_OK;
            // Read one byte past the limit so an unannounced oversize body is caught.
            while (total <= max_body) {
                const int n = esp_http_client_read(client, body + total,
                                                   static_cast<int>(max_body + 1 - total));
                if (n < 0) { result = CLAUSIUM_HTTP_UNREACHABLE; break; }
                if (n == 0) break;
                total += static_cast<size_t>(n);
            }
            if (result == CLAUSIUM_HTTP_OK) {
                if (total > max_body) result = CLAUSIUM_HTTP_TOO_LARGE;
                else if (!total || (length > 0 && total != static_cast<size_t>(length)) ||
                         !esp_http_client_is_complete_data_received(client))
                    result = CLAUSIUM_HTTP_BAD_RESPONSE;
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (result == CLAUSIUM_HTTP_OK) {
        body[total] = 0;
        *len = total;
    } else {
        body[0] = 0;
        ESP_LOGW(kTag, "GET %s failed: result=%d status=%d bytes=%u", path, (int)result, status,
                 (unsigned)total);
    }
    return result;
}
