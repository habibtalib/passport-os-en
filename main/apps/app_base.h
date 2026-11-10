#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "bsp_button.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_ID_LAUNCHER = 0,
    APP_ID_RADIO,
    APP_ID_VOICE,
    APP_ID_XIAOZHI,
    APP_ID_WIFI,
    APP_ID_SETTINGS,
    APP_ID_HOME,
    APP_ID_GAME,
    APP_ID_TETRIS,
    APP_ID_STOPWATCH,
    APP_ID_SOUND_JUMP,
    APP_ID_MUSE,
    APP_ID_POMODORO,
    APP_ID_COUNT
} passport_app_id_t;

typedef enum {
    BTN_EVT_CLICK = 0,
    BTN_EVT_PRESS,
    BTN_EVT_RELEASE,
    BTN_EVT_LONG,
    BTN_EVT_LONG_UP,
} app_btn_event_t;

typedef struct {
    passport_app_id_t id;
    const char *name;           // Display name
    const char *en_name;        // English label
    const char *desc;           // Short description
    const char *tag;            // Protocol/network tag (e.g. "Wi-Fi", "BLE")
    uint32_t theme_color;       // Theme color (RGB888)

    // Lifecycle hooks
    void (*init)(void);         // Init at startup
    void (*start)(void);        // Enter foreground
    void (*stop)(void);         // Leave foreground (clean up tasks, drop network, free heap)
    void (*on_key)(uint8_t btn, app_btn_event_t event, uint16_t mv); // Key event dispatch
    // Request cooperative shutdown before releasing the UI or starting another
    // app. Return false while a worker still owns network/audio resources.
#ifdef __cplusplus
    bool (*prepare_stop)(void) = nullptr;
#else
    bool (*prepare_stop)(void);
#endif
} passport_app_t;

#ifdef __cplusplus
}
#endif
