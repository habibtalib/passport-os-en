#include "launcher/wifi_setup.h"
#include "launcher/app_registry.h"
#include "apps/home/home_upload.h"
#include "apps/home/home_profile.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "ui_pixel.h"
#include "app_fonts.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>

namespace {
std::atomic<bool> s_required{false};
std::atomic<bool> s_active{false};
std::atomic<bool> s_completing{false};
lv_obj_t *s_screen = nullptr;
lv_obj_t *s_status = nullptr;
ui_pixel_battery_t s_battery{};
uint32_t s_battery_tick;

lv_obj_t *line(const char *text, int y, uint32_t color, const lv_font_t *font = &buddy_font_16) {
    auto *label = ui_pixel_label(s_screen, text, font, color);
    lv_obj_set_pos(label, 12, y);
    lv_obj_set_width(label, 216);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}
}

void wifi_setup_require_initial(void) { s_required = true; }
bool wifi_setup_can_leave(void) { return !home_upload_busy(); }
void wifi_setup_init(void) {}

void wifi_setup_start(void) {
    s_active = true;
    home_profile_init();
    if (SsidManager::GetInstance().GetSsidList().empty()) s_required = true;
    auto &wifi = WifiManager::GetInstance();
    wifi.StartConfigAp(true); // DNS/AP only; one shared HTTP server owns the full flow.
    const auto err = home_upload_start();
    bsp_lvgl_lock(-1);
    s_screen = lv_obj_create(nullptr);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    ui_pixel_background(s_screen);
    auto *top=ui_pixel_top_bar(s_screen);
    auto *name=ui_pixel_label(top,"Phone Setup",&buddy_font_16,UI_TEXT);lv_obj_align(name,LV_ALIGN_LEFT_MID,12,0);
    s_battery=ui_pixel_battery_create(top,206,6,UI_TEXT,&lv_font_montserrat_14);
    ui_pixel_battery_set(&s_battery,bsp_battery_soc());s_battery_tick=lv_tick_get();
    auto *welcome = line(s_required?"Welcome to AI Passport OS":"Set up with phone",44,UI_TEXT,&ui_font_20);
    // The complete product name needs slightly more than the usual 216px row.
    lv_obj_set_pos(welcome, 8, 44);
    lv_obj_set_width(welcome, 224);
    line("1 Join device hotspot",84,UI_TEXT_DIM);
    line(wifi.GetApSsid().c_str(),112,UI_ACCENT,&lv_font_montserrat_14);
    line("2 Open in phone browser",158,UI_TEXT_DIM);
    line("192.168.4.1",190,UI_ACCENT,&lv_font_montserrat_32);
    s_status=line(err==ESP_OK?"If told no internet, keep":"Service off, OK to retry",242,UI_TEXT_DIM);
    lv_obj_set_height(s_status,22);lv_label_set_long_mode(s_status,LV_LABEL_LONG_DOT);
    ui_pixel_footer(s_screen,"Wi-Fi, profile & card",s_required?"Hold OK: skip setup":"Hold OK: Home",&buddy_font_16);
    lv_screen_load(s_screen);
    bsp_lvgl_unlock();
    app_registry_keep_awake();
}

void wifi_setup_stop(void) {
    s_active = false;
    home_upload_stop();
    WifiManager::GetInstance().StopConfigAp();
    if (!SsidManager::GetInstance().GetSsidList().empty()) WifiManager::GetInstance().StartStation();
    bsp_lvgl_lock(-1);
    if (s_screen) lv_obj_delete(s_screen);
    s_screen = s_status = nullptr;
    bsp_lvgl_unlock();
}

void wifi_setup_on_key(uint8_t btn, app_btn_event_t event, uint16_t) {
    if (btn == BSP_BTN_OK && event == BTN_EVT_CLICK && !home_upload_active()) {
        const auto err = home_upload_start();
        bsp_lvgl_lock(-1);
        ui_pixel_label_set_text(s_status, err == ESP_OK ? "Refresh the phone browser" : "Start failed, reboot");
        bsp_lvgl_unlock();
    }
}

void wifi_setup_tick(void) {
    if (!s_active.load()) return;
    app_registry_keep_awake();
    if (home_upload_finished()) {
        // Home's JPEG decoder needs >4 KB stack; never switch from sys_mon's 2560 B stack.
        if (!s_completing.exchange(true)) {
            if (xTaskCreate([](void *) {
                s_required = false;
                if (s_active.load()) app_registry_switch_to(APP_ID_HOME);
                s_completing = false;
                vTaskDelete(nullptr);
            }, "setup_done", 8192, nullptr, 4, nullptr) != pdPASS) s_completing = false;
        }
        return;
    }
    if (bsp_lvgl_lock(50)) {
        if(s_screen&&lv_tick_get()-s_battery_tick>=5000){ui_pixel_battery_set(&s_battery,bsp_battery_soc());s_battery_tick=lv_tick_get();}
        if (s_status) ui_pixel_label_set_text(s_status, home_upload_busy() ? "Connecting, keep hotspot" :
                    home_upload_network_verified() ? "Online, continue on phone" :
                    home_upload_active() ? "If told no internet, keep" : "Service off, OK to retry");
        bsp_lvgl_unlock();
    }
}

const passport_app_t g_wifi_app = {
    .id = APP_ID_WIFI, .name = "Phone Setup", .en_name = "PHONE SETUP",
    .desc = "Wi-Fi, profile and card", .tag = "SYSTEM", .theme_color = 0x181B1C,
    .init = wifi_setup_init, .start = wifi_setup_start,
    .stop = wifi_setup_stop, .on_key = wifi_setup_on_key,
};
