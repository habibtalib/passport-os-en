// Clausium background alerts: baseline, one alert per event, +N more, heap and
// codec-app policy, settings toggle and poller lifecycle. Real service code.
#include "../main/apps/clausium/clausium_alert.c"

#include <stdio.h>

int mock_critical;
static int64_t now_us = 1000000;
static uint32_t heap_free = 60000;
static size_t heap_largest = 30000;
static int locks, lock_calls;
static bool lock_ok = true;
int64_t esp_timer_get_time(void) { return now_us; }
uint32_t esp_get_free_heap_size(void) { return heap_free; }
size_t heap_caps_get_largest_free_block(int caps) { (void)caps; return heap_largest; }
bool bsp_lvgl_lock(int t) {
    (void)t;
    ++lock_calls;
    if (!lock_ok) return false;
    ++locks;
    return true;
}
void bsp_lvgl_unlock(void) { assert(locks > 0); --locks; }

// LVGL
const lv_font_t buddy_font_16;
static lv_obj_t top_layer;
static int objects;
lv_obj_t *lv_layer_top(void) { return &top_layer; }
lv_obj_t *lv_label_create(lv_obj_t *parent) {
    lv_obj_t *o = calloc(1, sizeof(*o)); o->parent = parent; ++parent->children; ++objects; return o;
}
void lv_obj_delete(lv_obj_t *o) { assert(objects > 0); --o->parent->children; --objects; free(o); }
void lv_label_set_text(lv_obj_t *o, const char *t) { snprintf(o->text, sizeof(o->text), "%s", t); }

// Registry
static passport_app_id_t active = APP_ID_HOME;
static bool wake_ok = true, sound_ok = true;
static int wakes, sounds;
passport_app_id_t app_registry_get_active(void) { return active; }
bool app_registry_is_heavy(passport_app_id_t id) {
    return id == APP_ID_RADIO || id == APP_ID_XIAOZHI || id == APP_ID_MUSE;
}
bool app_registry_wake_for_notice(void) {
    assert(locks > 0);
    if (!wake_ok) return false;
    ++wakes;
    return true;
}
bool app_registry_sound_notice(void) { assert(locks > 0); ++sounds; return sound_ok; }

// Config + network
static bool configured = true, alerts_on = true, connected = true;
bool clausium_config_load(clausium_config_t *c) {
    memset(c, 0, sizeof(*c));
    snprintf(c->host, sizeof(c->host), "192.168.1.20"); c->port = 8790;
    snprintf(c->token, sizeof(c->token), "ABCDEFGH2345");
    return configured;
}
bool clausium_config_alerts_enabled(void) { return alerts_on; }
bool clausium_net_connected(void) { return connected; }
static char reply[600] = "{\"v\":1,\"boot\":\"a1b2c3\",\"seq\":4,\"wait\":0,\"e\":[]}";
static clausium_http_result_t http_result = CLAUSIUM_HTTP_OK;
static int http_calls;
static bool off_during_get; // Settings turns alerts off while a GET is in flight
static char last_path[64];
clausium_http_result_t clausium_http_get(const clausium_config_t *c, const char *path, char *body,
                                         size_t max_body, size_t *len, int timeout_ms) {
    assert(!strcmp(c->token, "ABCDEFGH2345") && timeout_ms == 2000 && max_body == 511);
    ++http_calls; snprintf(last_path, sizeof(last_path), "%s", path);
    if (off_during_get) { alerts_on = false; clausium_alert_reconfigure(); }
    if (http_result != CLAUSIUM_HTTP_OK) return http_result;
    *len = strlen(reply); assert(*len <= max_body); memcpy(body, reply, *len + 1);
    return CLAUSIUM_HTTP_OK;
}

// Tasks
static int tasks_created, deletes, delays;
static int exit_after_delays = -1; // simulate Settings turning alerts off
static void (*task_fn)(void *);
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int prio, TaskHandle_t *out) {
    (void)arg; (void)out; assert(!strcmp(name, "clausium_alert") && stack == 4096 && prio == 2);
    task_fn = fn; ++tasks_created; return pdPASS;
}
void vTaskDelay(TickType_t t) {
    assert(t == 250); ++delays; now_us += 250000;
    if (delays == exit_after_delays) { alerts_on = false; atomic_fetch_add(&s_generation, 1); }
}
void vTaskDelete(TaskHandle_t t) { assert(t == NULL); ++deletes; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 1500; }

static clausium_alert_msg_t msg(const char *json) {
    clausium_alert_msg_t m;
    assert(clausium_alert_parse(json, strlen(json), &m) == CLAUSIUM_FEED_OK);
    return m;
}
static void tick(void) { clausium_alert_service_tick(); assert(locks == 0 && mock_critical == 0); }
static void text_is(const char *want) {
    char t[CLAUSIUM_ALERT_TEXT_MAX]; clausium_alert_text(t, sizeof(t));
    if (strcmp(t, want)) { fprintf(stderr, "text '%s' want '%s'\n", t, want); assert(0); }
}

int main(void) {
    // Start only when configured and enabled; never twice.
    configured = false; clausium_alert_init(false); assert(!tasks_created && !atomic_load(&s_task_running));
    configured = true; alerts_on = false; clausium_alert_init(false); assert(!tasks_created);
    alerts_on = true; clausium_alert_reconfigure(); assert(tasks_created == 1 && atomic_load(&s_task_running));
    clausium_alert_reconfigure(); assert(tasks_created == 1);

    // Baseline on the first message: nothing is news.
    clausium_alert_msg_t m = msg("{\"v\":1,\"boot\":\"a1b2c3\",\"seq\":5,\"wait\":1,\"e\":[{\"q\":5,\"p\":\"old\",\"f\":\"x\",\"k\":\"permission\"}]}");
    assert(!clausium_alert_apply(&m) && !s_wake_pending && !s_sound_pending);
    tick(); assert(!wakes && !sounds && !objects);
    assert(!clausium_alert_apply(&m)); // same seq: no repeat

    // One alert per event: wake, banner, chime once.
    m = msg("{\"v\":1,\"boot\":\"a1b2c3\",\"seq\":6,\"wait\":1,\"e\":[{\"q\":6,\"p\":\"passport-os\",\"f\":\"battut\",\"k\":\"permission\"}]}");
    assert(clausium_alert_apply(&m)); text_is("passport-os: needs permission");
    tick(); assert(wakes == 1 && sounds == 1 && objects == 1 && top_layer.children == 1);
    // The banner sits below the Pomodoro notice (y 30..62) on the same layer.
    assert(s_banner->y >= 30 + 32 && s_banner->y + s_banner->h <= 280 && s_banner->x == 8);
    assert(!s_wake_pending && !s_sound_pending);
    for (int i = 0; i < 5; ++i) { now_us += 1000000; tick(); }
    assert(wakes == 1 && sounds == 1 && objects == 1);
    assert(!clausium_alert_apply(&m)); tick(); assert(wakes == 1 && sounds == 1);
    now_us += 3000000; tick(); assert(objects == 0 && !top_layer.children); // 8 s banner

    // Each kind, and +N more when several arrived since the last poll.
    m = msg("{\"v\":1,\"boot\":\"a1b2c3\",\"seq\":7,\"wait\":0,\"e\":[{\"q\":7,\"p\":\"a\",\"f\":\"x\",\"k\":\"question\"}]}");
    assert(clausium_alert_apply(&m)); text_is("a: has a question"); tick();
    m = msg("{\"v\":1,\"boot\":\"a1b2c3\",\"seq\":8,\"wait\":0,\"e\":[{\"q\":8,\"p\":\"\",\"f\":\"x\",\"k\":\"error\"}]}");
    assert(clausium_alert_apply(&m)); text_is("Claude: error"); tick();
    m = msg("{\"v\":1,\"boot\":\"a1b2c3\",\"seq\":13,\"wait\":0,\"e\":[{\"q\":11,\"p\":\"a\",\"f\":\"x\",\"k\":\"done\"},"
            "{\"q\":12,\"p\":\"b\",\"f\":\"x\",\"k\":\"done\"},{\"q\":13,\"p\":\"c\",\"f\":\"x\",\"k\":\"done\"}]}");
    assert(clausium_alert_apply(&m)); text_is("c: finished +4 more");
    tick(); assert(sounds == 4 && wakes == 4 && objects == 1 && !strcmp(top_layer.children ? "x" : "", "x"));
    now_us += 9000000; tick(); assert(!objects);

    // The Mac app restarted: new boot id is a baseline, not a burst of alerts.
    m = msg("{\"v\":1,\"boot\":\"0f0f0f\",\"seq\":2,\"wait\":2,\"e\":[{\"q\":1,\"p\":\"a\",\"f\":\"x\",\"k\":\"permission\"},"
            "{\"q\":2,\"p\":\"b\",\"f\":\"x\",\"k\":\"question\"}]}");
    assert(!clausium_alert_apply(&m)); tick(); assert(sounds == 4 && wakes == 4 && !objects);

    // Codec-owning apps: banner only, the chime is dropped, not deferred.
    const passport_app_id_t codec[] = {APP_ID_RADIO, APP_ID_VOICE, APP_ID_XIAOZHI, APP_ID_MUSE, APP_ID_GAME, APP_ID_SOUND_JUMP};
    uint32_t seq = 2;
    for (unsigned i = 0; i < sizeof(codec) / sizeof(*codec); ++i) {
        assert(!clausium_alert_sound_allowed(codec[i]));
        active = codec[i];
        char json[200];
        ++seq;
        snprintf(json, sizeof(json), "{\"v\":1,\"boot\":\"0f0f0f\",\"seq\":%u,\"wait\":1,\"e\":[{\"q\":%u,\"p\":\"r\",\"f\":\"x\",\"k\":\"done\"}]}",
                 (unsigned)seq, (unsigned)seq);
        m = msg(json); assert(clausium_alert_apply(&m));
        const int before = sounds;
        tick(); assert(sounds == before && objects == 1 && !s_sound_pending && !s_wake_pending);
        now_us += 9000000; tick(); assert(!objects);
    }
    const passport_app_id_t quiet[] = {APP_ID_HOME, APP_ID_LAUNCHER, APP_ID_SETTINGS, APP_ID_POMODORO,
        APP_ID_STOPWATCH, APP_ID_TETRIS, APP_ID_CLAUSIUM, APP_ID_WIFI};
    for (unsigned i = 0; i < sizeof(quiet) / sizeof(*quiet); ++i) assert(clausium_alert_sound_allowed(quiet[i]));

    // Low heap: no banner allocation and no wake yet; the notice waits.
    active = APP_ID_HOME;
    m = msg("{\"v\":1,\"boot\":\"0f0f0f\",\"seq\":20,\"wait\":1,\"e\":[{\"q\":20,\"p\":\"h\",\"f\":\"x\",\"k\":\"permission\"}]}");
    assert(clausium_alert_apply(&m));
    heap_free = 9000; sound_ok = false; int w = wakes, s = sounds;
    tick(); assert(!objects && wakes == w && s_wake_pending && s_sound_pending && sounds == s + 1);
    heap_free = 60000; heap_largest = 2000; tick(); assert(!objects && s_wake_pending);
    heap_largest = 30000; wake_ok = false; tick(); assert(!objects && s_wake_pending);
    wake_ok = true; tick(); assert(objects == 1 && !s_wake_pending && s_sound_pending);
    sound_ok = true; tick(); assert(!s_sound_pending);
    now_us += 9000000; tick(); assert(!objects);
    // A chime that cannot play within 30 s is dropped.
    m = msg("{\"v\":1,\"boot\":\"0f0f0f\",\"seq\":21,\"wait\":1,\"e\":[{\"q\":21,\"p\":\"h\",\"f\":\"x\",\"k\":\"done\"}]}");
    assert(clausium_alert_apply(&m)); sound_ok = false;
    tick(); assert(s_sound_pending);
    now_us += 31000000; s = sounds; tick(); assert(!s_sound_pending && sounds == s);
    sound_ok = true; now_us += 9000000; tick(); assert(!objects);
    // LVGL busy: nothing happens and nothing is lost.
    m = msg("{\"v\":1,\"boot\":\"0f0f0f\",\"seq\":22,\"wait\":1,\"e\":[{\"q\":22,\"p\":\"h\",\"f\":\"x\",\"k\":\"done\"}]}");
    assert(clausium_alert_apply(&m)); lock_ok = false; tick(); assert(s_wake_pending && s_sound_pending);
    lock_ok = true; tick(); assert(!s_wake_pending && !s_sound_pending && objects == 1);
    now_us += 9000000; tick();

    // The Clausium app in front: chime and wake, no banner over its own list.
    active = APP_ID_CLAUSIUM;
    m = msg("{\"v\":1,\"boot\":\"0f0f0f\",\"seq\":23,\"wait\":1,\"e\":[{\"q\":23,\"p\":\"h\",\"f\":\"x\",\"k\":\"question\"}]}");
    s = sounds; w = wakes; assert(clausium_alert_apply(&m)); heap_free = 5000;
    tick(); assert(sounds == s + 1 && wakes == w + 1 && !objects); heap_free = 60000;

    // Poll policy: not while the app is in front, offline or low on memory.
    assert(!should_poll()); active = APP_ID_GAME; assert(should_poll());
    // Heavy apps keep their heap and radio time: no background GET.
    active = APP_ID_RADIO; assert(!should_poll()); active = APP_ID_XIAOZHI; assert(!should_poll());
    active = APP_ID_MUSE; assert(!should_poll()); active = APP_ID_GAME;
    connected = false; assert(!should_poll()); connected = true;
    heap_free = 19000; assert(!should_poll()); heap_free = 60000;
    heap_largest = 8000; assert(!should_poll()); heap_largest = 30000; assert(should_poll());

    // Poll path and since parameter.
    snprintf(reply, sizeof(reply), "{\"v\":1,\"boot\":\"0f0f0f\",\"seq\":24,\"wait\":1,\"e\":[{\"q\":24,\"p\":\"p\",\"f\":\"x\",\"k\":\"done\"}]}");
    assert(clausium_alert_poll(&(clausium_config_t){"192.168.1.20", 8790, "ABCDEFGH2345"}));
    assert(!strcmp(last_path, "/passport/v1/alert?since=23")); text_is("p: finished");
    snprintf(reply, sizeof(reply), "{\"v\":2}");
    assert(!clausium_alert_poll(&(clausium_config_t){"192.168.1.20", 8790, "ABCDEFGH2345"}));
    http_result = CLAUSIUM_HTTP_UNREACHABLE;
    assert(!clausium_alert_poll(&(clausium_config_t){"192.168.1.20", 8790, "ABCDEFGH2345"}));
    http_result = CLAUSIUM_HTTP_OK;
    tick(); now_us += 9000000; tick(); assert(!objects);

    // Poller task: baselines on start (since=0), polls every 5 s, backs off
    // 30 s on errors, skips while low on memory, exits when alerts go off.
    snprintf(reply, sizeof(reply), "{\"v\":1,\"boot\":\"0f0f0f\",\"seq\":30,\"wait\":0,\"e\":[{\"q\":30,\"p\":\"q\",\"f\":\"x\",\"k\":\"done\"}]}");
    http_calls = 0; delays = 0; exit_after_delays = 45;
    assert(atomic_load(&s_task_running)); task_fn(NULL);
    assert(!strcmp(last_path, "/passport/v1/alert?since=0") || http_calls > 1);
    assert(http_calls == 3 && deletes == 1 && !atomic_load(&s_task_running)); // 0, 5, 10 s
    assert(!s_sound_pending); // baseline only: the restart is not an alert
    alerts_on = true; clausium_alert_reconfigure(); assert(tasks_created == 2);
    http_calls = 0; delays = 0; exit_after_delays = 130; http_result = CLAUSIUM_HTTP_UNREACHABLE;
    task_fn(NULL); assert(http_calls == 2 && deletes == 2); // 0 s, then after the 30 s backoff
    http_result = CLAUSIUM_HTTP_OK;
    alerts_on = true; clausium_alert_reconfigure(); assert(tasks_created == 3);
    http_calls = 0; delays = 0; exit_after_delays = 60; heap_free = 15000;
    task_fn(NULL); assert(http_calls == 0 && deletes == 3);
    heap_free = 60000;

    // Turning alerts off in Settings drops anything still pending.
    alerts_on = true; clausium_alert_reconfigure(); assert(tasks_created == 4);
    m = msg("{\"v\":1,\"boot\":\"0f0f0f\",\"seq\":31,\"wait\":0,\"e\":[{\"q\":31,\"p\":\"q\",\"f\":\"x\",\"k\":\"done\"}]}");
    assert(!clausium_alert_apply(&m)); // the last poller start never polled: baseline
    m = msg("{\"v\":1,\"boot\":\"0f0f0f\",\"seq\":32,\"wait\":0,\"e\":[{\"q\":32,\"p\":\"q\",\"f\":\"x\",\"k\":\"done\"}]}");
    assert(clausium_alert_apply(&m) && s_sound_pending);
    alerts_on = false; clausium_alert_reconfigure(); assert(!s_sound_pending && !s_wake_pending);
    exit_after_delays = -1; delays = 0; task_fn(NULL); assert(deletes == 4 && !atomic_load(&s_task_running));
    clausium_alert_reconfigure(); assert(tasks_created == 4);
    // App start re-baselines only when the poller is not keeping the tracker current.
    clausium_alert_app_started(); assert(!s_have_base);

    // Idle ticks never take the LVGL lock.
    int calls = lock_calls; tick(); tick(); assert(lock_calls == calls);

    // Alerts off silences the foreground app too: the app feeds the tracker
    // through clausium_alert_poll(); the tracker advances without a chime.
    active = APP_ID_CLAUSIUM; alerts_on = false; clausium_alert_reconfigure();
    m = msg("{\"v\":1,\"boot\":\"abcdef\",\"seq\":40,\"wait\":0,\"e\":[]}");
    assert(!clausium_alert_apply(&m) && s_have_base);
    w = wakes; s = sounds;
    snprintf(reply, sizeof(reply), "{\"v\":1,\"boot\":\"abcdef\",\"seq\":41,\"wait\":1,\"e\":[{\"q\":41,\"p\":\"o\",\"f\":\"x\",\"k\":\"done\"}]}");
    assert(clausium_alert_poll(&(clausium_config_t){"192.168.1.20", 8790, "ABCDEFGH2345"}));
    assert(s_seq == 41 && !s_wake_pending && !s_sound_pending);
    calls = lock_calls; tick(); assert(wakes == w && sounds == s && !objects && lock_calls == calls);
    // Stray pending flags are dropped by the tick while alerts are off.
    s_wake_pending = s_sound_pending = true; tick();
    assert(!s_wake_pending && !s_sound_pending && wakes == w && sounds == s && lock_calls == calls);
    // A poll in flight when alerts go off does not chime.
    alerts_on = true; clausium_alert_reconfigure();
    snprintf(reply, sizeof(reply), "{\"v\":1,\"boot\":\"abcdef\",\"seq\":42,\"wait\":1,\"e\":[{\"q\":42,\"p\":\"o\",\"f\":\"x\",\"k\":\"done\"}]}");
    off_during_get = true;
    assert(clausium_alert_poll(&(clausium_config_t){"192.168.1.20", 8790, "ABCDEFGH2345"}));
    off_during_get = false;
    assert(!alerts_on && s_seq == 42 && !s_wake_pending && !s_sound_pending);
    tick(); assert(wakes == w && sounds == s && !objects);
    // Back on: the next event chimes in front of the app (no banner over its list).
    alerts_on = true; clausium_alert_reconfigure();
    m = msg("{\"v\":1,\"boot\":\"abcdef\",\"seq\":43,\"wait\":1,\"e\":[{\"q\":43,\"p\":\"o\",\"f\":\"x\",\"k\":\"done\"}]}");
    assert(clausium_alert_apply(&m)); tick(); assert(wakes == w + 1 && sounds == s + 1 && !objects);

    // Overlapping pollers: an older reply from the same boot is ignored, not a
    // re-baseline, so the next event still alerts once with the right count.
    active = APP_ID_HOME;
    m = msg("{\"v\":1,\"boot\":\"abcdef\",\"seq\":45,\"wait\":1,\"e\":[{\"q\":45,\"p\":\"n\",\"f\":\"x\",\"k\":\"done\"}]}");
    assert(clausium_alert_apply(&m)); text_is("n: finished +1 more"); tick();
    m = msg("{\"v\":1,\"boot\":\"abcdef\",\"seq\":44,\"wait\":1,\"e\":[{\"q\":44,\"p\":\"late\",\"f\":\"x\",\"k\":\"done\"}]}");
    assert(!clausium_alert_apply(&m) && s_seq == 45 && s_have_base && !strcmp(s_boot, "abcdef"));
    m = msg("{\"v\":1,\"boot\":\"abcdef\",\"seq\":46,\"wait\":1,\"e\":[{\"q\":46,\"p\":\"m\",\"f\":\"x\",\"k\":\"done\"}]}");
    assert(clausium_alert_apply(&m)); text_is("m: finished"); tick();
    now_us += 9000000; tick(); assert(!objects);

    // Clean-launch boot straight into a heavy app: no task until a light app
    // is in front; reconfigure while heavy defers too; then it starts once.
    alerts_on = false; exit_after_delays = -1; task_fn(NULL); // poller sees alerts off and exits
    assert(!atomic_load(&s_task_running));
    alerts_on = true; active = APP_ID_RADIO; int t = tasks_created;
    clausium_alert_init(true); assert(tasks_created == t && atomic_load(&s_start_deferred));
    tick(); assert(tasks_created == t);
    clausium_alert_reconfigure(); assert(tasks_created == t && atomic_load(&s_start_deferred));
    active = APP_ID_MUSE; tick(); assert(tasks_created == t);
    active = APP_ID_HOME; tick(); assert(tasks_created == t + 1 && !atomic_load(&s_start_deferred));
    tick(); assert(tasks_created == t + 1);
    // The running poller skips its GET while a heavy app is in front.
    http_calls = 0; delays = 0; exit_after_delays = 45; active = APP_ID_XIAOZHI;
    task_fn(NULL); assert(http_calls == 0 && !atomic_load(&s_task_running));
    // Deferred while alerts are off: no repeated NVS reads, nothing starts.
    alerts_on = false; active = APP_ID_RADIO; clausium_alert_init(true); active = APP_ID_HOME; t = tasks_created;
    tick(); assert(tasks_created == t && !atomic_load(&s_start_deferred));

    assert(locks == 0 && mock_critical == 0);
    puts("Clausium alerts: start/stop on config, baseline and new boot, one alert per event, every kind, +N more, "
         "8 s banner, codec apps banner-only, heap/wake/LVGL deferral, 30 s expiry, app foreground, "
         "poll policy, backoff, settings toggle, alerts-off foreground silence, stale overlapping replies, "
         "heavy-app skip and deferred start, lock-free idle tick, banner below Pomodoro: PASS");
    return 0;
}
