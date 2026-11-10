// main/apps/voice/udp_audio.c -- Wi-Fi UDP audio/event channel implementation.
// Protocol contract: see docs/voice-udp-channel.md (isomorphic with companion/udp_transport.py).
#include "udp_audio.h"

#include "app_protocol.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "udp_audio";

// An audio block is fixed at 804B (4B block header + 800B of 4-bit data), see apps/voice/adpcm.h
#define ADPCM_BLOCK_MAX 804
// Uplink buffer: [type 1B][payload]; max payload = 2B frame header + 804B block
#define TX_BUF_MAX (1 + 2 + ADPCM_BLOCK_MAX)
// Downlink receive buffer: max payload = 513B (EVENT), with headroom to detect oversized packets
#define RX_BUF_MAX 640
// Threshold for deciding the peer "disappeared and came back". Note: the *1000 here is the ms->us conversion, the result is
// the link timeout itself (3000ms = 3e6us), not the "3x" the old comment claimed. Semantically it fits exactly:
// with a healthy beacon one is sent every second (interval ~1s < 3s, no trigger); once it stops for over 3s the link is down,
// and the next beacon re-sends device.hello so the companion learns us again.
#define PEER_STALE_US ((int64_t)VOICE_UDP_PEER_TIMEOUT_MS * 1000)
#define MY_IP_STR(buf) ((buf)[0] ? (buf) : "0.0.0.0")

static int s_sock = -1;
static volatile bool s_active = false;
static TaskHandle_t s_rx_task = NULL;
static SemaphoreHandle_t s_tx_mux = NULL;
static StaticSemaphore_t s_tx_mux_storage;

// Learned peer address (written only by the RX task when it receives a beacon; protected by s_peer_mux)
static struct sockaddr_in s_peer;
static volatile bool s_peer_known = false;
static volatile int64_t s_peer_last_us = 0;
static portMUX_TYPE s_peer_mux = portMUX_INITIALIZER_UNLOCKED;

// Uplink buffer (used inside tx_send while holding s_tx_mux)
static uint8_t s_tx_buf[TX_BUF_MAX];
// Audio assembly buffer: the only writer is the audio send worker, so no lock is needed
static uint8_t s_audio_pkt[2 + ADPCM_BLOCK_MAX];
static uint8_t s_audio_seq = 0;

static portMUX_TYPE s_stat_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_audio_drops = 0;
static uint32_t s_event_drops = 0;
static volatile uint32_t s_beacon_cnt = 0;   // link diagnostics: total beacons received
static volatile uint32_t s_ping_cnt = 0;     // link diagnostics: total pings sent

// ---- Internal helpers ----

static void drop_inc(uint32_t *counter)
{
    portENTER_CRITICAL(&s_stat_mux);
    (*counter)++;
    portEXIT_CRITICAL(&s_stat_mux);
}

static uint32_t drop_get(uint32_t *counter)
{
    portENTER_CRITICAL(&s_stat_mux);
    uint32_t v = *counter;
    portEXIT_CRITICAL(&s_stat_mux);
    return v;
}

// Take a snapshot of the peer address. Returns false = peer not learned yet (link not up).
static bool peer_snapshot(struct sockaddr_in *out)
{
    bool known;
    portENTER_CRITICAL(&s_peer_mux);
    known = s_peer_known;
    if (known) *out = s_peer;
    portEXIT_CRITICAL(&s_peer_mux);
    return known;
}

// Whether the link is fresh: the peer is known and a packet from it was received within the last VOICE_UDP_PEER_TIMEOUT_MS.
static bool peer_fresh(void)
{
    int64_t now = esp_timer_get_time();
    bool fresh = false;
    portENTER_CRITICAL(&s_peer_mux);
    fresh = s_peer_known && s_peer_last_us != 0 &&
            (now - s_peer_last_us) < (int64_t)VOICE_UDP_PEER_TIMEOUT_MS * 1000;
    portEXIT_CRITICAL(&s_peer_mux);
    return fresh;
}

// Unicast one packet. Returns -1 if the peer is unknown / the socket is not ready / the send failed.
// s_tx_buf is a shared buffer, serialized by s_tx_mux (the audio worker and the app task are two tasks).
static int tx_send(uint8_t type, const void *payload, size_t len)
{
    if (s_sock < 0 || len + 1 > TX_BUF_MAX || s_tx_mux == NULL) {
        return -1;
    }
    if (xSemaphoreTake(s_tx_mux, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "TX mutex wait timed out, dropping this frame");
        return -1;
    }
    int rc = -1;
    struct sockaddr_in peer;
    if (peer_snapshot(&peer)) {
        s_tx_buf[0] = type;
        if (len > 0) memcpy(s_tx_buf + 1, payload, len);
        ssize_t n = sendto(s_sock, s_tx_buf, len + 1, 0,
                           (const struct sockaddr *)&peer, sizeof(peer));
        if (n == (ssize_t)(len + 1)) {
            rc = 0;
        } else {
            ESP_LOGW(TAG, "sendto failed: rc=%d errno=%d", (int)n, errno);
        }
    }
    xSemaphoreGive(s_tx_mux);
    return rc;
}

static void send_device_hello(void)
{
    char buf[128];
    size_t n = app_protocol_device_hello(buf, sizeof(buf), 1);
    if (n > 0) {
        udp_audio_notify_event(buf, n);
    }
}

// Liveness: send the companion an empty-payload PING every VOICE_UDP_PING_MS.
// When the device is idle it produces no packets at all, and the companion cannot tell "the device is still here" from "the device left the voice app"
// -- this packet is the only basis for its 6s silence check. Not sent while the link is down (saves wasted work).
// Not counted in the drop statistics: a failed liveness packet is naturally covered by the beacon / the next packet, it is not a loss of audio/event semantics.
static void send_liveness_ping(void)
{
    if (!peer_fresh()) return;
    (void)tx_send(VOICE_UDP_T_PING, NULL, 0);
    s_ping_cnt++;
}

// beacon: establish or refresh the peer. Returns true = a device.hello must be sent back.
static bool peer_accept_beacon(const struct sockaddr_in *src)
{
    int64_t now = esp_timer_get_time();
    bool need_hello = false;
    portENTER_CRITICAL(&s_peer_mux);
    bool same = s_peer_known && s_peer.sin_addr.s_addr == src->sin_addr.s_addr;
    if (!same) {
        s_peer.sin_family = AF_INET;
        s_peer.sin_addr   = src->sin_addr;
        s_peer.sin_port   = htons(VOICE_UDP_PORT);
        s_peer_known      = true;
        need_hello        = true;
    } else if (s_peer_last_us == 0 || now - s_peer_last_us > PEER_STALE_US) {
        need_hello = true;   // the peer disappeared and came back: let the companion learn us again
    }
    s_peer_last_us = now;
    portEXIT_CRITICAL(&s_peer_mux);
    return need_hello;
}

// Non-beacon downlink: only extends the link's life when from the same source as the established peer (prevents a third party from keeping the link alive with spoofed packets)
static void peer_refresh(const struct sockaddr_in *src)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_peer_mux);
    if (s_peer_known && s_peer.sin_addr.s_addr == src->sin_addr.s_addr) {
        s_peer_last_us = now;
    }
    portEXIT_CRITICAL(&s_peer_mux);
}

static void rx_task(void *arg)
{
    (void)arg;
    static uint8_t rx[RX_BUF_MAX];
    static char json[APP_PROTO_RX_CAP + 1];
    int64_t last_ping_us = 0;
    ESP_LOGI(TAG, "UDP RX task started (port %d)", VOICE_UDP_PORT);

    while (s_active) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(s_sock, rx, sizeof(rx), 0,
                             (struct sockaddr *)&src, &slen);
        // n <= 0 means SO_RCVTIMEO (200ms) expired, the normal path -> fall straight through to the liveness tick below
        if (n > 0) {
            const uint8_t type = rx[0];
            if (type == VOICE_UDP_T_BEACON) {
                s_beacon_cnt++;
                if (peer_accept_beacon(&src)) {
                    ESP_LOGI(TAG, "companion beacon from %s -> link up",
                             inet_ntoa(src.sin_addr));
                    send_device_hello();
                }
            } else if (type == VOICE_UDP_T_EVENT) {
                struct sockaddr_in peer;
                if (!peer_snapshot(&peer) || peer.sin_addr.s_addr != src.sin_addr.s_addr ||
                    src.sin_port != peer.sin_port) continue;
                peer_refresh(&src);
                size_t plen = (size_t)n - 1;
                if (plen > APP_PROTO_RX_CAP) plen = APP_PROTO_RX_CAP;   // truncation guard
                memcpy(json, rx + 1, plen);
                json[plen] = '\0';
                app_event_t ev;
                if (app_protocol_parse(json, plen, &ev)) {
                    app_protocol_dispatch_event(&ev);   // approvals get important delivery, the rest zero-blocking
                } else {
                    ESP_LOGW(TAG, "downlink JSON rejected: %.*s",
                             (int)(plen > 80 ? 80 : plen), json);
                }
            } else {
                ESP_LOGW(TAG, "unknown packet type 0x%02x (%d B), ignored", type, (int)n);
            }
        }

        // Liveness tick: decoupled from receiving (runs whether or not a packet arrived this round)
        int64_t now_us = esp_timer_get_time();
        if (last_ping_us == 0 ||
            now_us - last_ping_us >= (int64_t)VOICE_UDP_PING_MS * 1000) {
            last_ping_us = now_us;
            send_liveness_ping();
        }
        // Link diagnostics (every 5s): the side whose beacon/ping counter does not grow is the break point
        static int64_t last_diag_us = 0;
        if (last_diag_us == 0 ||
            now_us - last_diag_us >= 5000000) {
            last_diag_us = now_us;
            bool known;
            int64_t age_ms;
            portENTER_CRITICAL(&s_peer_mux);
            known = s_peer_known;
            age_ms = s_peer_last_us ? (now_us - s_peer_last_us) / 1000 : -1;
            portEXIT_CRITICAL(&s_peer_mux);
            ESP_LOGI(TAG, "link diag: beacons=%u pings=%u peer_known=%d peer_age_ms=%ld",
                     (unsigned)s_beacon_cnt, (unsigned)s_ping_cnt,
                     (int)known, (long)age_ms);
        }
    }
    ESP_LOGI(TAG, "UDP RX task exiting");
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

// ---- Public interface ----

int udp_audio_init(void)
{
    if (s_sock >= 0) return 0;   // idempotent

    if (s_tx_mux == NULL) {
        s_tx_mux = xSemaphoreCreateMutexStatic(&s_tx_mux_storage);
        if (s_tx_mux == NULL) {
            ESP_LOGE(TAG, "TX mutex creation failed");
            return -1;
        }
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket creation failed: errno=%d", errno);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(VOICE_UDP_PORT);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind %d failed: errno=%d", VOICE_UDP_PORT, errno);
        close(sock);
        return -1;
    }
    // Receive timeout: lets the RX task periodically recheck the stop flag (so deinit can exit cleanly)
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 200 * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    // Allow sending to the broadcast address (the device does not broadcast now, kept for future active probing)
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    s_sock = sock;
    s_active = true;
    if (xTaskCreate(rx_task, "udp_rx", 4096, NULL, 4, &s_rx_task) != pdPASS) {
        ESP_LOGE(TAG, "RX task creation failed");
        s_active = false;
        close(sock);
        s_sock = -1;
        return -1;
    }
    ESP_LOGI(TAG, "UDP channel ready (port %d), waiting for companion beacon", VOICE_UDP_PORT);
    return 0;
}

int udp_audio_deinit(void)
{
    if (s_sock < 0 && s_rx_task == NULL) return 0;   // idempotent

    // 1. Set the stop flag first: the 200ms recvfrom timeout brings the task back to the top of the loop where it sees it
    s_active = false;
    // 2. Wait for the task to exit by itself before closing the socket -- close racing a blocked recvfrom is a race
    for (int i = 0; i < 50 && s_rx_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_rx_task != NULL) {
        ESP_LOGW(TAG, "RX task did not exit within 500ms (closing anyway, the task will settle on its own)");
    }
    // 3. Close the socket
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    // 4. Clear the peer state: rediscover on the next entry
    portENTER_CRITICAL(&s_peer_mux);
    s_peer_known  = false;
    s_peer_last_us = 0;
    memset(&s_peer, 0, sizeof(s_peer));
    portEXIT_CRITICAL(&s_peer_mux);
    s_audio_seq = 0;
    ESP_LOGI(TAG, "UDP channel closed");
    return 0;
}

int udp_audio_notify_audio(const uint8_t *frame, size_t len)
{
    if (frame == NULL || len == 0 || len > ADPCM_BLOCK_MAX) return -1;
    // Over UDP each block is always a single and last fragment -> frame header [seq][0x80], exactly what the companion's
    // reassemble_adpcm() expects: "new seq + is_last".
    s_audio_pkt[0] = s_audio_seq++;
    s_audio_pkt[1] = 0x80;
    memcpy(s_audio_pkt + 2, frame, len);
    int rc = tx_send(VOICE_UDP_T_AUDIO, s_audio_pkt, len + 2);
    if (rc != 0) drop_inc(&s_audio_drops);
    return rc;
}

int udp_audio_notify_event(const char *line, size_t len)
{
    if (line == NULL || len == 0) return -1;
    int rc = tx_send(VOICE_UDP_T_EVENT, line, len);
    if (rc != 0) drop_inc(&s_event_drops);
    return rc;
}

int udp_audio_notify_event_blocking(const char *line, size_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;   // UDP has no send queue or backpressure, no wait semantics
    return udp_audio_notify_event(line, len);
}

bool udp_audio_connected(void)        { return peer_fresh(); }
bool udp_audio_event_subscribed(void) { return peer_fresh(); }

uint32_t udp_audio_audio_drops(void) { return drop_get(&s_audio_drops); }
uint32_t udp_audio_event_drops(void) { return drop_get(&s_event_drops); }

const char *udp_audio_peer_ip(void)
{
    static char buf[16];
    struct sockaddr_in peer;
    if (!peer_snapshot(&peer)) {
        return "0.0.0.0";
    }
    const char *s = inet_ntoa(peer.sin_addr);
    strncpy(buf, s ? s : "0.0.0.0", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return MY_IP_STR(buf);
}
