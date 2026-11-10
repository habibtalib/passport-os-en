#include "apps/voice/app_voice.h"
#include "apps/voice/app_events.h"
#include "apps/voice/app_state.h"
#include "apps/voice/app_sound.h"
#include "apps/voice/app_ui.h"
#include "apps/voice/app_protocol.h"
#include "apps/voice/audio_streamer.h"
#include "apps/voice/mode.h"
#include "apps/voice/udp_audio.h"
#include "apps/voice/time_sync.h"
#include "apps/voice/nvs_settings.h"
#include "bsp_battery.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "launcher/app_registry.h"
#include "wifi_manager.h"
#include <atomic>

static const char *TAG = "app_voice";

namespace {

std::atomic<bool> s_voice_active{false};
std::atomic<bool> s_voice_ui_ready{false};
// UI alive flag: stop() sets it false first and then deletes the UI, so a task that has not exited in time does not keep rendering.
std::atomic<bool> s_voice_ui_alive{false};
std::atomic<bool> s_voice_transport_done{false};
std::atomic<bool> s_voice_transport_failed{false};
TaskHandle_t s_voice_task_handle = nullptr;
app_state_t s_voice_state;
bool s_voice_ui_initialized = false;
uint32_t s_last_drop_count = 0;

// 2026-09-12: after the channel moved from BLE to Wi-Fi UDP, the suspend_wifi_driver() /
// resume_wifi_driver() that used to be here (enter voice: StopStation + esp_wifi_deinit to free memory for NimBLE,
// rebuild on exit) were removed wholesale -- that is exactly the cost this rework set out to eliminate: entering voice used to
// cut off weather/time/upload entirely and exiting required reconnecting. Now Wi-Fi stays up and voice just takes one more UDP port.
// See docs/voice-udp-channel.md.

void run_voice_actions(const app_action_t *acts, uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        const app_action_t *a = &acts[i];
        char buf[256];
        size_t len = 0;

        switch (a->type) {
        case APP_ACT_NONE:
        case APP_ACT_UI_REFRESH:
        case APP_ACT_UI_SCREEN_OFF:
        case APP_ACT_UI_SCREEN_ON:
        case APP_ACT_UI_PANEL_OFF:
        case APP_ACT_UI_PANEL_ON:
            break;
        case APP_ACT_SEND_VOICE_START:
            len = app_protocol_voice_start(buf, sizeof(buf), "ima_adpcm");
            udp_audio_notify_event_blocking(buf, len, 100);
            break;
        case APP_ACT_SEND_VOICE_END:
            audio_streamer_drain(500);
            s_last_drop_count = audio_streamer_take_drops();
            len = app_protocol_voice_end(buf, sizeof(buf));
            udp_audio_notify_event_blocking(buf, len, 100);
            len = app_protocol_device_status(buf, sizeof(buf), s_last_drop_count);
            udp_audio_notify_event(buf, len);
            break;
        case APP_ACT_SEND_KEY_ACTION:
            len = app_protocol_key_action(buf, sizeof(buf),
                    static_cast<app_key_action_t>(a->u.key_action.action));
            udp_audio_notify_event_blocking(buf, len, 100);
            break;
        case APP_ACT_STREAM_START:
            s_last_drop_count = 0;
            audio_streamer_set_sender(udp_audio_notify_audio);
            audio_streamer_set_compressed(true);
            if (bsp_audio_init() != ESP_OK || bsp_audio_set_format(16000, 16, 1) != ESP_OK ||
                audio_streamer_start() != ESP_OK) {
                app_event_t ev = {};
                ev.type = APP_EV_AUDIO_ERROR;
                app_event_post(&ev);
            }
            break;
        case APP_ACT_STREAM_STOP:
            audio_streamer_stop();
            audio_streamer_release_audio();
            break;
        case APP_ACT_STREAM_CANCEL:
            audio_streamer_cancel();
            audio_streamer_release_audio();
            break;
        case APP_ACT_PLAY_TONE:
            app_sound_play((app_tone_t)a->u.tone);
            break;
        case APP_ACT_TIME_SET:
            time_sync_set_epoch(a->u.time_set.epoch);
            break;
        default:
            break;
        }
    }
}

void voice_worker_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Voice event dispatch task started");
    app_action_t acts[8];
    uint8_t act_n = 0;
    uint64_t last_render_ms = 0;
    uint64_t last_battery_ms = 0;
    int last_battery_soc = -1;
    bool last_wifi_connected = false;
    int8_t last_wifi_rssi = -127;
    app_stage_t last_state = APP_ST_COUNT;
    // Deadline checks run every 100ms, including during PC telemetry traffic.
    uint64_t s_last_tick_ms = 0;

    // Build the minimal first page before the transport starts. Run at
    // idle priority during object creation so the task watchdog stays fed.
    vTaskPrioritySet(NULL, 0);
    if (bsp_lvgl_lock(2000)) {
        if (!s_voice_ui_initialized) {
            s_voice_ui_initialized = app_ui_init() == ESP_OK;
        }
        app_ui_show();
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock for UI init!");
    }
    s_voice_ui_ready.store(true);
    while (s_voice_active.load() && !s_voice_transport_done.load()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_voice_transport_failed.load()) {
        app_event_t failure{};
        failure.type = APP_EV_INIT_FAILED;
        app_state_reduce(&s_voice_state, &failure, esp_timer_get_time() / 1000, acts, &act_n);
    }
    vTaskPrioritySet(NULL, 4);

    while (s_voice_active.load()) {
        app_event_t ev;
        bool had_event = false;
        if (xQueueReceive((QueueHandle_t)app_events_queue(), &ev, pdMS_TO_TICKS(50)) == pdTRUE) {
            uint64_t now_ms = esp_timer_get_time() / 1000;
            app_state_reduce(&s_voice_state, &ev, now_ms, acts, &act_n);
            if (act_n > 0) {
                run_voice_actions(acts, act_n);
            }
            had_event = true;
        }
        {
            // Drive deadlines even when telemetry keeps the event queue busy.
            uint64_t tick_now = esp_timer_get_time() / 1000;
            if (s_last_tick_ms == 0 || tick_now - s_last_tick_ms >= 100) {
                s_last_tick_ms = tick_now;
                app_event_t tick{};
                tick.type = APP_EV_TICK;
                app_state_reduce(&s_voice_state, &tick, tick_now, acts, &act_n);
                run_voice_actions(acts, act_n);
            }
        }

        // Link reconciliation: after the channel moved to Wi-Fi UDP there is no "connect callback" (the BLE stack only posted
        // APP_EV_LINK_UP once characteristic subscription completed; after the migration the whole repo has zero producers),
        // so the state machine's link_up can only be reconciled here -- every round (~10ms) compare mode_link_up() with
        // link_up, and on a flip post the matching event, reusing the state machine's existing CONNECTED /
        // DISCONNECTED handling (banner show/hide, PTT gate, wrap up the session on link loss). The UDP layer's
        // peer_fresh has a built-in 3s hysteresis, so it does not flap.
        const bool link_now = mode_link_up();
        if (link_now != s_voice_state.link_up) {
            app_event_t link_ev{};
            link_ev.type = link_now ? APP_EV_LINK_UP
                                    : APP_EV_LINK_DOWN;
            app_event_post(&link_ev);
        }

        uint64_t now_ms = esp_timer_get_time() / 1000;
        bool state_changed = (s_voice_state.state != last_state);
        bool recording = (s_voice_state.state == APP_ST_LISTENING);
        if (recording) {
            app_registry_keep_awake(); /* keep suppressing screen-off while recording and voice dialogue are in progress, so long conversations are not interrupted */
        }
        bool time_to_tick = (now_ms - last_render_ms >= (recording ? 200 : 500));

        if (had_event || state_changed || time_to_tick) {
            last_render_ms = now_ms;
            last_state = s_voice_state.state;

            app_ui_snapshot_t snap;
            app_state_snapshot(&s_voice_state, now_ms, &snap);

            if (last_battery_ms == 0 || now_ms - last_battery_ms >= 10000) {
                last_battery_soc = bsp_battery_soc();
                auto &wifi = WifiManager::GetInstance();
                last_wifi_connected = wifi.IsConnected();
                last_wifi_rssi = last_wifi_connected ? wifi.GetRssi() : -127;
                last_battery_ms = now_ms;
            }
            snap.battery_available = last_battery_soc >= 0 && last_battery_soc <= 100;
            snap.wifi_connected = last_wifi_connected;
            snap.wifi_rssi = last_wifi_rssi;
            snap.battery_soc = snap.battery_available
                                   ? static_cast<uint8_t>(last_battery_soc)
                                   : 0;

            if (s_voice_ui_alive.load() && bsp_lvgl_lock(200)) {
                if (s_voice_ui_alive.load()) app_ui_render(&snap);
                bsp_lvgl_unlock();
            }
        }

        // Force a CPU yield so the IDLE task can run, preventing the Task Watchdog from firing
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Voice event dispatch task exiting");
    s_voice_task_handle = nullptr;
    vTaskDelete(NULL);
}

} // namespace

void voice_app_init(void) {
    ESP_LOGI(TAG, "Initializing Voice App components...");
    nvs_settings_init();
    time_sync_init();
    app_events_init();
    app_state_init(&s_voice_state);
}

// The caller publishes the result only after rollback is complete. The UI
// worker must never consume a peer beacon as readiness after local init fails.
static bool start_voice_transport(void) {
    if (udp_audio_init() != 0) {
        udp_audio_deinit();
        return false;
    }
    if (audio_streamer_init() != ESP_OK) {
        audio_streamer_deinit();
        udp_audio_deinit();
        return false;
    }
    return true;
}

void voice_app_start(void) {
    ESP_LOGI(TAG, "Starting Voice App...");
    xQueueReset((QueueHandle_t)app_events_queue());
    app_state_init(&s_voice_state);
    s_voice_active.store(true);
    s_voice_ui_alive.store(true);

    // Wi-Fi stays up: the voice channel is Wi-Fi UDP, so Wi-Fi is deliberately no longer stopped.
    // The original implementation here was suspend_wifi_driver() (freeing memory for NimBLE), removed together with BLE
    // -- see docs/voice-udp-channel.md.

    s_voice_ui_ready.store(false);
    s_voice_transport_done.store(false);
    s_voice_transport_failed.store(false);

    // 1. Build the UI on our own large stack first, then hand the runtime buffers to the transport layer.
    if (s_voice_task_handle == nullptr) {
        if (xTaskCreate(voice_worker_task, "voice_task", 8192, NULL, 4,
                        &s_voice_task_handle) != pdPASS) {
            s_voice_task_handle = nullptr;
            s_voice_active.store(false);
            ESP_LOGE(TAG, "Failed to create Voice task");
            return;
        }
    }
    uint32_t ui_wait_ms = 0;
    while (!s_voice_ui_ready.load() && ui_wait_ms < 10000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        ui_wait_ms += 10;
    }
    if (!s_voice_ui_ready.load()) {
        ESP_LOGE(TAG, "Voice UI initialization timed out");
        s_voice_active.store(false);
        int w = 0;   // C3: bounded, never wait forever on the stop path
        while (s_voice_task_handle != nullptr && w < 2000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            w += 10;
        }
        return;
    }

    const bool ready = start_voice_transport();
    if (!ready) ESP_LOGE(TAG, "Voice transport initialization failed; exit and retry");
    s_voice_transport_failed.store(!ready);
    s_voice_transport_done.store(true);
}

bool voice_app_prepare_stop(void) {
    s_voice_active.store(false);
    s_voice_ui_alive.store(false);
    if (s_voice_task_handle != nullptr) return false;
    // A slow worker still owns its codec and buffers. Keep that BSP ownership
    // until both workers finish, before another app starts audio.
    return audio_streamer_deinit();
}

void voice_app_stop(void) {
    ESP_LOGI(TAG, "Stopping Voice App and releasing UDP/Audio streamer...");
    s_voice_active.store(false);

    // 3. Wait for the main task to exit (2026-09-13 fix C3: add a 2s bound + withdraw the UI flag first,
    //    it used to be an unbounded while, running in input_task, and getting stuck meant all keys stopped responding)
    s_voice_ui_alive.store(false);
    int wait_ms = 0;
    while (s_voice_task_handle != nullptr && wait_ms < 2000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }
    if (s_voice_task_handle != nullptr) {
        ESP_LOGW(TAG, "voice task did not exit in 2s; continue (no vTaskDelete)");
    }

    // Worker no longer races stream start/end. Release the PC key before closing UDP.
    audio_streamer_cancel();
    audio_streamer_deinit();
    audio_streamer_release_audio();
    if (s_voice_state.state == APP_ST_LISTENING) {
        char buf[64];
        size_t len = app_protocol_voice_end(buf, sizeof(buf));
        udp_audio_notify_event_blocking(buf, len, 100);
    }
    udp_audio_deinit();

    // 4. Restore the launcher and free Voice LVGL objects for the next app.
    bsp_lvgl_lock(-1);
    app_ui_deinit();
    bsp_lvgl_unlock();
    s_voice_ui_initialized = false;

    ESP_LOGI(TAG, "Voice App cleanly stopped. Free heap: %u KB",
             (unsigned)(esp_get_free_heap_size() / 1024));
}

void voice_app_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    app_event_t ev = {};
    ev.u.key.btn = btn;
    ev.u.key.mv = mv;

    switch (event) {
    case BTN_EVT_PRESS:
        ev.type = APP_EV_KEY_PRESS;
        break;
    case BTN_EVT_RELEASE:
        ev.type = APP_EV_KEY_RELEASE;
        break;
    case BTN_EVT_CLICK:
        ev.type = APP_EV_KEY_CLICK;
        break;
    case BTN_EVT_LONG:
        ev.type = APP_EV_KEY_LONG;
        break;
    case BTN_EVT_LONG_UP:
        ev.type = APP_EV_KEY_LONG_UP;
        break;
    default:
        return;
    }

    // A dropped release must never leave capture running until the long timeout.
    app_event_post_important(&ev, 100);
}

const passport_app_t g_voice_app = {
    .id = APP_ID_VOICE,
    .name = "AI Voice",
    .en_name = "VOICE",
    .desc = "Hold Up to talk\nText goes to PC cursor",
    .tag = "Needs PC",
    .theme_color = 0x4ED39A,
    .init = voice_app_init,
    .start = voice_app_start,
    .stop = voice_app_stop,
    .on_key = voice_app_on_key,
    .prepare_stop = voice_app_prepare_stop,
};
