#include "apps/radio/app_radio.h"
#include "apps/radio/radio_ui.h"
#include "apps/radio/radio_player.h"
#include "apps/radio/radio_catalog.h"
#include "apps/home/home_profile.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>

static const char *TAG = "app_radio";

namespace {

std::atomic<bool> s_radio_active{false};
std::atomic<bool> s_wifi_connected{false};
std::atomic<bool> s_ui_alive{false};
std::atomic<bool> s_player_ready{false};
// Reserve before task creation so concurrent Wi-Fi/start paths own one worker.
std::atomic<bool> s_catalog_running{false};
std::atomic<bool> s_catalog_cancel{false};
// One successful city search per Radio session. Outdoor Wi-Fi drops and
// returns often; re-running the search on every reconnect stopped playback
// for several seconds each time.
std::atomic<bool> s_catalog_installed{false};
SemaphoreHandle_t s_catalog_commit = nullptr;

bool catalog_cancelled() { return s_catalog_cancel.load() || !s_radio_active.load(); }

void catalog_worker(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Starting shared city station catalog search...");
    constexpr std::size_t kDiscoveryCapacity = 8;
    // Release the live decoder/TLS working set before directory HTTP/cJSON.
    radio_player_set_playing(false);
    if (!radio_player_wait_idle(2000) || catalog_cancelled()) {
        // A timed-out decoder/TLS worker still owns its memory. Do not overlap
        // it with another HTTP connection, and leave normal playback retryable.
        xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
        if (!catalog_cancelled()) radio_player_set_playing(true);
        s_catalog_running.store(false);
        xSemaphoreGive(s_catalog_commit);
        ESP_LOGW(TAG, "Catalog deferred: player has not released stream resources");
        vTaskDelete(nullptr);
        return;
    }
    auto *stations = static_cast<RadioStation *>(
        std::calloc(kDiscoveryCapacity, sizeof(RadioStation)));
    if (!stations) {
        xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
        if (!catalog_cancelled()) radio_player_set_playing(true);
        s_catalog_running.store(false);
        xSemaphoreGive(s_catalog_commit);
        ESP_LOGE(TAG, "Unable to allocate station catalog");
        vTaskDelete(nullptr);
        return;
    }
    home_profile_t profile;
    home_profile_get(&profile);
    RadioLocation preferred = {}, loc = {};
    std::snprintf(preferred.city, sizeof(preferred.city), "%s", profile.city);
    std::snprintf(preferred.region, sizeof(preferred.region), "%s", profile.region);
    std::snprintf(preferred.search_city, sizeof(preferred.search_city), "%s", profile.radio_city);
    std::snprintf(preferred.country_code, sizeof(preferred.country_code), "%s", profile.country_code);
    std::size_t found = radio_catalog_discover_location(preferred, stations, kDiscoveryCapacity,
                                                       &loc, catalog_cancelled);
    // Serialize installation/start with stop; no late playback in the next app.
    xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    const bool alive = !catalog_cancelled() && s_ui_alive.load();
    if (alive && found > 0) {
        ESP_LOGI(TAG, "Location detected: %s (%s), found %u stations", loc.city, loc.region, (unsigned)found);
        radio_ui_set_location(loc.city);
        radio_player_replace_stations_resuming(stations, found);
        s_catalog_installed.store(true);
    }
    std::free(stations);
    if (alive) {
        // Start the MP3 decoder only after directory HTTP/cJSON is released.
        radio_player_set_playing(true);
    }
    xSemaphoreGive(s_catalog_commit);
    s_catalog_running.store(false);
    vTaskDelete(nullptr);
}

void start_catalog() {
#ifdef PASSPORT_RADIO_HLS_TEST
    // Keep the known test matrix visible; city discovery would replace it.
    radio_ui_set_location("Global radio test");
    radio_player_set_playing(true);
    return;
#endif
    if (!s_catalog_commit) return;
    xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    bool idle = false;
    // Reserve before task creation; a fast worker cannot leave a stale handle.
    if (!s_radio_active.load() || !s_player_ready.load() || s_catalog_installed.load() ||
        !s_catalog_running.compare_exchange_strong(idle, true)) {
        xSemaphoreGive(s_catalog_commit);
        return;
    }
    s_catalog_cancel.store(false);
    if (xTaskCreate(catalog_worker, "radio_cat", 6144, nullptr, 5, nullptr) != pdPASS) {
        s_catalog_running.store(false);
        ESP_LOGE(TAG, "Failed to create catalog task");
        radio_player_set_playing(true);
    }
    xSemaphoreGive(s_catalog_commit);
}

void on_wifi_event(WifiEvent event, const std::string &data) {
    if (!s_radio_active.load()) return;

    if (event == WifiEvent::Connected) {
        ESP_LOGI(TAG, "Wi-Fi connected: %s", data.c_str());
        s_wifi_connected.store(true);
        radio_ui_set_network(true, ("Connected: " + data).c_str());
        if (!s_player_ready.load()) return;
        radio_player_set_network(true);

        start_catalog();
    } else if (event == WifiEvent::Disconnected) {
        ESP_LOGI(TAG, "Wi-Fi disconnected: %s", data.c_str());
        s_wifi_connected.store(false);
        radio_ui_set_network(false, "Wi-Fi disconnected");
        if (s_player_ready.load()) radio_player_set_network(false);
    }
}

} // namespace

void radio_app_init(void) {
    ESP_LOGI(TAG, "Initializing Radio App subsystems...");
    radio_ui_init();
    static StaticSemaphore_t catalog_commit_storage;
    s_catalog_commit = xSemaphoreCreateMutexStatic(&catalog_commit_storage);
}

void radio_app_start(void) {
    ESP_LOGI(TAG, "Starting Radio App...");
    if (s_radio_active.load()) {
        ESP_LOGW(TAG, "Radio App is already active");
        return;
    }
    // 1. Show the Radio UI
    radio_ui_show_main();
    radio_ui_set_volume(radio_player_volume());
    if (!s_catalog_commit || s_catalog_running.load()) {
        s_ui_alive.store(true);
        radio_ui_set_playback(RadioPlaybackState::Error, "Catalog still exiting, retry later");
        return;
    }

    // A previous bounded stop may have retained a slow stream worker to avoid
    // freeing decoder/I2S resources underneath it.  Finish that cleanup before
    // creating the next session; never allow two player owners to coexist.
    if (s_player_ready.load()) {
        if (!radio_player_deinit(2000)) {
            s_radio_active.store(false);
            s_ui_alive.store(true);
            radio_ui_set_playback(RadioPlaybackState::Error,
                                  "Audio still exiting, retry later");
            ESP_LOGE(TAG, "Previous player worker is still alive; refusing a second owner");
            return;
        }
        s_player_ready.store(false);
    }

    if (!radio_player_init()) {
        s_player_ready.store(false);
        s_radio_active.store(false);
        s_ui_alive.store(true);
        radio_ui_set_playback(RadioPlaybackState::Error, "Low memory, audio failed to start");
        return;
    }
    s_player_ready.store(true);
    s_catalog_installed.store(false); // stations were reset to presets above
    s_radio_active.store(true);
    s_ui_alive.store(true);
    radio_player_set_playing(false);

    // 2. Hook the global Wi-Fi event callback
    WifiManager::GetInstance().SetEventCallback(on_wifi_event);
    if (!WifiManager::GetInstance().IsConnected()) {
        ESP_LOGI(TAG, "Wi-Fi not connected yet, checking station...");
        if (!SsidManager::GetInstance().GetSsidList().empty()) {
            radio_ui_set_network(false, "Connecting to network...");
            WifiManager::GetInstance().StartStation();
        } else {
            radio_ui_set_playback(RadioPlaybackState::Error, "Set up Wi-Fi in phone settings");
        }
    } else {
        radio_ui_set_network(true, "Wi-Fi connected");
        radio_player_set_network(true);
        start_catalog();
    }

    // 3. Start playback after the catalog query has released HTTP/cJSON memory.
}

bool radio_app_prepare_stop(void) {
    // Match catalog installation's lock: it must not resume playback after
    // this cancellation has stopped the player.
    if (s_catalog_commit) xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    s_catalog_cancel.store(true);
    if (s_radio_active.exchange(false) && s_player_ready.load())
        radio_player_set_playing(false);
    if (s_catalog_commit) xSemaphoreGive(s_catalog_commit);
    if (s_catalog_running.load()) return false;
    if (s_player_ready.load()) {
        if (!radio_player_deinit(0)) return false;
        s_player_ready.store(false);
    }
    return true;
}

void radio_app_stop(void) {
    ESP_LOGI(TAG, "Stopping Radio App (keeping global Wi-Fi active)...");
    if (s_catalog_commit) xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    s_catalog_cancel.store(true);
    s_radio_active.store(false);
    if (s_catalog_commit) xSemaphoreGive(s_catalog_commit);

    // 1. Stop playback and wait for the streaming thread to exit
    if (s_player_ready.load()) {
        radio_player_set_playing(false);
        radio_player_wait_idle(3500);
    }

    // 2. 2026-09-13 fix (C2): the catalog task **must be joined**. It used to just set a flag and return,
    //    leaving the task holding a 6144B stack + 24KB response buffer (a single HTTP call takes up to 5s), so entering
    //    Xiaozhi/voice right after very easily failed to allocate. Wait up to 5s here; on timeout still do not vTaskDelete
    //    (it may be holding the heap lock), just log and let it exit by itself.
    s_ui_alive.store(false);
    int waited_ms = 0;
    while (s_catalog_running.load() && waited_ms < 5000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited_ms += 50;
    }
    if (s_catalog_running.load()) {
        ESP_LOGW(TAG, "catalog task still running after 5s; left to exit by itself");
    }

    // 3. Remove the event callback, keep the global Wi-Fi connection
    WifiManager::GetInstance().SetEventCallback(nullptr);

    // 4. Radio is a session, not a boot-lifetime service. Release the 7KB
    // stream task, decoder registry, codec objects and I2S DMA before the next
    // app (especially Xiaozhi) tries to allocate a large contiguous block.
    if (s_player_ready.load()) {
        if (radio_player_deinit(5000)) s_player_ready.store(false);
    }

    // 5. Hide the Radio UI and pause the high-frequency timer
    radio_ui_hide();

    ESP_LOGI(TAG, "Radio App stopped. free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void radio_app_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;
    if (!s_player_ready.load()) return;

    if (event == BTN_EVT_CLICK) {
        if (btn == BSP_BTN_UP) {
            // "+" short press: next station
            radio_player_select_relative(1);
        } else if (btn == BSP_BTN_DOWN) {
            // "-" short press: previous station
            radio_player_select_relative(-1);
        } else if (btn == BSP_BTN_OK) {
            // "OK" short press: toggle play / pause
            radio_player_toggle();
        }
    } else if (event == BTN_EVT_LONG) {
        if (btn == BSP_BTN_UP) {
            // "+" long press: volume up
            uint8_t cur = radio_player_volume();
            uint8_t next = cur >= 90 ? 100 : cur + 10;
            radio_player_set_volume(next);
            ESP_LOGI(TAG, "Radio volume up: %u -> %u", cur, next);
        } else if (btn == BSP_BTN_DOWN) {
            // "-" long press: volume down
            uint8_t cur = radio_player_volume();
            uint8_t next = cur <= 10 ? 0 : cur - 10;
            radio_player_set_volume(next);
            ESP_LOGI(TAG, "Radio volume down: %u -> %u", cur, next);
        }
    }
}

const passport_app_t g_radio_app = {
    .id = APP_ID_RADIO,
    .name = "Radio",
    .en_name = "RADIO",
    .desc = "Listen to city net radio\nUp/Down: tune, OK: play",
    .tag = "Direct",
    .theme_color = 0xFFB74D,
    .init = radio_app_init,
    .start = radio_app_start,
    .stop = radio_app_stop,
    .on_key = radio_app_on_key,
    .prepare_stop = radio_app_prepare_stop,
};
