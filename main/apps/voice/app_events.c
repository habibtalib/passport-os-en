// main/app_events.c -- application event queue implementation.
#include "app_events.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "app_evt";

#define APP_EVENT_QUEUE_DEPTH 8

// Static queue: 8x240B ~ 1.9KB static allocation, removing the startup heap peak and the creation-failure branch (M2).
// Depth 16->8 (performance round, saves 1856B): consuming takes ~us per item, backlog only occurs during app_task's long-action
// windows (voice.end drain <=500ms / cancel <=200ms), which are orthogonal in time to the high-burst sources (transcript/CTRL lines);
// drop-when-full + log stays as the safety net, and critical wrap-up events use blocking post_important delivery
// and are not lost; the worst case is a dropped display-only transcript line (overwritten line by line, self-healing, observable in logs).
// Note: after TIME_SET introduced int64 the struct is 240B (depth 8 = 1920B); the analysis above is unchanged (independent of length).
static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[APP_EVENT_QUEUE_DEPTH * sizeof(app_event_t)];
static QueueHandle_t s_queue;

esp_err_t app_events_init(void) {
    s_queue = xQueueCreateStatic(APP_EVENT_QUEUE_DEPTH, sizeof(app_event_t),
                                 s_queue_storage, &s_queue_struct);
    if (!s_queue) {   // static creation only fails on abnormal storage alignment, defensive check kept
        ESP_LOGE(TAG, "event queue creation failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void app_event_post(const app_event_t *ev) {
    if (!s_queue || !ev) return;
    if (xQueueSend(s_queue, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropping type=%d", (int)ev->type);
    }
}

esp_err_t app_event_post_important(const app_event_t *ev, uint32_t timeout_ms) {
    if (!s_queue || !ev) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_queue, ev, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "important event post timed out (%u ms), type=%d",
                 (unsigned)timeout_ms, (int)ev->type);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

QueueHandle_t app_events_queue(void) { return s_queue; }
