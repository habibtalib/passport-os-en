#pragma once
// LVGL/BSP/FreeRTOS mocks for the Clausium app (C++), modelled on pomodoro_mocks.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

inline unsigned mock_objects = 0, mock_timers = 0, mock_tasks = 0, mock_task_deletes = 0, mock_delays = 0;
inline unsigned mock_heap = 100000;
inline int mock_locks = 0, mock_exit_after_delays = -1;
inline bool mock_lock_available = true;
inline int64_t mock_now = 1000000;
inline void (*mock_task_fn)(void *) = nullptr;
inline void (*mock_delay_hook)() = nullptr;
constexpr uint8_t BSP_BTN_OK=0, BSP_BTN_UP=1, BSP_BTN_DOWN=2;
constexpr int MALLOC_CAP_INTERNAL=1, LV_OBJ_FLAG_SCROLLABLE=1, LV_OBJ_FLAG_HIDDEN=2;
constexpr int LV_LABEL_LONG_DOT=0, LV_TEXT_ALIGN_CENTER=0, LV_TEXT_ALIGN_RIGHT=2, LV_ALIGN_LEFT_MID=0, LV_OPA_COVER=255;
constexpr uint32_t UI_BG=0, UI_TEXT=1, UI_TEXT_DIM=2, UI_ACCENT=3,
    UI_WARNING=4, UI_SURFACE_ALT=5, UI_ON_ACCENT=6, UI_DANGER=7, UI_SUCCESS=8;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
struct lv_font_t {};
inline const lv_font_t buddy_font_16{}, lv_font_montserrat_14{};
struct lv_obj_t {
    lv_obj_t *parent=nullptr;
    std::vector<lv_obj_t*> children;
    std::string text;
    int width=0;
    uint32_t color=0;
    bool hidden=false;
};
struct lv_timer_t {};
inline int64_t esp_timer_get_time(){return mock_now;}
inline unsigned esp_get_free_heap_size(){return mock_heap;}
inline bool bsp_lvgl_lock(int){if(!mock_lock_available)return false;++mock_locks;return true;}
inline void bsp_lvgl_unlock(){assert(mock_locks>0);--mock_locks;}
inline int bsp_battery_soc(){return 70;}
inline uint32_t lv_color_hex(uint32_t c){return c;}
inline lv_obj_t *lv_obj_create(lv_obj_t *parent){
    auto *p=new lv_obj_t;p->parent=parent;if(parent)parent->children.push_back(p);++mock_objects;return p;
}
inline lv_obj_t *lv_label_create(lv_obj_t *parent){return lv_obj_create(parent);}
inline void lv_obj_delete(lv_obj_t *p){
    while(!p->children.empty())lv_obj_delete(p->children.back());
    if(p->parent){auto &v=p->parent->children;for(auto it=v.begin();it!=v.end();++it)if(*it==p){v.erase(it);break;}}
    assert(mock_objects);--mock_objects;delete p;
}
inline void lv_label_set_long_mode(lv_obj_t*,int){}
inline void lv_obj_set_pos(lv_obj_t*,int,int){}
inline void lv_obj_set_size(lv_obj_t *p,int w,int){p->width=w;}
inline void lv_obj_set_width(lv_obj_t *p,int w){p->width=w;}
inline void lv_obj_set_style_text_align(lv_obj_t*,int,int){}
inline void lv_obj_set_style_bg_color(lv_obj_t *p,uint32_t c,int){p->color=c;}
inline void lv_obj_set_style_border_width(lv_obj_t*,int,int){}
inline void lv_obj_set_style_pad_all(lv_obj_t*,int,int){}
inline void lv_obj_set_style_radius(lv_obj_t*,int,int){}
inline void lv_obj_remove_flag(lv_obj_t*,int){}
inline void lv_obj_set_flag(lv_obj_t *p,int f,bool v){if(f==LV_OBJ_FLAG_HIDDEN)p->hidden=v;}
inline void lv_obj_align(lv_obj_t*,int,int,int){}
inline void lv_screen_load(lv_obj_t*){}
inline lv_timer_t *lv_timer_create(void(*)(lv_timer_t*),int,void*){++mock_timers;return new lv_timer_t;}
inline void lv_timer_delete(lv_timer_t *p){assert(mock_timers);--mock_timers;delete p;}
struct ui_pixel_battery_t { lv_obj_t *fill=nullptr,*label=nullptr; };
inline lv_obj_t *ui_pixel_label(lv_obj_t *parent,const char *s,const lv_font_t*,uint32_t c){auto *p=lv_label_create(parent);p->text=s;p->color=c;return p;}
inline void ui_pixel_label_set_text(lv_obj_t *p,const char *s){p->text=s;}
inline void ui_pixel_text_color(lv_obj_t *p,uint32_t c){p->color=c;}
inline void ui_pixel_bg_color(lv_obj_t *p,uint32_t c){p->color=c;}
inline void ui_pixel_background(lv_obj_t*){}
inline lv_obj_t *ui_pixel_top_bar(lv_obj_t *p){return lv_obj_create(p);}
inline ui_pixel_battery_t ui_pixel_battery_create(lv_obj_t*,int,int,uint32_t,const lv_font_t*){return {};}
inline void ui_pixel_battery_set(ui_pixel_battery_t*,int){}
inline lv_obj_t *ui_pixel_footer(lv_obj_t *parent,const char *a,const char *b,const lv_font_t *f){
    ui_pixel_label(parent,b,f,0);return ui_pixel_label(parent,a,f,0);
}
// FreeRTOS
typedef void *TaskHandle_t;
typedef int BaseType_t;
constexpr BaseType_t pdPASS=1;
#define pdMS_TO_TICKS(ms) (ms)
inline bool mock_task_create_ok = true;
inline BaseType_t xTaskCreate(void (*fn)(void *), const char *, uint32_t stack, void *, int, TaskHandle_t *out){
    assert(stack==5120);if(!mock_task_create_ok)return 0;mock_task_fn=fn;++mock_tasks;*out=reinterpret_cast<TaskHandle_t>(1);return pdPASS;
}
inline void vTaskDelay(int ms){++mock_delays;mock_now+=ms*1000LL;if(mock_delay_hook)mock_delay_hook();}
inline void vTaskDelete(TaskHandle_t t){assert(!t);++mock_task_deletes;}
inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t){return 2048;}
