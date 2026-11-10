// main/app_events.h -- global application event queue (the only event channel).
// Key callbacks / link callbacks / audio workers only post here; app_task is the sole consumer.
#pragma once

#include "app_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_events_init(void);
void app_event_post(const app_event_t *ev);   // non-blocking post (dropped + logged when full)
// Important-event post (mode-switch disconnect, etc.): waits up to timeout_ms for a free slot (when full it waits for app_task to
// consume rather than dropping -- critical wrap-up events are not lost). Returns ESP_ERR_TIMEOUT on timeout, and the caller decides
// what next (log a warning and continue). Returns ESP_ERR_INVALID_STATE if the queue is not created.
esp_err_t app_event_post_important(const app_event_t *ev, uint32_t timeout_ms);
struct QueueDefinition *app_events_queue(void);  // QueueHandle_t, for app_task to consume

#ifdef __cplusplus
}
#endif
