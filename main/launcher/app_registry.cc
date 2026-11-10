#include "launcher/app_registry.h"
#include "launcher/timer_resume.h"
#include "bsp_memory.h"
#include "bsp_memory.h"
#include "lwip/sockets.h"
#include "launcher/launcher.h"
#include "launcher/wifi_setup.h"
#include "launcher/wake_gate.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "lvgl.h"
#include "ui_pixel.h"
#include "app_fonts.h"
#include "wifi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_pm.h"
#include "esp_heap_caps.h"
#include "boot_chime.h"
#include "ui_sfx.h"
#include <atomic>
#include <vector>

static const char *TAG = "app_registry";

// Forward declarations of external App instances
extern const passport_app_t g_radio_app;
extern const passport_app_t g_voice_app;
extern const passport_app_t g_xiaozhi_app;
extern const passport_app_t g_wifi_app;
extern const passport_app_t g_settings_app;
extern const passport_app_t g_home_app;
extern const passport_app_t g_game_app;
extern const passport_app_t g_tetris_app;
extern const passport_app_t g_stopwatch_app;
extern const passport_app_t g_sound_jump_app;
extern const passport_app_t g_muse_app;
extern const passport_app_t g_pomodoro_app;

namespace {

// Apps shown in the app section (Launcher carousel).
// Home (APP_ID_HOME) is **not here** - it is the base, not a card, and is addressed separately via
// app_registry_get_by_id(), so switch_to(APP_ID_HOME) will not fail to find it and black-screen.
// The AI Device (AI Device / Codex companion sprite) was removed on 2026-09-12, see docs/app-review-2026-09-12.md.
const passport_app_t *s_apps[] = {
    &g_radio_app,
    &g_voice_app,
    &g_xiaozhi_app,
    &g_muse_app,
    &g_game_app,
    &g_sound_jump_app,
    &g_tetris_app,
    &g_stopwatch_app,
    &g_pomodoro_app,
    &g_settings_app,   // Settings goes last (the usual closing slot for apps)
};

constexpr size_t kAppCount = sizeof(s_apps) / sizeof(s_apps[0]);
bool feedback_allowed(passport_app_id_t id) {
    return id == APP_ID_HOME || id == APP_ID_LAUNCHER || id == APP_ID_SETTINGS ||
           id == APP_ID_WIFI || id == APP_ID_STOPWATCH || id == APP_ID_POMODORO || id == APP_ID_TETRIS;
}
std::atomic<passport_app_id_t> s_active_id{APP_ID_LAUNCHER};
wake_gate_t s_wake_gate{};
bool s_initialized[APP_ID_COUNT] = {};

// 2026-09-13: switch_to() is called from three different tasks (input_task keys/long-press,
// the serial command task in main.cc, wifi_setup connect_worker), and internally it is a
// multi-step "stop old App -> start new App" sequence - running concurrently would cause double start,
// double task creation, and torn s_active_id. A mutex serializes the whole switch here.
// The lock uses a 3s timeout rather than waiting forever: better to drop one switch than to hang the caller permanently
// (see A2: on the C3 any unbounded wait eventually becomes "keys do nothing").
SemaphoreHandle_t s_switch_mutex = nullptr;

struct SwitchGuard {
    bool owned = false;
    bool try_take(TickType_t wait = pdMS_TO_TICKS(3000)) {
        if (!s_switch_mutex) return false;
        owned = xSemaphoreTakeRecursive(s_switch_mutex, wait) == pdTRUE;
        return owned;
    }
    ~SwitchGuard() {
        if (owned && s_switch_mutex) xSemaphoreGiveRecursive(s_switch_mutex);
    }
};

// Choose the Wi-Fi power-save level by foreground app: streaming/voice need immediate downlink and keep MIN modem sleep;
// standby foregrounds such as desktop/Home/Settings relax to MAX modem sleep (downlink latency can reach ~1s, which
// weather/SNTP/upload services do not notice), the single largest standby-current optimization.
WifiPowerSaveLevel power_save_level_for(passport_app_id_t id) {
    switch (id) {
        case APP_ID_RADIO:
        case APP_ID_VOICE:
        case APP_ID_XIAOZHI:
        case APP_ID_MUSE:
            return WifiPowerSaveLevel::BALANCED;
        default:
            return WifiPowerSaveLevel::LOW_POWER;
    }
}

// Radio/Xiaozhi/Muse TLS, decoding and audio buffers need large contiguous memory. The C3 has no PSRAM,
// and the small blocks left after these apps exit (lwIP, Wi-Fi, launcher UI) fragment the heap; on re-entry
// the total is enough but the largest contiguous block is not, showing up as connection failure or crash while running. Fragmentation cannot be defragmented in place:
// only when the current heap truly cannot fit the target app, or the previous heavy session already hit an allocation failure,
// do we record the target and soft-reboot, entering that app directly at boot.
constexpr uint32_t kCleanLaunchMagic = 0x434c4e31; // "CLN1"
RTC_NOINIT_ATTR uint32_t s_clean_launch[2];

struct HeapNeed {
    size_t largest; // single-block contiguous need plus margin
    size_t free;    // total session usage plus margin
};
// rc.51 measured (heap-trace diagnostic build): the radio MP3 decoder requests about 22.9 KiB at once, session takes about 34 KiB;
// Xiaozhi largest single block 12 KiB (task stack), session about 30 KiB; Muse worker task stack 12 KiB plus TLS 8 KiB.
HeapNeed heap_need(passport_app_id_t id) {
    switch (id) {
        case APP_ID_RADIO: return {24 * 1024, 48 * 1024};
        case APP_ID_XIAOZHI: return {16 * 1024, 44 * 1024};
        case APP_ID_MUSE: return {20 * 1024, 44 * 1024};
        default: return {0, 0};
    }
}

// On entering a heavy app, record the allocation-failure count; on leaving, if the count increased, the session
// already struggled while short of memory, so the next heavy-app entry goes straight to a clean heap.
unsigned s_failures_at_entry = 0;
bool s_heap_strained = false;
// Just rebooted for some app: the heap is already as clean as it gets; even if still below the bar we do not reboot again, avoiding a loop.
bool s_fresh_heap = false;

bool heavy_app(passport_app_id_t id) {
    return id == APP_ID_RADIO || id == APP_ID_XIAOZHI || id == APP_ID_MUSE;
}

// Call after the old app stop(): only then is the measured heap what the new app can use.
bool needs_clean_launch(passport_app_id_t id, size_t largest, size_t free) {
    if (!heavy_app(id)) return false;
    const HeapNeed need = heap_need(id);
    return s_heap_strained || largest < need.largest || free < need.free;
}

void show_clean_launch_notice(passport_app_id_t id);

[[noreturn]] void clean_launch(passport_app_id_t id) {
    s_clean_launch[0] = kCleanLaunchMagic;
    s_clean_launch[1] = static_cast<uint32_t>(id) ^ kCleanLaunchMagic;
    ESP_LOGW(TAG, "Restarting for a defragmented heap: target app=%d", (int)id);
    show_clean_launch_notice(id);
#ifdef ESP_PLATFORM
    timers_save_for_restart();
#endif
    esp_restart();
}

// Screen-off and backlight management
std::atomic<bool> s_screen_on{true};
std::atomic<passport_app_id_t> s_pending_target{APP_ID_COUNT};
esp_pm_lock_handle_t s_display_awake_lock = nullptr;
std::atomic<int64_t> s_last_activity_us{0};
std::atomic<uint16_t> s_screen_timeout_seconds{60};

// Long-press OK force-return flag
bool s_ok_long_triggered = false;

// Keep the last image in the panel while releasing the old LVGL tree. This
// empty screen exists only to keep LVGL's active pointer valid; never draw it.
lv_obj_t *s_handoff_screen = nullptr;
std::atomic<bool> s_building_target{false};
uint32_t s_handoff_count = 0;
uint32_t s_handoff_flushes = 0;
uint32_t s_panel_flushes = 0;

void resume_target_refresh(lv_display_t *display) {
    if (!s_screen_on.load() || s_building_target.load() ||
        (s_handoff_screen && lv_screen_active() == s_handoff_screen)) return;
    if (auto *timer = lv_display_get_refr_timer(display)) {
        // A full invalidation may already be cached while the holder is active.
        // Re-invalidating then emits no REFR_REQUEST, so explicitly resume.
        lv_timer_resume(timer);
        lv_timer_ready(timer);
    }
}

void display_refresh_guard(lv_event_t *event) {
    auto *display = static_cast<lv_display_t *>(lv_event_get_target(event));
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_REFR_REQUEST) {
        // LVGL's default REFR_REQUEST handler resumes this timer on every
        // invalidation. Pausing it once is insufficient; keep holding until a
        // real target is loaded, including targets built by another task.
        if (!s_screen_on.load() || s_building_target.load() ||
            (s_handoff_screen && lv_screen_active() == s_handoff_screen)) {
            if (auto *timer = lv_display_get_refr_timer(display)) lv_timer_pause(timer);
        }
    } else if (code == LV_EVENT_SCREEN_LOADED && !s_building_target.load() &&
               lv_screen_active() != s_handoff_screen) {
        // Async destinations can arrive after switch_to() has returned.
        resume_target_refresh(display);
    } else if (code == LV_EVENT_FLUSH_START) {
        ++s_panel_flushes;
        if (s_handoff_screen && lv_screen_active() == s_handoff_screen) {
            ++s_handoff_flushes;
            ESP_LOGE(TAG, "Unexpected empty handoff flush");
        }
    }
}

bool begin_screen_handoff() {
    if (!bsp_lvgl_lock(500)) return false;
    lv_obj_t *screen = lv_obj_create(nullptr);
    if (screen) {
        s_building_target = true;
        auto *old_handoff = s_handoff_screen;
        s_handoff_screen = screen;
        lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BG), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        if (auto *timer = lv_display_get_refr_timer(nullptr)) lv_timer_pause(timer);
        lv_screen_load(screen);
        if (old_handoff) lv_obj_delete(old_handoff);
        ++s_handoff_count;
    }
    bsp_lvgl_unlock();
    return screen != nullptr;
}

void finish_screen_handoff() {
    s_building_target = false;
    if (!bsp_lvgl_lock(500)) return;
    if (s_handoff_screen && lv_screen_active() != s_handoff_screen) {
        lv_obj_delete(s_handoff_screen);
        s_handoff_screen = nullptr;
        // Request a full target frame on the LVGL task, never render from the
        // input task or during the old application's slow network teardown.
        lv_obj_invalidate(lv_screen_active());
        resume_target_refresh(lv_display_get_default());
    } else {
        ESP_LOGI(TAG, "Keeping panel image until asynchronous target is loaded");
    }
    bsp_lvgl_unlock();
}

// Without this the panel freezes on the old app and then goes black, which
// reads as a crash. Draw one plain frame on the LVGL task and give the DMA
// flush time to land before the restart. The target is already recorded, so
// even a failure here still ends in the intended clean launch.
void show_clean_launch_notice(passport_app_id_t id) {
    if (!s_screen_on.load() || !bsp_lvgl_lock(500)) return;
    const passport_app_t *app = app_registry_get_by_id(id);
    s_building_target = false;
    lv_obj_t *screen = lv_obj_create(nullptr);
    if (screen) {
        lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BG), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        char text[64];
        snprintf(text, sizeof(text), "Freeing memory\nOpening %s soon", app ? app->name : "app");
        lv_obj_t *label = ui_pixel_label(screen, text, &buddy_font_16, UI_TEXT);
        lv_obj_set_width(label, 216);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(label, 6, 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        lv_screen_load(screen);
        if (s_handoff_screen) { lv_obj_delete(s_handoff_screen); s_handoff_screen = nullptr; }
        if (auto *timer = lv_display_get_refr_timer(nullptr)) { lv_timer_resume(timer); lv_timer_ready(timer); }
    }
    bsp_lvgl_unlock();
    if (screen) vTaskDelay(pdMS_TO_TICKS(400));
}

} // namespace

void app_registry_init(void) {
    ESP_LOGI(TAG, "Initializing app registry with %u apps...", (unsigned)kAppCount);
    if (!s_switch_mutex) s_switch_mutex = xSemaphoreCreateRecursiveMutex();
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "display_on", &s_display_awake_lock) == ESP_OK)
        esp_pm_lock_acquire(s_display_awake_lock);
    bsp_lvgl_lock(-1);
    lv_display_add_event_cb(lv_display_get_default(), display_refresh_guard, LV_EVENT_REFR_REQUEST, nullptr);
    lv_display_add_event_cb(lv_display_get_default(), display_refresh_guard, LV_EVENT_FLUSH_START, nullptr);
    lv_display_add_event_cb(lv_display_get_default(), display_refresh_guard, LV_EVENT_SCREEN_LOADED, nullptr);
    bsp_lvgl_unlock();
    s_last_activity_us.store(esp_timer_get_time(), std::memory_order_release);
    s_active_id = APP_ID_LAUNCHER;
    // Do not init/load the Launcher UI here: at boot main goes through the boot animation straight to Home,
    // loading early would make the app page flash for a frame or two at boot. launcher_ui_show() builds it itself when the drawer is entered.
}

size_t app_registry_get_count(void) {
    return kAppCount;
}

const passport_app_t *app_registry_get_by_index(size_t index) {
    if (index >= kAppCount) return nullptr;
    return s_apps[index];
}

const passport_app_t *app_registry_get_by_id(passport_app_id_t id) {
    // Home is not in the s_apps carousel list, but must still be directly addressable by switch_to().
    // Without this special case, switching back to Home would stall on the transition screen (black screen) because no app instance is found.
    if (id == APP_ID_HOME) return &g_home_app;
    if (id == APP_ID_WIFI) return &g_wifi_app; // hidden system setup, never a launcher card
    for (size_t i = 0; i < kAppCount; ++i) {
        if (s_apps[i] && s_apps[i]->id == id) {
            return s_apps[i];
        }
    }
    return nullptr;
}

passport_app_id_t app_registry_take_clean_launch(void) {
    const uint32_t magic = s_clean_launch[0];
    const auto id = static_cast<passport_app_id_t>(s_clean_launch[1] ^ kCleanLaunchMagic);
    s_clean_launch[0] = s_clean_launch[1] = 0; // one-shot: if the target app crashes, the next boot returns to Home
    if (magic != kCleanLaunchMagic || esp_reset_reason() != ESP_RST_SW || !heavy_app(id))
        return APP_ID_COUNT;
    s_fresh_heap = true;
    return id;
}

passport_app_id_t app_registry_get_active(void) {
    return s_active_id;
}

void app_registry_switch_to(passport_app_id_t id) {
    // Serialize: see the s_switch_mutex comment. If the lock cannot be obtained, give up this switch and log it,
    // no unbounded wait.
    SwitchGuard guard;
    if (!guard.try_take()) {
        ESP_LOGE(TAG, "switch_to(%d): registry busy; switch dropped", (int)id);
        return;
    }

    // Another caller may have completed this same switch while we waited.
    // A request for the current app can arrive while it is already stopping
    // for a different destination. Honour that latest request by restarting it
    // after shutdown, rather than silently navigating to the abandoned target.
    if (s_active_id == id && s_pending_target == APP_ID_COUNT) return;

    if (id != APP_ID_WIFI && !wifi_setup_can_leave()) return;

    // Reject unknown destinations before touching the current screen.
    // This guard was added after the AI Device module was removed on 2026-09-12, to keep a stale id from tripping us again.
    const passport_app_t *next = app_registry_get_by_id(id);
    if (id != APP_ID_LAUNCHER && next == nullptr) {
        ESP_LOGE(TAG, "switch_to(%d): no app registered for this id; switch ignored", (int)id);
        return;
    }

    const passport_app_t *current = app_registry_get_by_id(s_active_id);
    if (boot_chime_busy() || (!feedback_allowed(id) && !ui_sfx_prepare_stop()) ||
        (current && current->prepare_stop && !current->prepare_stop())) {
        if (s_pending_target.exchange(id) == APP_ID_COUNT)
            ESP_LOGI(TAG, "Deferring switch until current worker releases resources");
        return;
    }
    ESP_LOGI(TAG, "Switching app: %d -> %d", (int)s_active_id.load(), id);

    // Free old UI safely without replacing the physical panel image with a
    // loading/return page. The real destination supplies the next visible frame.
    const int64_t switch_started_us = esp_timer_get_time();
    if (!begin_screen_handoff()) {
        s_pending_target = id;
        ESP_LOGE(TAG, "Unable to prepare screen handoff; app switch cancelled safely");
        return;
    }
    s_pending_target = APP_ID_COUNT;

    // 1. Exit the previous App and clean up resources
    if (s_active_id != APP_ID_LAUNCHER) {
        const passport_app_t *prev = app_registry_get_by_id(s_active_id);
        if (prev && prev->stop) {
            ESP_LOGI(TAG, "Stopping app: %s", prev->name);
            prev->stop();
        }
    } else {
        launcher_ui_hide();
    }

    // Socket calls made by this long-lived caller lazily create a thread-local
    // semaphore. Once synchronous teardown has returned, no call is waiting on
    // it. Release it here so a tiny allocation cannot pin a session-sized hole.
    // Each background network task owns its own semaphore independently.
    lwip_socket_thread_cleanup();

    // 2. Print currently free memory to confirm the cleanup effect
    uint32_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Free heap after app stop: %u KB largest=%u minimum=%u",
             (unsigned)(free_heap / 1024),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)esp_get_minimum_free_heap_size());

    if (heavy_app(s_active_id) && bsp_memory_failures() != s_failures_at_entry) {
        s_heap_strained = true;
    }
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (!s_fresh_heap && needs_clean_launch(id, largest, free_heap)) clean_launch(id);
    s_fresh_heap = false;
    if (heavy_app(id)) s_failures_at_entry = bsp_memory_failures();

    bsp_memory_phase(next ? next->name : "launcher");
    s_active_id = id;
    // when the manager is not initialized only remember the level; it takes effect automatically after StartStation (see wifi_manager)
    WifiManager::GetInstance().SetPowerSaveLevel(power_save_level_for(id));

    // 3. Start the new target
    if (id == APP_ID_LAUNCHER) {
        launcher_ui_show();
    } else if (next) {
        ESP_LOGI(TAG, "Starting app: %s", next->name);
        if (next->init && !s_initialized[id]) {
            next->init();
            s_initialized[id] = true;
        }
        if (next->start) next->start();
    }
    finish_screen_handoff();
    lwip_socket_thread_cleanup();
    ESP_LOGI(TAG, "Target ready: app=%d elapsed_ms=%lld free=%u", id,
             (long long)((esp_timer_get_time() - switch_started_us) / 1000),
             (unsigned)esp_get_free_heap_size());
}

void app_registry_process_pending_switch(void) {
    SwitchGuard guard;
    if (!guard.try_take()) return;
    const auto target = s_pending_target.load();
    if (target != APP_ID_COUNT) app_registry_switch_to(target);
}

void app_registry_dispatch_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    // Hold the same lifecycle lock as stop/start while calling app->on_key.
    // Recursive because a key handler may itself request an app switch.
    SwitchGuard guard;
    if (!guard.try_take()) return;
    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "Dispatch Key: btn=%d, evt=%d, mv=%u, cur_active=%d", btn, event, mv, (int)s_active_id.load());

    if (wake_gate_consume(&s_wake_gate, event == BTN_EVT_PRESS)) return;

    // 1. Screen-off wake guard: if the screen is asleep, the first press only lights the screen and is not passed to the app logic
    if (!s_screen_on.load(std::memory_order_acquire)) {
        if (event == BTN_EVT_PRESS || event == BTN_EVT_CLICK) {
            // Wake the panel first (SLPOUT+DISPON, includes a 120ms settle wait), then turn on the backlight,
            // resume LVGL refresh and add one full-screen redraw (refreshes during sleep were dropped by the panel).
            wake_gate_arm(&s_wake_gate);
            if (!bsp_lvgl_lock(500)) return;
            if (s_display_awake_lock) esp_pm_lock_acquire(s_display_awake_lock);
            bsp_display_power(true);
            bsp_display_backlight(100);
            s_screen_on.store(true, std::memory_order_release);
            s_last_activity_us.store(now, std::memory_order_release);
            {
                resume_target_refresh(lv_display_get_default());
                lv_obj_t *scr = lv_screen_active();
                if (scr) lv_obj_invalidate(scr);
                bsp_lvgl_unlock();
            }
            ESP_LOGI(TAG, "Screen woken up by key press");
            return;
        }
    }

    s_last_activity_us.store(now, std::memory_order_release);

    const auto source = s_active_id.load();
    auto feedback = [&](ui_sfx_kind_t kind) {
        if (s_pending_target == APP_ID_COUNT && !boot_chime_busy() &&
            feedback_allowed(source) && feedback_allowed(s_active_id.load())) ui_sfx_play(kind);
    };

    // 2. Global intercept: long-press OK = the device-wide uniform "back", moving up the hierarchy:
    //    sub-app -> app section -> Home. Home is already the top level, long-press does nothing.
    //    (All future App interaction design follows this semantic and no longer defines long-press OK individually.)
    if (btn == BSP_BTN_OK && event == BTN_EVT_LONG) {
        if (s_active_id == APP_ID_WIFI) {
            s_ok_long_triggered = true;
            if (wifi_setup_can_leave()) {
                feedback(UI_SFX_BACK);
                app_registry_switch_to(APP_ID_HOME);
            }
            return;
        }
        if (s_active_id == APP_ID_LAUNCHER) {
            ESP_LOGI(TAG, "Back: Launcher -> Home");
            s_ok_long_triggered = true;
            feedback(UI_SFX_BACK);
            app_registry_switch_to(APP_ID_HOME);
            return;
        }
        if (s_active_id != APP_ID_HOME) {
            ESP_LOGI(TAG, "Back: App -> Launcher");
            s_ok_long_triggered = true;
            feedback(UI_SFX_BACK);
            app_registry_switch_to(APP_ID_LAUNCHER);
            return;
        }
        // Home is the top level: do not intercept or swallow; Home on_key itself does not consume long-press OK.
    }
    if (btn == BSP_BTN_OK && (event == BTN_EVT_RELEASE || event == BTN_EVT_LONG_UP)) {
        if (s_ok_long_triggered) {
            s_ok_long_triggered = false;
            return;
        }
    }

    // The old worker is already cancelling. Do not start another operation in
    // it while the input task waits for its cooperative shutdown.
    if (s_pending_target.load() != APP_ID_COUNT) return;

    const bool click = (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN || btn == BSP_BTN_OK) && event == BTN_EVT_CLICK;
    const auto kind = btn == BSP_BTN_OK ? UI_SFX_CONFIRM : UI_SFX_NAV;
    // Home can defer navigation while its weather worker closes. Acknowledge
    // the gesture now; settings must first apply mute and the feedback toggle.
    if (click && source != APP_ID_SETTINGS) feedback(kind);
    if (source == APP_ID_HOME && event == BTN_EVT_LONG && btn != BSP_BTN_OK)
        feedback(UI_SFX_CONFIRM);

    // 3. Event dispatch
    if (s_active_id == APP_ID_LAUNCHER) {
        launcher_ui_on_key(btn, event);
    } else {
        const passport_app_t *app = app_registry_get_by_id(s_active_id);
        if (app && app->on_key) {
            app->on_key(btn, event, mv);
        }
    }
    if (click && source == APP_ID_SETTINGS) feedback(kind);
}

void app_registry_tick(void) {
    if (bsp_lvgl_lock(50)) {
        if (!s_building_target.load() && s_handoff_screen && lv_screen_active() != s_handoff_screen) {
            lv_obj_delete(s_handoff_screen);
            s_handoff_screen = nullptr;
            resume_target_refresh(lv_display_get_default());
        }
        bsp_lvgl_unlock();
    }

    const uint16_t timeout = s_screen_timeout_seconds.load(std::memory_order_acquire);
    if (!timeout || !s_screen_on.load(std::memory_order_acquire)) return;

    const int64_t now = esp_timer_get_time();
    const int64_t last = s_last_activity_us.load(std::memory_order_acquire);
    const int64_t timeout_us = static_cast<int64_t>(timeout) * 1000000LL;
    if (now - last >= timeout_us) {
        // Do screen-off while holding the LVGL lock: serialized with refresh transactions, avoiding DISPOFF/SLPIN landing in the middle of a DMA transfer;
        // during sleep also pause LVGL refresh (pushing data to a sleeping panel wastes power and is pointless).
        if (bsp_lvgl_lock(200)) {
            const int64_t latest = s_last_activity_us.load(std::memory_order_acquire);
            if (esp_timer_get_time() - latest < timeout_us) {
                bsp_lvgl_unlock();
                return;
            }
            bsp_display_backlight(0);
            bsp_display_power(false); // DISPOFF+SLPIN real panel sleep; only turning off the backlight leaves image retention
            lv_timer_t *refr = lv_display_get_refr_timer(lv_display_get_default());
            if (refr) lv_timer_pause(refr);
            s_screen_on.store(false, std::memory_order_release);
            if (s_display_awake_lock) esp_pm_lock_release(s_display_awake_lock);
            bsp_lvgl_unlock();
            ESP_LOGI(TAG, "Screen blanked after %u seconds", (unsigned)timeout);
        }
    }
}

void app_registry_keep_awake(void) {
    // Renew only, do not wake: refreshing the timer when the screen is already off has no side effect (still off),
    // the next real key press still goes through the "first press only wakes" guard.
    s_last_activity_us.store(esp_timer_get_time(), std::memory_order_release);
}

bool app_registry_sound_notice(void) {
    SwitchGuard guard;
    // LVGL is already held: a blocking take would invert switch->LVGL order.
    if (!guard.try_take(0) || s_building_target.load() ||
        s_pending_target.load() != APP_ID_COUNT || boot_chime_busy() ||
        !feedback_allowed(s_active_id.load())) return false;
    if (esp_get_free_heap_size() < 12288 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 6144) return false;
    return ui_sfx_alarm_tick();
}

bool app_registry_wake_for_notice(void) {
    // Caller owns the LVGL lock. Never expose a transition holder or start an
    // app from the system monitor's small stack just to display a reminder.
    if (s_building_target.load() || s_pending_target.load() != APP_ID_COUNT ||
        s_handoff_screen || !lv_screen_active()) return false;
    if (!s_screen_on.load(std::memory_order_acquire)) {
        if (s_display_awake_lock) esp_pm_lock_acquire(s_display_awake_lock);
        bsp_display_power(true);
        bsp_display_backlight(100);
        s_screen_on.store(true, std::memory_order_release);
        resume_target_refresh(lv_display_get_default());
        lv_obj_invalidate(lv_screen_active());
        ESP_LOGI(TAG, "Screen woken for visual notice; foreground unchanged");
    }
    s_last_activity_us.store(esp_timer_get_time(), std::memory_order_release);
    return true;
}

void app_registry_set_screen_timeout(uint16_t seconds) {
    s_screen_timeout_seconds.store(seconds, std::memory_order_release);
    s_last_activity_us.store(esp_timer_get_time(), std::memory_order_release);
}

uint16_t app_registry_get_screen_timeout(void) {
    return s_screen_timeout_seconds.load(std::memory_order_acquire);
}

bool app_registry_screen_is_on(void) { return s_screen_on.load(std::memory_order_acquire); }

void app_registry_log_display_state(void) {
    if (!bsp_lvgl_lock(200)) return;
    ESP_LOGI(TAG, "Display: handoffs=%u placeholder_flushes=%u panel_flushes=%u waiting=%d",
        (unsigned)s_handoff_count, (unsigned)s_handoff_flushes, (unsigned)s_panel_flushes,
        s_handoff_screen && lv_screen_active() == s_handoff_screen);
    bsp_lvgl_unlock();
}
