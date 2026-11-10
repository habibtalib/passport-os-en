// Clausium NVS settings: host/port/token validation, keep-token saves, alerts flag.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "apps/clausium/clausium_config.h"
#include "nvs.h"

typedef struct { char key[16]; char str[80]; uint32_t num; int type; } entry_t; // type 1 str, 2 u16, 3 u8
static entry_t store[8];
static bool opened, fail_open;
static entry_t *find(const char *key, bool create) {
    for (int i = 0; i < 8; ++i) if (store[i].type && !strcmp(store[i].key, key)) return &store[i];
    if (!create) return NULL;
    for (int i = 0; i < 8; ++i) if (!store[i].type) { snprintf(store[i].key, sizeof(store[i].key), "%s", key); return &store[i]; }
    assert(0); return NULL;
}
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    (void)mode; assert(!strcmp(ns, "clausium") && !opened);
    if (fail_open) return ESP_ERR_NVS_NOT_FOUND;
    opened = true; *h = 7; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { assert(h == 7 && opened); opened = false; }
esp_err_t nvs_commit(nvs_handle_t h) { assert(h == 7 && opened); return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *size) {
    assert(h == 7 && opened);
    entry_t *e = find(key, false); if (!e || e->type != 1) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < strlen(e->str) + 1) return ESP_ERR_INVALID_SIZE;
    strcpy(out, e->str); *size = strlen(e->str) + 1; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value) {
    assert(h == 7 && opened); entry_t *e = find(key, true); e->type = 1;
    snprintf(e->str, sizeof(e->str), "%s", value); return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    assert(h == 7 && opened); entry_t *e = find(key, false);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    memset(e, 0, sizeof(*e)); return ESP_OK;
}
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out) {
    assert(h == 7 && opened); entry_t *e = find(key, false);
    if (!e || e->type != 2) return ESP_ERR_NVS_NOT_FOUND;
    *out = (uint16_t)e->num; return ESP_OK;
}
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t v) {
    assert(h == 7 && opened); entry_t *e = find(key, true); e->type = 2; e->num = v; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out) {
    assert(h == 7 && opened); entry_t *e = find(key, false);
    if (!e || e->type != 3) return ESP_ERR_NVS_NOT_FOUND;
    *out = (uint8_t)e->num; return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v) {
    assert(h == 7 && opened); entry_t *e = find(key, true); e->type = 3; e->num = v; return ESP_OK;
}

int main(void) {
    // Hosts.
    const char *good[] = {"192.168.1.20", "10.0.0.1", "223.255.255.254", "my-mac.local", "mac", "a.b-c.d",
        "MacBook-Pro.local", "a23456789012345678901234567890123456789012345678901234567890123"};
    for (unsigned i = 0; i < sizeof(good) / sizeof(*good); ++i) assert(clausium_host_valid(good[i]));
    const char *bad[] = {"", "127.0.0.1", "0.1.2.3", "224.0.0.1", "192.168.1.256", "192.168.1", "1.2.3.4.5",
        "192.168..1", "-mac.local", "mac-.local", "mac..local", ".local", "mac.local.", "my_mac", "mac local",
        "http://mac", "1234567890123456789012345678901234567890123456789012345678901234", "1.2.3.0001"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        if (clausium_host_valid(bad[i])) { fprintf(stderr, "accepted %s\n", bad[i]); assert(0); }
    }
    assert(!clausium_host_valid(NULL));
    // Tokens.
    char token[CLAUSIUM_TOKEN_LEN + 1];
    assert(clausium_token_normalize("abcdefgh2345", token) && !strcmp(token, "ABCDEFGH2345"));
    assert(clausium_token_normalize("ZZZZZZ777777", token));
    assert(!clausium_token_normalize("ABCDEFGH234", token) && !clausium_token_normalize("ABCDEFGH23456", token));
    assert(!clausium_token_normalize("ABCDEFGH2341", token) && !clausium_token_normalize("ABCDEFGH234-", token));
    assert(!clausium_token_normalize(NULL, token));

    // Empty store: not configured, defaults, alerts on.
    clausium_config_t c;
    char host[CLAUSIUM_HOST_MAX + 1];
    uint16_t port;
    bool token_set;
    assert(!clausium_config_load(&c) && c.port == CLAUSIUM_DEFAULT_PORT && !c.host[0]);
    clausium_config_status(host, sizeof(host), &port, &token_set);
    assert(!host[0] && port == 8790 && !token_set);
    assert(clausium_config_alerts_enabled());
    fail_open = true; assert(clausium_config_alerts_enabled() && !clausium_config_load(&c)); fail_open = false;

    // First save needs a token; invalid inputs never write.
    assert(clausium_config_save("192.168.1.20", 8790, NULL) == ESP_ERR_INVALID_ARG);
    assert(clausium_config_save("192.168.1.20", 8790, "") == ESP_ERR_INVALID_ARG);
    assert(clausium_config_save("127.0.0.1", 8790, "ABCDEFGH2345") == ESP_ERR_INVALID_ARG);
    assert(clausium_config_save("192.168.1.20", 0, "ABCDEFGH2345") == ESP_ERR_INVALID_ARG);
    assert(clausium_config_save("192.168.1.20", 8790, "bad") == ESP_ERR_INVALID_ARG);
    assert(clausium_config_save(NULL, 8790, "ABCDEFGH2345") == ESP_ERR_INVALID_ARG);
    assert(!find("host", false) && !find("token", false));
    assert(clausium_config_save("192.168.1.20", 8790, "abcdefgh2345") == ESP_OK);
    assert(clausium_config_load(&c) && !strcmp(c.host, "192.168.1.20") && c.port == 8790 &&
           !strcmp(c.token, "ABCDEFGH2345"));
    assert(!strcmp(find("token", false)->str, "ABCDEFGH2345")); // stored upper case
    // Keep the token while changing host and port.
    assert(clausium_config_save("my-mac.local", 9000, NULL) == ESP_OK);
    assert(clausium_config_load(&c) && !strcmp(c.host, "my-mac.local") && c.port == 9000 &&
           !strcmp(c.token, "ABCDEFGH2345"));
    clausium_config_status(host, sizeof(host), &port, &token_set);
    assert(!strcmp(host, "my-mac.local") && port == 9000 && token_set);
    // Corrupt stored values read as unset.
    snprintf(find("token", false)->str, 80, "short");
    assert(!clausium_config_load(&c) && !c.token[0]);
    assert(clausium_config_save("my-mac.local", 9000, "ZZZZZZ777777") == ESP_OK && clausium_config_load(&c));
    // Clearing the host disables Clausium and forgets the token.
    assert(clausium_config_save("", 0, NULL) == ESP_OK);
    assert(!clausium_config_load(&c) && !find("host", false) && !find("token", false));
    clausium_config_status(host, sizeof(host), &port, &token_set);
    assert(!host[0] && !token_set);
    // Alerts flag.
    assert(clausium_config_set_alerts(false) == ESP_OK && !clausium_config_alerts_enabled());
    assert(clausium_config_set_alerts(true) == ESP_OK && clausium_config_alerts_enabled());
    assert(!opened);
    puts("Clausium config: host/IPv4 rules, base32 token normalisation, first-save token, keep-token save, "
         "corrupt values, clear to disable, alerts flag: PASS");
    return 0;
}
