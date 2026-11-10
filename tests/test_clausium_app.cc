// Real Clausium app handlers, worker steps and lifecycle; LVGL/BSP/FreeRTOS,
// HTTP and NVS are mocked. The feed parser is the real one.
#include "../main/apps/clausium/app_clausium.cc"

static bool mock_awake = true, mock_configured = true, mock_wifi = true;
static clausium_http_result_t mock_http = CLAUSIUM_HTTP_OK;
static std::string mock_body;
static unsigned http_calls = 0, alert_polls = 0, app_starts = 0;
passport_app_id_t app_registry_get_active(){return APP_ID_CLAUSIUM;}
bool app_registry_screen_is_on(){return mock_awake;}
bool clausium_config_load(clausium_config_t *c){
    *c = {};std::snprintf(c->host,sizeof(c->host),"192.168.1.20");c->port=8790;
    std::snprintf(c->token,sizeof(c->token),"ABCDEFGH2345");return mock_configured;
}
bool clausium_net_connected(){return mock_wifi;}
clausium_http_result_t clausium_http_get(const clausium_config_t *c,const char *path,char *body,size_t max_body,
                                         size_t *len,int timeout_ms){
    assert(!std::strcmp(path,"/passport/v1")&&max_body==CLAUSIUM_FEED_MAX_BYTES&&timeout_ms==1500);
    assert(!std::strcmp(c->token,"ABCDEFGH2345")&&c->port==8790);
    ++http_calls;
    if(mock_http!=CLAUSIUM_HTTP_OK)return mock_http;
    assert(mock_body.size()<=max_body);
    std::memcpy(body,mock_body.c_str(),mock_body.size()+1);*len=mock_body.size();return CLAUSIUM_HTTP_OK;
}
bool clausium_alert_poll(const clausium_config_t *){++alert_polls;return true;}
void clausium_alert_app_started(){++app_starts;}

static const char *kSample =
    "{\"v\":1,\"t\":1762770000,\"n\":{\"wait\":1,\"work\":2,\"live\":3},"
    "\"need\":[{\"p\":\"passport-os\",\"f\":\"battut\",\"k\":\"permission\",\"a\":\"Bash: run tests\"}],"
    "\"live\":[{\"p\":\"claudedeck\",\"f\":\"personal\",\"s\":\"working\",\"l\":\"edit Station.swift\"}],"
    "\"plan\":[{\"f\":\"battut\",\"h5\":42,\"wk\":67,\"h5r\":1762780000,\"wkr\":1763200000}]}";

static void press(uint8_t key){g_clausium_app.on_key(key,BTN_EVT_CLICK,0);assert(mock_locks==0);}
static void ui_tick(){tick(nullptr);assert(mock_locks==0);}
static const std::string &row(int i){return s_rows[i]->text;}
static void finish_worker(){ // the real worker loop observes s_active=false and exits
    assert(!g_clausium_app.prepare_stop()||!s_worker_running.load());
    if(s_worker_running.load()){assert(!g_clausium_app.prepare_stop());mock_task_fn(nullptr);}
    assert(g_clausium_app.prepare_stop());
}
static void leave(){finish_worker();g_clausium_app.stop();assert(mock_objects==0&&mock_timers==0&&!s_ctx&&!s_rx);}

int main(){
    // Not configured: setup guidance, no worker, no allocation.
    mock_configured=false;g_clausium_app.start();
    assert(mock_tasks==0&&!s_ctx&&!s_rx&&mock_timers==1);
    assert(row(0)=="Set up Clausium on phone"&&row(2)=="Home: hold Down to open"&&s_rows[0]->color==UI_WARNING);
    assert(g_clausium_app.prepare_stop());press(BSP_BTN_OK);assert(!s_refresh.load());
    leave();
    const auto unconfigured_tasks=mock_tasks;assert(unconfigured_tasks==0);

    // Configured: connecting, then live data from the first poll.
    mock_configured=true;g_clausium_app.start();
    const auto page_objects=mock_objects;
    assert(mock_tasks==1&&s_ctx&&s_rx&&app_starts==1&&s_worker_running.load());
    assert(row(0)=="Connecting..."&&row(1)=="192.168.1.20:8790");
    assert(!g_clausium_app.prepare_stop()); // worker still owns the socket
    s_active.store(true);
    mock_body=kSample;assert(poll_once());assert(alert_polls==1);
    ui_tick();
    assert(row(0)=="Needs you · 1"&&s_rows[0]->color==UI_WARNING);
    assert(row(1)=="passport-os · approve?"&&s_rows[1]->color==UI_WARNING);
    assert(row(2)=="  battut · Bash: run tests"&&s_rows[2]->color==UI_TEXT_DIM);
    assert(row(3)=="Live · 3 (2 working)");
    assert(row(4)=="claudedeck · edit Station.swift"&&s_rows[4]->color==UI_TEXT);
    assert(row(5)=="+2 more"&&row(6).empty());
    assert(s_status->text=="Sessions 1/2");
    for(auto &b:s_usage){assert(b.name->hidden&&b.text[0]->hidden&&b.track[0]->hidden&&b.track[1]->hidden);}
    // Leaving the app: the feed GET finishes but the alert GET is skipped.
    s_active.store(false);assert(poll_once()&&alert_polls==1);s_active.store(true);ui_tick();
    // Usage page: one block per profile, countdowns from the feed's own clock.
    press(BSP_BTN_DOWN);
    assert(s_page==PAGE_USAGE&&s_status->text=="Usage 2/2");
    for(int r=0;r<kRows;++r)assert(row(r).empty()); // session list hidden
    assert(!s_usage[0].name->hidden&&s_usage[0].name->text=="battut"&&s_usage[1].name->hidden);
    assert(s_usage[0].text[0]->text=="5h 42% · resets 2h47m"&&s_usage[0].text[1]->text=="Wk 67% · resets 4d 23h");
    assert(s_usage[0].fill[0]->width==90&&s_usage[0].fill[1]->width==144);
    assert(s_usage[0].fill[0]->color==UI_SUCCESS&&s_usage[0].fill[1]->color==UI_SUCCESS);
    // Paging wraps both ways.
    press(BSP_BTN_DOWN);assert(s_page==PAGE_SESSIONS&&row(0)=="Needs you · 1"&&s_usage[0].name->hidden);
    press(BSP_BTN_UP);assert(s_page==PAGE_USAGE&&s_usage[0].text[0]->text=="5h 42% · resets 2h47m");
    press(BSP_BTN_UP);assert(s_page==PAGE_SESSIONS&&s_status->text=="Sessions 1/2");
    // OK asks the worker for an immediate poll.
    press(BSP_BTN_OK);assert(s_refresh.load());s_refresh.store(false);

    // A failed poll keeps fresh data with "Retrying"; after 30 s it is unreachable.
    mock_http=CLAUSIUM_HTTP_UNREACHABLE;assert(!poll_once());assert(alert_polls==1);ui_tick();
    assert(row(0)=="Needs you · 1"&&s_status->text=="Retrying"&&s_status->color==UI_WARNING);
    mock_now+=31000000;ui_tick();
    assert(row(0)=="Mac unreachable"&&row(1)=="192.168.1.20:8790"&&row(10)=="Last update 31s ago");
    mock_now+=1000000;ui_tick();assert(row(10)=="Last update 32s ago");
    // Stale data without a reported error.
    mock_http=CLAUSIUM_HTTP_OK;assert(poll_once());ui_tick();assert(row(0)=="Needs you · 1");
    mock_now+=31000000;ui_tick();assert(row(0)=="Data is stale"&&s_status->text=="Stale");
    // No Wi-Fi, bad reply (wrong version / oversize), wrong token.
    mock_wifi=false;assert(!poll_once());ui_tick();assert(row(0)=="No Wi-Fi");mock_wifi=true;
    mock_body="{\"v\":2}";assert(!poll_once());ui_tick();assert(row(0)=="Unexpected reply");
    mock_http=CLAUSIUM_HTTP_TOO_LARGE;assert(!poll_once());ui_tick();assert(row(0)=="Unexpected reply");
    mock_http=CLAUSIUM_HTTP_UNAUTHORIZED;mock_body=kSample;assert(!poll_once());ui_tick();
    assert(row(0)=="Wrong token"&&!s_have_data&&row(10).empty());
    assert(alert_polls==2); // only after understood feeds: no chime logic on errors
    // Wrong token wins over fresh data: the Mac no longer authorises this device.
    mock_http=CLAUSIUM_HTTP_OK;assert(poll_once());ui_tick();assert(row(0)=="Needs you · 1");
    mock_http=CLAUSIUM_HTTP_UNAUTHORIZED;assert(!poll_once());ui_tick();assert(row(0)=="Wrong token");
    mock_http=CLAUSIUM_HTTP_OK;

    // Many sessions page across screens; caps and "+N more" counts.
    mock_body="{\"v\":1,\"t\":1,\"n\":{\"wait\":8,\"work\":1,\"live\":12},\"need\":[";
    for(int i=0;i<6;++i)mock_body+=std::string(i?",":"")+"{\"p\":\"n"+std::to_string(i)+"\",\"f\":\"f\",\"k\":\"question\"}";
    mock_body+="],\"live\":[";
    for(int i=0;i<8;++i)mock_body+=std::string(i?",":"")+"{\"p\":\"l"+std::to_string(i)+"\",\"f\":\"f\",\"s\":\""+(i?"idle":"error")+"\",\"l\":\"x\"}";
    mock_body+="]}";
    assert(poll_once());ui_tick();
    // Too many lines: ask lines go first, then the tail is summarised.
    assert(row(0)=="Needs you · 8"&&row(1)=="n0 · question"&&row(6)=="n5 · question"&&row(7)=="+2 more waiting");
    assert(row(8)=="Live · 12 (1 working)"&&row(9)=="l0 · x"&&s_rows[9]->color==UI_DANGER);
    assert(row(10)=="+8 more lines"&&s_rows[10]->color==UI_TEXT_DIM&&s_status->text=="Sessions 1/2");
    press(BSP_BTN_DOWN);assert(row(0)=="No usage data yet"&&s_usage[0].name->hidden);
    press(BSP_BTN_DOWN);assert(s_page==PAGE_SESSIONS&&row(0)=="Needs you · 8");
    // Usage: four profiles, colour thresholds, unknown values, past resets.
    mock_body="{\"v\":1,\"t\":1000,\"n\":{\"wait\":0,\"work\":0,\"live\":0},\"plan\":["
        "{\"f\":\"a\",\"h5\":69,\"wk\":70,\"h5r\":1059,\"wkr\":4600},"
        "{\"f\":\"b\",\"h5\":89,\"wk\":90,\"h5r\":1000,\"wkr\":87400},"
        "{\"f\":\"\",\"h5\":-1,\"wk\":100,\"h5r\":0,\"wkr\":0},"
        "{\"f\":\"d\",\"h5\":0,\"wk\":5,\"h5r\":61,\"wkr\":1001}]}";
    assert(poll_once());ui_tick();press(BSP_BTN_DOWN);
    assert(s_usage[0].text[0]->text=="5h 69% · resets 1m"&&s_usage[0].fill[0]->color==UI_SUCCESS);
    assert(s_usage[0].text[1]->text=="Wk 70% · resets 1h00m"&&s_usage[0].fill[1]->color==UI_WARNING);
    assert(s_usage[1].text[0]->text=="5h 89% · resets now"&&s_usage[1].fill[0]->color==UI_WARNING);
    assert(s_usage[1].text[1]->text=="Wk 90% · resets 1d 0h"&&s_usage[1].fill[1]->color==UI_DANGER);
    assert(s_usage[2].name->text=="Claude"&&s_usage[2].text[0]->text=="5h -- · resets --"&&s_usage[2].fill[0]->hidden);
    assert(s_usage[2].text[1]->text=="Wk 100% · resets --"&&s_usage[2].fill[1]->width==kRowW);
    assert(s_usage[3].text[0]->text=="5h 0% · resets now"&&s_usage[3].fill[0]->hidden&&!s_usage[3].name->hidden);
    press(BSP_BTN_UP);
    // Empty desk.
    mock_body="{\"v\":1,\"t\":1,\"n\":{\"wait\":0,\"work\":0,\"live\":0}}";assert(poll_once());ui_tick();
    assert(row(0)=="Needs you · 0"&&row(1)=="Nobody is waiting"&&row(2)=="Live · 0 (0 working)"&&row(3)=="No live sessions");
    assert(s_status->text=="Sessions 1/2");

    // Screen off: the slot is consumed but labels are untouched until it is on.
    mock_body=kSample;assert(poll_once());mock_awake=false;ui_tick();
    assert(!s_ctx->slot_full.load()&&row(0)=="Needs you · 0");
    mock_awake=true;ui_tick();assert(row(0)=="Needs you · 1");
    // A full slot (UI busy) drops the sample after a bounded wait.
    assert(poll_once());const auto delays=mock_delays;assert(poll_once());
    assert(mock_delays==delays+20&&s_ctx->slot_full.load());ui_tick();

    // Worker cadence: 3 s after success, 15 s after errors; OK cuts the wait short.
    // The hook plays the UI timer, emptying the slot between polls.
    mock_delay_hook=[]{consume_slot();if(static_cast<int>(mock_delays)==mock_exit_after_delays)s_active.store(false);};
    s_active.store(true);mock_delays=0;mock_exit_after_delays=65;const auto calls=http_calls;
    mock_task_fn(nullptr);assert(http_calls==calls+3&&!s_worker_running.load()&&mock_task_deletes==1);
    ui_tick();
    s_active.store(true);s_worker_running.store(true);mock_delays=0;mock_exit_after_delays=160;
    mock_http=CLAUSIUM_HTTP_UNREACHABLE;const auto calls2=http_calls;
    mock_task_fn(nullptr);assert(http_calls==calls2+2&&mock_task_deletes==2);
    mock_http=CLAUSIUM_HTTP_OK;ui_tick();
    s_active.store(true);s_worker_running.store(true);mock_delays=0;mock_exit_after_delays=8;s_refresh.store(true);
    const auto calls3=http_calls;mock_task_fn(nullptr);assert(http_calls==calls3+2);ui_tick();
    mock_delay_hook=nullptr;
    // prepare_stop: false while the worker runs, true once it has exited.
    s_worker_running.store(true);s_active.store(true);
    assert(!g_clausium_app.prepare_stop()&&!s_active.load());
    mock_task_fn(nullptr);assert(g_clausium_app.prepare_stop());
    g_clausium_app.stop();assert(mock_objects==0&&mock_timers==0&&!s_ctx&&!s_rx);

    // Task creation failure releases memory and says so.
    mock_task_create_ok=false;g_clausium_app.start();assert(!s_ctx&&!s_rx&&row(0)=="Not enough memory");
    assert(g_clausium_app.prepare_stop());g_clausium_app.stop();assert(!mock_objects&&!mock_timers);
    mock_task_create_ok=true;

    // Repeated entry/exit cannot accumulate objects, timers or memory.
    for(int i=0;i<50;++i){
        g_clausium_app.start();assert(mock_objects==page_objects&&mock_timers==1&&s_ctx);
        s_active.store(true);assert(poll_once());ui_tick();
        leave();
    }
    mock_lock_available=false;g_clausium_app.start();assert(!s_screen&&!mock_objects);mock_lock_available=true;
    puts("Clausium application: setup guidance, Sessions/Usage pages with wrap, usage countdowns and colours, Retrying/unreachable/stale/no Wi-Fi/"
         "bad reply/wrong token states, paging, screen-off deferral, bounded slot, 3 s/15 s cadence, OK refresh, "
         "prepare_stop, task failure and 50 leak-free UI cycles PASS");
}
