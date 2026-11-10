// components/bsp/src/bsp_button.c
// Ported from trae_card/components/platform/platform_esp32/src/btn_iot_button.c
#include "bsp_button.h"
#include "bsp_pins.h"
#include "iot_button.h"
#include "button_adc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_timer.h"

// Panel power-on transient suppression window: SLPOUT+120ms+DISPON+LVGL full-screen refresh ~300ms, use 600ms.
#define BSP_BTN_PANEL_GLITCH_US (600 * 1000)

// SPI activity at panel power-on couples into ADC1_CH0 (button divider); readings inside this window are untrustworthy.
// bsp_display_power(true) registers the suppression deadline before the wake sequence; see read_mv for the read path.
// Historically this was a patch to the managed_component button_adc.c, which a component upgrade would overwrite, so it was absorbed here.
static volatile int64_t s_ignore_until_us = 0;

void button_adc_set_ignore_until(int64_t until_us) {
    if (until_us > s_ignore_until_us) s_ignore_until_us = until_us;
}

static const char *TAG = "bsp_btn";

static const uint16_t BTN_MV[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;

static button_handle_t s_btn[BSP_BTN_COUNT];
static bsp_btn_cb_t    s_cb;
static void           *s_user;

// ADC1 is a unit-level exclusive resource: iot_button and bsp_button_read_mv() must share the same oneshot
// handle. Whoever calls adc_oneshot_new_unit() second gets "adc1 is already in use".
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;

// BSP polling and the button component share the attenuation setting, and calibration must match too, otherwise readings are misaligned with the button thresholds.
// managed_components/espressif__button/button_adc.c:26 uses ADC_ATTEN_DB_6+1 on the C3.
#define BSP_BTN_ATTEN  ADC_ATTEN_DB_12       // range about 0~3100mV, covers the released state

// Each button carries "which key" back with the callback. The button component callback signature is fixed, so the index is passed via usr_data.
static void on_event(void *arg, void *usr_data, bsp_btn_ev_t ev) {
    (void)arg;
    if (!s_cb) return;
    s_cb((bsp_btn_t)(intptr_t)usr_data, ev, s_user);
}
static void cb_press  (void *a, void *u) { on_event(a, u, BSP_BTN_PRESS);   }
static void cb_release(void *a, void *u) { on_event(a, u, BSP_BTN_RELEASE); }
static void cb_click  (void *a, void *u) { on_event(a, u, BSP_BTN_CLICK);   }
static void cb_double (void *a, void *u) { on_event(a, u, BSP_BTN_DOUBLE);  }
static void cb_long   (void *a, void *u) { on_event(a, u, BSP_BTN_LONG);    }
static void cb_long_up(void *a, void *u) { on_event(a, u, BSP_BTN_LONG_UP); }

esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user) {
    s_cb = cb; s_user = user;

    // BSP creates the unit first, then hands the handle to the button component (button_adc.h: a non-NULL adc_handle is reused),
    // so bsp_button_read_mv() in this file can also read the same ADC.
    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = BSP_BTN_ADC_UNIT };
    esp_err_t ae = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (ae != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit creation failed (%s)", esp_err_to_name(ae));
        s_adc = NULL;
        return ae;
    }

    // The main program uses polling mode without callbacks and never calls iot_button_new_adc_device.
    // So the channel must be configured by the BSP and cannot rely on the button component creation flow.
    const adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = BSP_BTN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ae = adc_oneshot_config_channel(s_adc, BSP_BTN_ADC_CHANNEL, &channel_cfg);
    if (ae != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed (%s)", esp_err_to_name(ae));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return ae;
    }

    if (cb) {
        for (int i = 0; i < BSP_BTN_COUNT; i++) {
            const button_adc_config_t ac = {
                .adc_handle   = &s_adc,          // reuse the one above, do not let the component create its own
                .unit_id      = BSP_BTN_ADC_UNIT,
                .adc_channel  = BSP_BTN_ADC_CHANNEL,
                .button_index = i,
                .min          = BTN_MV[i][0],
                .max          = BTN_MV[i][1],
            };
            button_config_t bc = { 0 };
            bc.long_press_time = 500;
            esp_err_t e = iot_button_new_adc_device(&bc, &ac, &s_btn[i]);
            if (e != ESP_OK || !s_btn[i]) {
                ESP_LOGE(TAG, "Button %d creation failed (%s) - check the ADC config and divider resistors on GPIO%d",
                         i, esp_err_to_name(e), BSP_BTN_ADC_CHANNEL);
                return e == ESP_OK ? ESP_FAIL : e;
            }
            void *idx = (void *)(intptr_t)i;
            iot_button_register_cb(s_btn[i], BUTTON_PRESS_DOWN,      NULL, cb_press,   idx);
            iot_button_register_cb(s_btn[i], BUTTON_PRESS_UP,        NULL, cb_release, idx);
            iot_button_register_cb(s_btn[i], BUTTON_SINGLE_CLICK,    NULL, cb_click,   idx);
            iot_button_register_cb(s_btn[i], BUTTON_DOUBLE_CLICK,    NULL, cb_double,  idx);
            iot_button_register_cb(s_btn[i], BUTTON_LONG_PRESS_START,NULL, cb_long,    idx);
            iot_button_register_cb(s_btn[i], BUTTON_LONG_PRESS_UP,  NULL, cb_long_up,  idx);
        }
    }

    // Channel is configured; create here a calibration handle matching its attenuation for bsp_button_read_mv().
    // Polling-mode key decisions depend on calibrated voltage, so failure must be reported; callback mode can use the component calibration.
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id  = BSP_BTN_ADC_UNIT,
        .chan     = BSP_BTN_ADC_CHANNEL,
        .atten    = BSP_BTN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ae = adc_cali_create_scheme_curve_fitting(&cal, &s_cali);
    if (ae != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration creation failed");
        s_cali = NULL;
        if (!cb) {
            adc_oneshot_del_unit(s_adc);
            s_adc = NULL;
            return ae;
        }
    }

    ESP_LOGI(TAG, "Buttons ready: ADC1_CH%d, 3-key divider", BSP_BTN_ADC_CHANNEL);
    return ESP_OK;
}

void bsp_button_suppress_panel_glitch(void) {
    button_adc_set_ignore_until(esp_timer_get_time() + BSP_BTN_PANEL_GLITCH_US);
}

int bsp_button_read_mv(void) {
    // Reads the ADC set up by bsp_button_init() and shared with iot_button.
    // A single sample and the component key polling do not interfere (oneshot has its own lock).
    if (!s_adc || !s_cali) return -1;
    if (esp_timer_get_time() < s_ignore_until_us) return -1;  // readings inside the suppression window are untrustworthy

    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, BSP_BTN_ADC_CHANNEL, &raw) != ESP_OK) return -1;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1;
    return mv;
}
