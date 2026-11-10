#pragma once
// C mocks for the Clausium alert service: FreeRTOS, heap, LVGL banner.
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
extern int mock_critical;
#define portENTER_CRITICAL(m) do { (void)(m); assert(mock_critical == 0); ++mock_critical; } while (0)
#define portEXIT_CRITICAL(m) do { (void)(m); assert(mock_critical == 1); --mock_critical; } while (0)
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int prio, TaskHandle_t *out);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);

#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
int64_t esp_timer_get_time(void);
uint32_t esp_get_free_heap_size(void);
#define MALLOC_CAP_INTERNAL 1
size_t heap_caps_get_largest_free_block(int caps);
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);

typedef struct lv_obj_t { struct lv_obj_t *parent; char text[96]; int children, x, y, w, h; } lv_obj_t;
typedef struct { int unused; } lv_font_t;
extern const lv_font_t buddy_font_16;
#define LV_TEXT_ALIGN_CENTER 0
#define LV_OPA_COVER 255
#define LV_LABEL_LONG_DOT 0
#define UI_ACCENT 3u
#define UI_ON_ACCENT 6u
lv_obj_t *lv_layer_top(void);
lv_obj_t *lv_label_create(lv_obj_t *parent);
void lv_obj_delete(lv_obj_t *obj);
void lv_label_set_text(lv_obj_t *obj, const char *text);
static inline uint32_t lv_color_hex(uint32_t c) { return c; }
static inline void lv_obj_set_style_text_font(lv_obj_t *o, const lv_font_t *f, int s) { (void)o; (void)f; (void)s; }
static inline void lv_obj_set_style_text_color(lv_obj_t *o, uint32_t c, int s) { (void)o; (void)c; (void)s; }
static inline void lv_obj_set_style_text_align(lv_obj_t *o, int a, int s) { (void)o; (void)a; (void)s; }
static inline void lv_obj_set_style_bg_color(lv_obj_t *o, uint32_t c, int s) { (void)o; (void)c; (void)s; }
static inline void lv_obj_set_style_bg_opa(lv_obj_t *o, int a, int s) { (void)o; (void)a; (void)s; }
static inline void lv_obj_set_style_pad_all(lv_obj_t *o, int p, int s) { (void)o; (void)p; (void)s; }
static inline void lv_obj_set_pos(lv_obj_t *o, int x, int y) { o->x = x; o->y = y; }
static inline void lv_obj_set_size(lv_obj_t *o, int w, int h) { o->w = w; o->h = h; }
static inline void lv_label_set_long_mode(lv_obj_t *o, int m) { (void)o; (void)m; }
