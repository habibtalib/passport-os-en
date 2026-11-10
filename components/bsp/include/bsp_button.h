// components/bsp/include/bsp_button.h
// Three buttons share one ADC pin and are told apart by divider resistors. See bsp_pins.h for the voltage windows.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Button index. The count uses BSP_BTN_COUNT from bsp_pins.h (a hardware property, owned by the pin table),
// so no trailing count item is defined here, avoiding two near-identical names BSP_BTN_COUNT / BSP_BTN_COUNT_.
typedef enum {
    BSP_BTN_UP = 0,
    BSP_BTN_DOWN,
    BSP_BTN_OK,
} bsp_btn_t;

typedef enum {
    BSP_BTN_PRESS = 0,   // press instant (low latency, suits instant response in games)
    BSP_BTN_RELEASE,     // release instant (Push-to-Talk "release to end" depends on it)
    BSP_BTN_CLICK,       // single click (press and release)
    BSP_BTN_DOUBLE,      // double click
    BSP_BTN_LONG,        // long press (instant the threshold is reached; UP key 0.5s = talk start)
    BSP_BTN_LONG_UP,     // release after a long press timeout (long-press state no longer reports RELEASE; UP key "talk end")
} bsp_btn_ev_t;

// Button event callback. Runs in the button component timer task; do not block or do heavy work in it.
typedef void (*bsp_btn_cb_t)(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user);

// Read the current raw ADC voltage (mV). About 3300 when released; about that key divider value while held.
// * After changing divider/pull-up resistors, use it to measure your three voltage levels, then edit BSP_BTN_MV_TABLE in bsp_pins.h.
// Returns -1 on read failure.
int bsp_button_read_mv(void);

// Panel power-on transient suppression: called at the entry of bsp_display_power(true). SPI activity during SLPOUT+DISPON+LVGL
// refresh couples into ADC1_CH0, the ADC keeps reading ~0mV -> false UP press -> false PTT
// (measured on device 2026-08-28). Inside the window, key detection simply returns "not pressed".
void bsp_button_suppress_panel_glitch(void);

#ifdef __cplusplus
}
#endif
