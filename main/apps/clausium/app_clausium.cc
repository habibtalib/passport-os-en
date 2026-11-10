#include "apps/app_base.h"
#include "app_clausium.h"
#include "clausium_alert.h"
#include "clausium_client.h"
#include "clausium_config.h"
#include "clausium_feed.h"
#include "launcher/app_registry.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
constexpr char kTag[] = "clausium";
constexpr int kRows = 11, kRowY = 30, kRowPitch = 22, kRowW = 216;
// Usage page: one 60 px block per profile (name, 5h line + bar, week line + bar).
constexpr int kUsageY = 28, kUsagePitch = 60, kUsageBarH = 3;
constexpr uint32_t kPollMs = 3000, kBackoffMs = 15000;
constexpr int kHttpTimeoutMs = 1500;
constexpr int64_t kStaleUs = 30000000LL;
// Plain HTTP only (no TLS); measured high-water mark is logged on first poll.
constexpr uint32_t kWorkerStack = 5120;

enum Status : uint8_t { ST_CONNECTING, ST_OK, ST_NO_WIFI, ST_UNREACHABLE, ST_UNAUTHORIZED, ST_BAD_REPLY };
enum Mode : uint8_t {
    M_NOT_CONFIGURED, M_NO_MEMORY, M_CONNECTING, M_DATA, M_NO_WIFI, M_UNREACHABLE,
    M_UNAUTHORIZED, M_BAD_REPLY, M_STALE
};

// One heap block per visit, freed in stop(). The worker fills `slot` only
// while it is empty; the UI timer copies it to `shown` and empties it.
struct Context {
    clausium_config_t config;
    std::atomic<bool> slot_full{false};
    uint8_t slot_status;
    bool slot_has_state;
    clausium_state_t slot;
    clausium_state_t shown;
};

Context *s_ctx;
char *s_rx;
bool s_configured;
std::atomic<bool> s_active{false}, s_worker_running{false}, s_refresh{false};
TaskHandle_t s_worker;

// UI state: LVGL lock only.
lv_obj_t *s_screen, *s_status, *s_rows[kRows];
// Usage page objects per profile: name, window text, bar track and fill.
struct UsageBlock { lv_obj_t *name, *text[2], *track[2], *fill[2]; };
UsageBlock s_usage[CLAUSIUM_PLAN_MAX];
enum Page : uint8_t { PAGE_SESSIONS = 0, PAGE_USAGE, PAGE_COUNT };
lv_timer_t *s_timer;
ui_pixel_battery_t s_battery{};
uint64_t s_battery_updated_us;
uint8_t s_status_code = ST_CONNECTING;
bool s_have_data, s_dirty;
int64_t s_last_ok_us;
int s_page = PAGE_SESSIONS;
Mode s_mode = M_CONNECTING;
int64_t s_rendered_age_s = -1;

lv_obj_t *block_obj(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

// ---------------------------------------------------------------- worker

bool acquire_slot() {
    for (int i = 0; i < 20 && s_active.load(); ++i) {
        if (!s_ctx->slot_full.load(std::memory_order_acquire)) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return !s_ctx->slot_full.load(std::memory_order_acquire);
}

void publish(uint8_t status, bool has_state) {
    s_ctx->slot_status = status;
    s_ctx->slot_has_state = has_state;
    s_ctx->slot_full.store(true, std::memory_order_release);
}

// One poll; returns true when the feed was read and understood.
bool poll_once() {
    if (!clausium_net_connected()) {
        if (acquire_slot()) publish(ST_NO_WIFI, false);
        return false;
    }
    size_t len = 0;
    const clausium_http_result_t http = clausium_http_get(&s_ctx->config, "/passport/v1", s_rx,
        CLAUSIUM_FEED_MAX_BYTES, &len, kHttpTimeoutMs);
    // The HTTP client is released inside clausium_http_get before parsing.
    uint8_t status = http == CLAUSIUM_HTTP_UNAUTHORIZED ? ST_UNAUTHORIZED :
                     http == CLAUSIUM_HTTP_UNREACHABLE ? ST_UNREACHABLE :
                     http == CLAUSIUM_HTTP_OK ? ST_OK : ST_BAD_REPLY;
    if (!acquire_slot()) return status == ST_OK; // UI busy: drop this sample
    bool has_state = false;
    if (status == ST_OK) {
        const clausium_feed_result_t parsed = clausium_feed_parse(s_rx, len, &s_ctx->slot);
        has_state = parsed == CLAUSIUM_FEED_OK;
        if (!has_state) {
            status = ST_BAD_REPLY;
            ESP_LOGW(kTag, "feed rejected: %d (%u bytes)", (int)parsed, (unsigned)len);
        }
    }
    publish(status, has_state);
    // Chime and wake go through the shared alert tracker: one per event.
    // Skip the second request once the user is leaving the app.
    if (has_state && s_active.load()) clausium_alert_poll(&s_ctx->config);
    return has_state;
}

void worker(void *) {
    bool logged = false;
    while (s_active.load()) {
        const uint32_t wait_ms = poll_once() ? kPollMs : kBackoffMs;
        if (!logged) {
            logged = true;
            ESP_LOGI(kTag, "first poll; worker stack high-water=%u bytes free=%u",
                (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)esp_get_free_heap_size());
        }
        for (uint32_t waited = 0; waited < wait_ms && s_active.load(); waited += 100) {
            if (s_refresh.exchange(false)) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    s_worker = nullptr;
    s_worker_running.store(false); // last access: stop() may free the context now
    vTaskDelete(nullptr);
}

// -------------------------------------------------------------------- UI

void set_row(int i, const char *text, uint32_t color) {
    ui_pixel_label_set_text(s_rows[i], text);
    ui_pixel_text_color(s_rows[i], color);
}

const char *kind_word(uint8_t kind) {
    switch (kind) {
        case CLAUSIUM_NEED_PERMISSION: return "approve?";
        case CLAUSIUM_NEED_QUESTION: return "question";
        case CLAUSIUM_NEED_DONE: return "done";
        default: return "error";
    }
}

uint32_t live_color(uint8_t status) {
    switch (status) {
        case CLAUSIUM_LIVE_NEEDS: return UI_WARNING;
        case CLAUSIUM_LIVE_ERROR: return UI_DANGER;
        case CLAUSIUM_LIVE_WORKING: return UI_TEXT;
        default: return UI_TEXT_DIM;
    }
}

// The session list as virtual lines; returns false past the end.
bool session_line(const clausium_state_t &st, int index, char *buf, size_t cap, uint32_t *color,
                  bool with_ask) {
    int i = index;
    if (i-- == 0) {
        std::snprintf(buf, cap, "Needs you · %u", (unsigned)st.wait);
        *color = st.wait ? UI_WARNING : UI_TEXT_DIM;
        return true;
    }
    for (int n = 0; n < st.need_count; ++n) {
        const clausium_need_t &need = st.need[n];
        if (i-- == 0) {
            std::snprintf(buf, cap, "%s · %s", need.project[0] ? need.project : "Claude", kind_word(need.kind));
            *color = UI_WARNING;
            return true;
        }
        if (with_ask && i-- == 0) {
            if (need.ask[0]) std::snprintf(buf, cap, "  %s · %s", need.profile, need.ask);
            else std::snprintf(buf, cap, "  %s", need.profile);
            *color = UI_TEXT_DIM;
            return true;
        }
    }
    if (st.wait > st.need_count && i-- == 0) {
        std::snprintf(buf, cap, "+%u more waiting", (unsigned)(st.wait - st.need_count));
        *color = UI_TEXT_DIM;
        return true;
    }
    if (!st.wait && !st.need_count && i-- == 0) {
        std::snprintf(buf, cap, "Nobody is waiting");
        *color = UI_TEXT_DIM;
        return true;
    }
    if (i-- == 0) {
        std::snprintf(buf, cap, "Live · %u (%u working)", (unsigned)st.live_total, (unsigned)st.work);
        *color = UI_TEXT;
        return true;
    }
    for (int n = 0; n < st.live_count; ++n) {
        if (i-- == 0) {
            const clausium_live_t &live = st.live[n];
            std::snprintf(buf, cap, "%s · %s", live.project[0] ? live.project : "Claude", live.label);
            *color = live_color(live.status);
            return true;
        }
    }
    if (st.live_total > st.live_count && i-- == 0) {
        std::snprintf(buf, cap, "+%u more", (unsigned)(st.live_total - st.live_count));
        *color = UI_TEXT_DIM;
        return true;
    }
    if (!st.live_total && !st.live_count && i-- == 0) {
        std::snprintf(buf, cap, "No live sessions");
        *color = UI_TEXT_DIM;
        return true;
    }
    return false;
}

int session_line_count(const clausium_state_t &st, bool with_ask) {
    char buf[8];
    uint32_t color;
    int n = 0;
    while (session_line(st, n, buf, sizeof(buf), &color, with_ask)) ++n;
    return n;
}

void show_usage(bool visible) {
    for (auto &block : s_usage) {
        lv_obj_set_flag(block.name, LV_OBJ_FLAG_HIDDEN, !visible);
        for (int w = 0; w < 2; ++w) {
            lv_obj_set_flag(block.text[w], LV_OBJ_FLAG_HIDDEN, !visible);
            lv_obj_set_flag(block.track[w], LV_OBJ_FLAG_HIDDEN, !visible);
        }
    }
}

// "1h20m", "2d 4h", "59m"; "--" when unknown.
void format_left(char *buf, size_t cap, uint32_t reset, int64_t now_epoch) {
    if (!reset) { std::snprintf(buf, cap, "--"); return; }
    const int64_t left = static_cast<int64_t>(reset) - now_epoch;
    if (left <= 0) { std::snprintf(buf, cap, "now"); return; }
    const int64_t minutes = (left + 59) / 60;
    if (minutes < 60) std::snprintf(buf, cap, "%dm", static_cast<int>(minutes));
    else if (minutes < 24 * 60) std::snprintf(buf, cap, "%dh%02dm", static_cast<int>(minutes / 60), static_cast<int>(minutes % 60));
    else std::snprintf(buf, cap, "%dd %dh", static_cast<int>(minutes / 1440), static_cast<int>(minutes % 1440 / 60));
}

uint32_t usage_color(int used) {
    return used >= 90 ? UI_DANGER : used >= 70 ? UI_WARNING : UI_SUCCESS;
}

void set_window(UsageBlock &block, int w, const char *tag, int used, uint32_t reset, int64_t now_epoch) {
    char pct[16], left[32], line[80];
    if (used < 0) std::snprintf(pct, sizeof(pct), "--");
    else std::snprintf(pct, sizeof(pct), "%d%%", used);
    format_left(left, sizeof(left), reset, now_epoch);
    std::snprintf(line, sizeof(line), "%s %s · resets %s", tag, pct, left);
    ui_pixel_label_set_text(block.text[w], line);
    const int width = used <= 0 ? 0 : used * kRowW / 100;
    lv_obj_set_flag(block.fill[w], LV_OBJ_FLAG_HIDDEN, width == 0);
    if (width) lv_obj_set_width(block.fill[w], width);
    ui_pixel_bg_color(block.fill[w], usage_color(used));
}

// Countdowns use the Mac's clock (`t`) plus local time since the feed
// arrived, so the device clock is never needed.
void render_usage(const clausium_state_t &st, int64_t now_us) {
    const int64_t now_epoch = static_cast<int64_t>(st.built_at) + (now_us - s_last_ok_us) / 1000000LL;
    for (int r = 0; r < kRows; ++r) set_row(r, "", UI_TEXT);
    if (!st.plan_count) set_row(0, "No usage data yet", UI_TEXT_DIM);
    for (int p = 0; p < CLAUSIUM_PLAN_MAX; ++p) {
        UsageBlock &block = s_usage[p];
        const bool used = p < st.plan_count;
        lv_obj_set_flag(block.name, LV_OBJ_FLAG_HIDDEN, !used);
        for (int w = 0; w < 2; ++w) {
            lv_obj_set_flag(block.text[w], LV_OBJ_FLAG_HIDDEN, !used);
            lv_obj_set_flag(block.track[w], LV_OBJ_FLAG_HIDDEN, !used);
        }
        if (!used) continue;
        const clausium_plan_t &plan = st.plan[p];
        ui_pixel_label_set_text(block.name, plan.profile[0] ? plan.profile : "Claude");
        set_window(block, 0, "5h", plan.h5_used, plan.h5_reset, now_epoch);
        set_window(block, 1, "Wk", plan.week_used, plan.week_reset, now_epoch);
    }
}

void render_sessions(const clausium_state_t &st) {
    // Drop the ask lines before dropping sessions; the last row says what is cut.
    const bool with_ask = session_line_count(st, true) <= kRows;
    const int lines = session_line_count(st, with_ask);
    char buf[80];
    for (int r = 0; r < kRows; ++r) {
        uint32_t color = UI_TEXT;
        if (lines > kRows && r == kRows - 1) {
            std::snprintf(buf, sizeof(buf), "+%d more lines", lines - (kRows - 1));
            set_row(r, buf, UI_TEXT_DIM);
        } else if (session_line(st, r, buf, sizeof(buf), &color, with_ask)) {
            set_row(r, buf, color);
        } else {
            set_row(r, "", UI_TEXT);
        }
    }
}

void render_message(Mode mode, int64_t age_s) {
    char addr[CLAUSIUM_HOST_MAX + 8] = "";
    if (s_ctx) std::snprintf(addr, sizeof(addr), "%s:%u", s_ctx->config.host, (unsigned)s_ctx->config.port);
    const char *lines[kRows] = {};
    uint32_t first = UI_WARNING;
    switch (mode) {
        case M_NOT_CONFIGURED:
            lines[0] = "Set up Clausium on phone"; lines[2] = "Home: hold Down to open";
            lines[3] = "phone setup. In the"; lines[4] = "Clausium section, enter";
            lines[5] = "the Mac address and the"; lines[6] = "token from Clausium";
            lines[7] = "Settings > Devices.";
            break;
        case M_NO_MEMORY:
            lines[0] = "Not enough memory"; lines[2] = "Close the app and"; lines[3] = "try again.";
            break;
        case M_CONNECTING:
            lines[0] = "Connecting..."; lines[1] = addr; first = UI_TEXT;
            break;
        case M_NO_WIFI:
            lines[0] = "No Wi-Fi"; lines[2] = "Waiting for Wi-Fi."; lines[3] = "Retrying every 15 s.";
            break;
        case M_UNREACHABLE:
            lines[0] = "Mac unreachable"; lines[1] = addr; lines[3] = "Is Clausium running";
            lines[4] = "with Passport on?"; lines[5] = "Same Wi-Fi as the Mac?";
            lines[6] = "Retrying every 15 s.";
            break;
        case M_UNAUTHORIZED:
            lines[0] = "Wrong token"; lines[2] = "Copy the token from"; lines[3] = "Clausium Settings >";
            lines[4] = "Devices, then enter it"; lines[5] = "in phone setup.";
            break;
        case M_BAD_REPLY:
            lines[0] = "Unexpected reply"; lines[2] = "Update Clausium on the";
            lines[3] = "Mac (feed v1)."; lines[4] = "Retrying every 15 s.";
            break;
        default:
            lines[0] = "Data is stale"; lines[2] = "No fresh data from the"; lines[3] = "Mac. Retrying.";
            break;
    }
    char age[48] = "";
    if (s_have_data && age_s >= 0 && mode != M_UNAUTHORIZED && mode != M_NOT_CONFIGURED) {
        std::snprintf(age, sizeof(age), "Last update %llds ago", (long long)age_s);
        lines[kRows - 1] = age;
    }
    for (int i = 0; i < kRows; ++i)
        set_row(i, lines[i] ? lines[i] : "", i == 0 ? first : UI_TEXT_DIM);
}

Mode current_mode(int64_t now, int64_t *age_s) {
    *age_s = s_have_data ? (now - s_last_ok_us) / 1000000LL : -1;
    if (!s_configured) return M_NOT_CONFIGURED;
    if (!s_ctx || !s_rx) return M_NO_MEMORY;
    if (s_status_code == ST_UNAUTHORIZED) return M_UNAUTHORIZED;
    if (s_have_data && now - s_last_ok_us <= kStaleUs) return M_DATA;
    switch (s_status_code) {
        case ST_NO_WIFI: return M_NO_WIFI;
        case ST_UNREACHABLE: return M_UNREACHABLE;
        case ST_BAD_REPLY: return M_BAD_REPLY;
        case ST_CONNECTING: return s_have_data ? M_STALE : M_CONNECTING;
        default: return M_STALE;
    }
}

void render() {
    if (!s_screen) return;
    const int64_t now = esp_timer_get_time();
    int64_t age_s;
    const Mode mode = current_mode(now, &age_s);
    // Redraw only when something visible changed: stale pages tick each
    // second, Usage countdowns each minute.
    const int64_t clock = mode != M_DATA ? age_s : s_page == PAGE_USAGE ? age_s / 60 : 0;
    if (!s_dirty && mode == s_mode && clock == s_rendered_age_s) return;
    s_dirty = false;
    s_mode = mode;
    s_rendered_age_s = clock;
    char top[24] = "";
    if (mode == M_DATA) {
        const clausium_state_t &st = s_ctx->shown;
        if (s_page == PAGE_USAGE) {
            render_usage(st, now);
        } else {
            show_usage(false);
            render_sessions(st);
        }
        if (s_status_code != ST_OK) std::snprintf(top, sizeof(top), "Retrying");
        else std::snprintf(top, sizeof(top), s_page == PAGE_USAGE ? "Usage 2/2" : "Sessions 1/2");
        ui_pixel_text_color(s_status, s_status_code != ST_OK ? UI_WARNING : UI_TEXT_DIM);
    } else {
        show_usage(false);
        render_message(mode, age_s);
        if (mode == M_STALE) std::snprintf(top, sizeof(top), "Stale");
        ui_pixel_text_color(s_status, UI_WARNING);
    }
    ui_pixel_label_set_text(s_status, top);
}

void consume_slot() {
    if (!s_ctx || !s_ctx->slot_full.load(std::memory_order_acquire)) return;
    s_status_code = s_ctx->slot_status;
    if (s_ctx->slot_has_state) {
        s_ctx->shown = s_ctx->slot;
        s_have_data = true;
        s_last_ok_us = esp_timer_get_time();
    } else if (s_status_code == ST_UNAUTHORIZED) {
        s_have_data = false; // never keep showing data the Mac no longer authorises
    }
    s_ctx->slot_full.store(false, std::memory_order_release);
    s_dirty = true;
}

void tick(lv_timer_t *) {
    consume_slot();
    if (!app_registry_screen_is_on()) return;
    render();
    const uint64_t now = esp_timer_get_time();
    if (!s_battery_updated_us || now - s_battery_updated_us >= 5000000ULL) {
        ui_pixel_battery_set(&s_battery, bsp_battery_soc());
        s_battery_updated_us = now;
    }
}

void release_memory() {
    std::free(s_rx);
    s_rx = nullptr;
    delete s_ctx;
    s_ctx = nullptr;
}

void start() {
    // NVS read and allocation before taking the LVGL lock.
    clausium_config_t config;
    s_configured = clausium_config_load(&config);
    if (!bsp_lvgl_lock(1000)) return;
    if (s_screen) { bsp_lvgl_unlock(); return; }
    const uint32_t before = esp_get_free_heap_size();
    if (s_configured) {
        s_ctx = new (std::nothrow) Context();
        s_rx = static_cast<char *>(std::malloc(CLAUSIUM_FEED_MAX_BYTES + 2));
        if (s_ctx && s_rx) s_ctx->config = config;
        else release_memory();
    }
    std::memset(&config, 0, sizeof(config));
    s_status_code = ST_CONNECTING;
    s_have_data = false;
    s_last_ok_us = 0;
    s_page = PAGE_SESSIONS;
    s_dirty = true;
    s_screen = lv_obj_create(nullptr);
    ui_pixel_background(s_screen);
    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    lv_obj_t *title = ui_pixel_label(top, "Clausium", &buddy_font_16, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);
    s_status = ui_pixel_label(top, "", &buddy_font_16, UI_TEXT_DIM);
    lv_obj_set_pos(s_status, 96, 4);
    lv_obj_set_size(s_status, 104, 18);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_RIGHT, 0);
    s_battery = ui_pixel_battery_create(top, 206, 6, UI_TEXT, &lv_font_montserrat_14);
    for (int i = 0; i < kRows; ++i) {
        s_rows[i] = ui_pixel_label(s_screen, "", &buddy_font_16, UI_TEXT);
        lv_obj_set_pos(s_rows[i], 12, kRowY + i * kRowPitch);
        lv_obj_set_size(s_rows[i], kRowW, 20);
        lv_label_set_long_mode(s_rows[i], LV_LABEL_LONG_DOT);
    }
    for (int p = 0; p < CLAUSIUM_PLAN_MAX; ++p) {
        UsageBlock &block = s_usage[p];
        const int y = kUsageY + p * kUsagePitch;
        block.name = ui_pixel_label(s_screen, "", &buddy_font_16, UI_TEXT);
        lv_obj_set_pos(block.name, 12, y);
        lv_obj_set_size(block.name, kRowW, 18);
        lv_label_set_long_mode(block.name, LV_LABEL_LONG_DOT);
        for (int w = 0; w < 2; ++w) {
            const int line_y = y + 16 + w * 21;
            block.text[w] = ui_pixel_label(s_screen, "", &buddy_font_16, UI_TEXT_DIM);
            lv_obj_set_pos(block.text[w], 12, line_y);
            lv_obj_set_size(block.text[w], kRowW, 18);
            lv_label_set_long_mode(block.text[w], LV_LABEL_LONG_DOT);
            block.track[w] = block_obj(s_screen, 12, line_y + 18, kRowW, kUsageBarH, UI_SURFACE_ALT);
            block.fill[w] = block_obj(block.track[w], 0, 0, 1, kUsageBarH, UI_SUCCESS);
        }
    }
    show_usage(false);
    ui_pixel_footer(s_screen, "Up/Dn: page | OK: refresh", "Hold OK: back", &buddy_font_16);
    s_battery_updated_us = 0;
    s_rendered_age_s = -1;
    render();
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());
    lv_screen_load(s_screen);
    s_timer = lv_timer_create(tick, 500, nullptr);
    if (s_ctx) {
        clausium_alert_app_started();
        s_active.store(true);
        s_refresh.store(false);
        s_worker_running.store(true);
        if (xTaskCreate(worker, "clausium", kWorkerStack, nullptr, 4, &s_worker) != pdPASS) {
            s_worker_running.store(false);
            s_active.store(false);
            s_worker = nullptr;
            release_memory();
            s_dirty = true;
            render();
            ESP_LOGE(kTag, "worker not created; free=%u", (unsigned)esp_get_free_heap_size());
        }
    }
    ESP_LOGI(kTag, "started; configured=%d ui_heap_delta=%ld free=%u", s_configured,
        (long)before - (long)esp_get_free_heap_size(), (unsigned)esp_get_free_heap_size());
    bsp_lvgl_unlock();
}

bool prepare_stop() {
    s_active.store(false);
    return !s_worker_running.load();
}

void stop() {
    s_active.store(false);
    if (!bsp_lvgl_lock(-1)) return;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (s_screen) { lv_obj_delete(s_screen); s_screen = nullptr; }
    s_status = nullptr;
    for (auto &row : s_rows) row = nullptr;
    for (auto &block : s_usage) block = {};
    s_battery = {};
    // The registry calls stop() only after prepare_stop() reported the worker
    // gone. Never free memory a still-running worker may touch.
    if (!s_worker_running.load()) release_memory();
    else ESP_LOGE(kTag, "worker still running; context kept");
    s_have_data = false;
    ESP_LOGI(kTag, "UI released; free=%u", (unsigned)esp_get_free_heap_size());
    bsp_lvgl_unlock();
}

void on_key(uint8_t button, app_btn_event_t event, uint16_t) {
    if (event != BTN_EVT_CLICK || !bsp_lvgl_lock(500)) return;
    if (s_screen) {
        if (button == BSP_BTN_OK) {
            if (s_ctx) s_refresh.store(true);
        } else if (button == BSP_BTN_UP || button == BSP_BTN_DOWN) {
            // Two pages, wrapping both ways: Sessions <-> Usage.
            if (s_mode == M_DATA)
                s_page = (s_page + (button == BSP_BTN_DOWN ? 1 : PAGE_COUNT - 1)) % PAGE_COUNT;
        }
        s_dirty = true;
        render();
    }
    bsp_lvgl_unlock();
}
} // namespace

extern const passport_app_t g_clausium_app = {
    .id = APP_ID_CLAUSIUM,
    .name = "Clausium",
    .en_name = "CLAUSIUM",
    .desc = "Claude sessions\nthat need you",
    .tag = "LAN",
    .theme_color = 0xD97757,
    .init = nullptr,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .prepare_stop = prepare_stop,
};
