#include "clausium_config.h"

#include "nvs.h"
#include <string.h>

static const char *const kNamespace = "clausium";

bool clausium_host_valid(const char *host) {
    if (!host) return false;
    const size_t len = strlen(host);
    if (!len || len > CLAUSIUM_HOST_MAX) return false;
    bool numeric = true;
    for (size_t i = 0; i < len; ++i) {
        const char c = host[i];
        const bool digit = c >= '0' && c <= '9';
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!digit && !alpha && c != '.' && c != '-') return false;
        if (!digit && c != '.') numeric = false;
    }
    if (numeric) {
        unsigned octets[4], count = 0, value = 0, digits = 0;
        for (size_t i = 0; i <= len; ++i) {
            if (i == len || host[i] == '.') {
                if (!digits || count >= 4) return false;
                octets[count++] = value;
                value = digits = 0;
                continue;
            }
            if (++digits > 3) return false;
            value = value * 10 + (unsigned)(host[i] - '0');
            if (value > 255) return false;
        }
        return count == 4 && octets[0] >= 1 && octets[0] <= 223 && octets[0] != 127;
    }
    // RFC 1123 labels: 1..63 characters, no leading or trailing hyphen.
    size_t label = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || host[i] == '.') {
            if (!label || host[i - 1] == '-') return false;
            label = 0;
            continue;
        }
        if (!label && host[i] == '-') return false;
        if (++label > 63) return false;
    }
    return true;
}

bool clausium_token_normalize(const char *in, char out[CLAUSIUM_TOKEN_LEN + 1]) {
    if (!in || strlen(in) != CLAUSIUM_TOKEN_LEN) return false;
    for (size_t i = 0; i < CLAUSIUM_TOKEN_LEN; ++i) {
        char c = in[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) return false;
        out[i] = c;
    }
    out[CLAUSIUM_TOKEN_LEN] = 0;
    return true;
}

static void read_raw(clausium_config_t *out, bool *token_ok) {
    memset(out, 0, sizeof(*out));
    out->port = CLAUSIUM_DEFAULT_PORT;
    *token_ok = false;
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return;
    size_t cap = sizeof(out->host);
    if (nvs_get_str(h, "host", out->host, &cap) != ESP_OK || !clausium_host_valid(out->host))
        out->host[0] = 0;
    uint16_t port = 0;
    if (nvs_get_u16(h, "port", &port) == ESP_OK && port) out->port = port;
    char token[CLAUSIUM_TOKEN_LEN + 1] = {0};
    cap = sizeof(token);
    *token_ok = nvs_get_str(h, "token", token, &cap) == ESP_OK &&
        clausium_token_normalize(token, out->token);
    if (!*token_ok) memset(out->token, 0, sizeof(out->token));
    memset(token, 0, sizeof(token));
    nvs_close(h);
}

bool clausium_config_load(clausium_config_t *out) {
    bool token_ok;
    read_raw(out, &token_ok);
    return out->host[0] && token_ok;
}

void clausium_config_status(char *host, size_t host_cap, uint16_t *port, bool *token_set) {
    clausium_config_t config;
    read_raw(&config, token_set);
    if (host_cap) {
        strncpy(host, config.host, host_cap - 1);
        host[host_cap - 1] = 0;
    }
    *port = config.port;
    memset(&config, 0, sizeof(config));
}

esp_err_t clausium_config_save(const char *host, uint16_t port, const char *token) {
    if (!host) return ESP_ERR_INVALID_ARG;
    const bool clear = !host[0];
    char normalized[CLAUSIUM_TOKEN_LEN + 1] = {0};
    const bool new_token = token && token[0];
    if (!clear) {
        if (!clausium_host_valid(host) || !port) return ESP_ERR_INVALID_ARG;
        if (new_token && !clausium_token_normalize(token, normalized)) return ESP_ERR_INVALID_ARG;
        if (!new_token) {
            clausium_config_t current;
            bool token_ok;
            read_raw(&current, &token_ok);
            memset(&current, 0, sizeof(current));
            if (!token_ok) return ESP_ERR_INVALID_ARG; // the first save must include a token
        }
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (clear) {
        err = nvs_erase_key(h, "host");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) {
            err = nvs_erase_key(h, "token");
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        }
    } else {
        err = nvs_set_str(h, "host", host);
        if (err == ESP_OK) err = nvs_set_u16(h, "port", port);
        if (err == ESP_OK && new_token) err = nvs_set_str(h, "token", normalized);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    memset(normalized, 0, sizeof(normalized));
    return err;
}

bool clausium_config_alerts_enabled(void) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return true;
    uint8_t value = 1;
    if (nvs_get_u8(h, "alerts", &value) != ESP_OK) value = 1;
    nvs_close(h);
    return value != 0;
}

esp_err_t clausium_config_set_alerts(bool enabled) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "alerts", enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
