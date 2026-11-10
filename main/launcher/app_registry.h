#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the app registry
void app_registry_init(void);

// Get the app count (excluding Launcher)
size_t app_registry_get_count(void);

// Get the app descriptor at the given index
const passport_app_t *app_registry_get_by_index(size_t index);

// Get the app descriptor for the given ID
const passport_app_t *app_registry_get_by_id(passport_app_id_t id);

// Get the ID of the app currently active in the foreground
passport_app_id_t app_registry_get_active(void);

// Radio, Xiaozhi and Muse: apps that need a large, clean heap.
bool app_registry_is_heavy(passport_app_id_t id);

// Called once at boot: if the last boot was a soft reboot to enter a heavy app, return that app ID (otherwise
// APP_ID_COUNT). The record is cleared after reading.
passport_app_id_t app_registry_take_clean_launch(void);

// Switch the foreground app (passing APP_ID_LAUNCHER returns to the main menu)
void app_registry_switch_to(passport_app_id_t id);
// Run from the input task's full-size stack, never from sys_mon or LVGL.
void app_registry_process_pending_switch(void);

// Global key dispatch (includes 1.5s long-press OK interception and screen-off wake protection)
void app_registry_dispatch_key(uint8_t btn, app_btn_event_t event, uint16_t mv);

// Called by the system heartbeat; handles screen-off across Apps uniformly, without creating an extra resident task.
void app_registry_tick(void);
void app_registry_set_screen_timeout(uint16_t seconds);
uint16_t app_registry_get_screen_timeout(void);
bool app_registry_screen_is_on(void);
void app_registry_log_display_state(void);

// Activity (app activity) renewal: called by apps not driven by keys (e.g. voice-controlled games) while running,
// to suppress the screen-off timer. It only refreshes the timer and never lights a screen that is already off.
void app_registry_keep_awake(void);
// Caller holds LVGL lock. Wake without switching apps; false during handoff.
bool app_registry_wake_for_notice(void);
// Caller holds LVGL lock. Never wait for the lifecycle lock or seize audio.
bool app_registry_sound_notice(void);

#ifdef __cplusplus
}
#endif
