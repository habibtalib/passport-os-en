#include "clausium_alert.h"

#include "clausium_client.h"
#include "launcher/app_registry.h"
#include "bsp_display.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "clausium_alert";

#define POLL_MS 5000
#define BACKOFF_MS 30000
#define HTTP_TIMEOUT_MS 2000
#define TASK_STACK 4096
#define MIN_FREE_HEAP 20480
#define MIN_LARGEST_BLOCK 8192
#define BANNER_US 8000000LL
// Below the Pomodoro notice (8,30 224x32) on lv_layer_top: both can show at once.
#define BANNER_X 8
#define BANNER_Y 66
#define BANNER_W 224
#define BANNER_H 32
#define NOTICE_EXPIRY_US 30000000LL

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
// Tracker and pending notice: shared by the poller, the app worker and sys_mon.
static bool s_have_base;
static char s_boot[CLAUSIUM_BOOT_LEN + 1];
static uint32_t s_seq;
static bool s_wake_pending, s_sound_pending;
static int64_t s_event_us;
static char s_text[CLAUSIUM_ALERT_TEXT_MAX];
// Banner: only touched by service_tick (sys_mon), under the LVGL lock.
static lv_obj_t *s_banner;
static int64_t s_banner_until_us;
// Poller lifecycle.
static atomic_bool s_task_running;
// Settings > Clausium Alerts, cached so the foreground path checks it without NVS.
static atomic_bool s_enabled;
// Task creation waits while a heavy app (Radio, Xiaozhi, Muse) owns the heap.
static atomic_bool s_start_deferred;
static atomic_uint s_generation;

static const char *kind_text(uint8_t kind) {
    switch (kind) {
        case CLAUSIUM_NEED_PERMISSION: return "needs permission";
        case CLAUSIUM_NEED_QUESTION: return "has a question";
        case CLAUSIUM_NEED_DONE: return "finished";
        default: return "error";
    }
}

bool clausium_alert_sound_allowed(passport_app_id_t id) {
    switch (id) {
        case APP_ID_RADIO: case APP_ID_VOICE: case APP_ID_XIAOZHI:
        case APP_ID_MUSE: case APP_ID_GAME: case APP_ID_SOUND_JUMP:
            return false;
        default:
            return true;
    }
}

bool clausium_alert_apply(const clausium_alert_msg_t *msg) {
    bool raised = false;
    uint32_t more = 0;
    portENTER_CRITICAL(&s_mux);
    if (!s_have_base || strcmp(s_boot, msg->boot) != 0) {
        // First poll, or the Mac app restarted: old events are not news.
        memcpy(s_boot, msg->boot, sizeof(s_boot));
        s_have_base = true;
    } else if (msg->seq < s_seq) {
        // Same boot, older reply (an overlapping poll answered late): ignore it.
        portEXIT_CRITICAL(&s_mux);
        return false;
    } else if (msg->seq > s_seq && msg->event_count) {
        more = msg->seq - s_seq - 1;
        raised = true;
    }
    s_seq = msg->seq; // the tracker advances even with alerts off
    portEXIT_CRITICAL(&s_mux);
    if (!raised || !atomic_load(&s_enabled)) return false;

    // Format outside the critical section; show the newest event.
    char text[CLAUSIUM_ALERT_TEXT_MAX];
    const clausium_event_t *e = &msg->event[msg->event_count - 1];
    const char *project = e->project[0] ? e->project : "Claude";
    if (more)
        snprintf(text, sizeof(text), "%s: %s +%u more", project, kind_text(e->kind), (unsigned)more);
    else
        snprintf(text, sizeof(text), "%s: %s", project, kind_text(e->kind));
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    // Re-check under the lock: reconfigure() clears s_enabled before it clears
    // the pending flags, so a poll in flight when alerts go off stays silent.
    raised = atomic_load(&s_enabled);
    if (raised) {
        memcpy(s_text, text, sizeof(s_text));
        s_event_us = now;
        s_wake_pending = s_sound_pending = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!raised) return false;
    ESP_LOGI(TAG, "alert seq=%u more=%u", (unsigned)msg->seq, (unsigned)more);
    return true;
}

void clausium_alert_text(char *out, size_t cap) {
    if (!cap) return;
    char copy[CLAUSIUM_ALERT_TEXT_MAX];
    portENTER_CRITICAL(&s_mux);
    memcpy(copy, s_text, sizeof(copy));
    portEXIT_CRITICAL(&s_mux);
    snprintf(out, cap, "%s", copy);
}

bool clausium_alert_poll(const clausium_config_t *config) {
    char path[48];
    portENTER_CRITICAL(&s_mux);
    const uint32_t since = s_have_base ? s_seq : 0;
    portEXIT_CRITICAL(&s_mux);
    snprintf(path, sizeof(path), "/passport/v1/alert?since=%u", (unsigned)since);
    char *body = malloc(CLAUSIUM_ALERT_MAX_BYTES + 2);
    if (!body) return false;
    size_t len = 0;
    bool ok = clausium_http_get(config, path, body, CLAUSIUM_ALERT_MAX_BYTES, &len,
                                HTTP_TIMEOUT_MS) == CLAUSIUM_HTTP_OK;
    if (ok) {
        clausium_alert_msg_t msg;
        ok = clausium_alert_parse(body, len, &msg) == CLAUSIUM_FEED_OK;
        if (ok) clausium_alert_apply(&msg);
        else ESP_LOGW(TAG, "alert reply rejected");
    }
    free(body);
    return ok;
}

static bool should_poll(void) {
    // The Clausium app polls the same tracker itself while it is in front;
    // heavy apps keep their heap and radio time.
    const passport_app_id_t active = app_registry_get_active();
    return active != APP_ID_CLAUSIUM && !app_registry_is_heavy(active) && clausium_net_connected() &&
        esp_get_free_heap_size() >= MIN_FREE_HEAP &&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= MIN_LARGEST_BLOCK;
}

static bool wanted(clausium_config_t *config) {
    return clausium_config_load(config) && clausium_config_alerts_enabled();
}

static void start_if_wanted(void);

static void alert_task(void *arg) {
    (void)arg;
    clausium_config_t config;
    unsigned generation = atomic_load(&s_generation);
    bool run = wanted(&config);
    portENTER_CRITICAL(&s_mux);
    s_have_base = false; // events from before this start are not news
    portEXIT_CRITICAL(&s_mux);
    bool logged_stack = false;
    while (run) {
        uint32_t delay_ms = POLL_MS;
        if (should_poll()) {
            if (!clausium_alert_poll(&config)) delay_ms = BACKOFF_MS;
            if (!logged_stack) {
                logged_stack = true;
                ESP_LOGI(TAG, "first poll; stack high-water=%u bytes",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
        }
        for (uint32_t waited = 0; waited < delay_ms && atomic_load(&s_generation) == generation;
             waited += 250)
            vTaskDelay(pdMS_TO_TICKS(250));
        if (atomic_load(&s_generation) != generation) {
            generation = atomic_load(&s_generation);
            run = wanted(&config);
        }
    }
    memset(&config, 0, sizeof(config));
    atomic_store(&s_task_running, false);
    ESP_LOGI(TAG, "poller stopped");
    // A reconfigure may have raced with this exit; honour it.
    if (atomic_load(&s_generation) != generation) start_if_wanted();
    vTaskDelete(NULL);
}

static void start_if_wanted(void) {
    clausium_config_t config;
    const bool want = wanted(&config);
    memset(&config, 0, sizeof(config));
    if (!want) {
        atomic_store(&s_start_deferred, false); // reconfigure() asks again
        return;
    }
    // Never carve the task stack out of a heavy app's heap (for example right
    // after a clean-launch reboot); service_tick starts it after the app exits.
    if (app_registry_is_heavy(app_registry_get_active())) {
        atomic_store(&s_start_deferred, true);
        return;
    }
    atomic_store(&s_start_deferred, false);
    if (atomic_exchange(&s_task_running, true)) return;
    if (xTaskCreate(alert_task, "clausium_alert", TASK_STACK, NULL, 2, NULL) != pdPASS) {
        atomic_store(&s_task_running, false);
        ESP_LOGW(TAG, "poller task not created; free=%u", (unsigned)esp_get_free_heap_size());
    } else {
        ESP_LOGI(TAG, "poller started; free=%u", (unsigned)esp_get_free_heap_size());
    }
}

void clausium_alert_init(bool defer) {
    atomic_store(&s_enabled, clausium_config_alerts_enabled());
    if (defer) atomic_store(&s_start_deferred, true);
    else start_if_wanted();
}

void clausium_alert_reconfigure(void) {
    const bool enabled = clausium_config_alerts_enabled();
    atomic_store(&s_enabled, enabled); // before clearing pending: see apply()
    atomic_fetch_add(&s_generation, 1);
    if (!enabled) {
        portENTER_CRITICAL(&s_mux);
        s_wake_pending = s_sound_pending = false;
        portEXIT_CRITICAL(&s_mux);
    }
    start_if_wanted();
}

void clausium_alert_app_started(void) {
    if (atomic_load(&s_task_running)) return;
    portENTER_CRITICAL(&s_mux);
    s_have_base = false;
    portEXIT_CRITICAL(&s_mux);
}

static void remove_banner(void) {
    if (s_banner) { lv_obj_delete(s_banner); s_banner = NULL; }
    s_banner_until_us = 0;
}

static void show_banner(const char *text, int64_t now) {
    remove_banner();
    s_banner = lv_label_create(lv_layer_top());
    lv_label_set_text(s_banner, text);
    lv_obj_set_style_text_font(s_banner, &buddy_font_16, 0);
    lv_obj_set_style_text_color(s_banner, lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_set_style_text_align(s_banner, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_banner, 6, 0);
    lv_obj_set_pos(s_banner, BANNER_X, BANNER_Y);
    lv_obj_set_size(s_banner, BANNER_W, BANNER_H);
    lv_label_set_long_mode(s_banner, LV_LABEL_LONG_DOT);
    s_banner_until_us = now + BANNER_US;
}

void clausium_alert_service_tick(void) {
    const passport_app_id_t front = app_registry_get_active();
    if (atomic_load(&s_start_deferred) && !app_registry_is_heavy(front)) start_if_wanted();
    // Cheap check first: no LVGL lock while there is nothing to show or remove.
    // s_banner is only written by this function, on this task.
    const bool enabled = atomic_load(&s_enabled);
    portENTER_CRITICAL(&s_mux);
    if (!enabled) s_wake_pending = s_sound_pending = false;
    const bool busy = s_wake_pending || s_sound_pending;
    portEXIT_CRITICAL(&s_mux);
    if (!busy && !s_banner) return;

    if (!bsp_lvgl_lock(30)) return;
    const int64_t now = esp_timer_get_time();
    if (s_banner && now >= s_banner_until_us) remove_banner();
    char text[CLAUSIUM_ALERT_TEXT_MAX];
    portENTER_CRITICAL(&s_mux);
    const bool wake = s_wake_pending, sound = s_sound_pending;
    const int64_t event_us = s_event_us;
    memcpy(text, s_text, sizeof(text));
    portEXIT_CRITICAL(&s_mux);
    if (!wake && !sound) { bsp_lvgl_unlock(); return; }

    const passport_app_id_t active = app_registry_get_active();
    const bool own_screen = active == APP_ID_CLAUSIUM;
    const bool expired = now - event_us > NOTICE_EXPIRY_US;
    bool wake_done = false, sound_done = false;
    if (wake) {
        // Same guards as the Pomodoro notice: never allocate a banner into a
        // fragmented or exhausted heap; retry on the next heartbeat.
        const bool room = own_screen || (esp_get_free_heap_size() >= 12288 &&
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= 4096);
        if (expired) {
            wake_done = true;
        } else if (room && app_registry_wake_for_notice()) {
            if (!own_screen) show_banner(text, now); // the app shows the list itself
            wake_done = true;
        }
    }
    if (sound) {
        if (expired || !clausium_alert_sound_allowed(active)) sound_done = true; // banner only
        else if (app_registry_sound_notice()) sound_done = true;
    }
    portENTER_CRITICAL(&s_mux);
    if (s_event_us == event_us) { // a newer event keeps its own pending flags
        if (wake_done) s_wake_pending = false;
        if (sound_done) s_sound_pending = false;
    }
    portEXIT_CRITICAL(&s_mux);
    bsp_lvgl_unlock();
}
