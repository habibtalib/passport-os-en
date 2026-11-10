// main/app_protocol.c -- protocol implementation.
// Field contract: see the protocol section of prd.md; any parse failure only returns false, never crashes.
#include "app_protocol.h"
#include "app_events.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

// JSON nesting depth limit: parsing runs in the NimBLE host task context (default stack 4KB, the review counted a 2048B stack
// path also in use), and cJSON's recursive parsing has no depth limit -- deep nesting within a 2KB payload
// (e.g. [[[[...]]]]) can overflow the stack. Review P1: 32 levels x per-level cJSON recursion frame
// (~150B) = ~4.8KB, already beyond the 4KB stack margin; 12 levels = ~1.8KB, safe in any parsing context.
// The deepest real protocol payload is ~5 levels (type -> payload -> list -> item -> field), so 12 leaves headroom.
// The pre-check only scans structural characters ({ [ counted and paired), skips strings (incl. escapes), O(len), early exit in microseconds.
#define APP_PROTO_JSON_DEPTH_MAX 12

static bool json_depth_ok(const char *json, size_t len) {
    int depth = 0;
    bool in_str = false;
    // Stop at '\0': consistent with the lenient semantics of cJSON_ParseWithLength (len is only an upper bound,
    // callers may pass a short line with a trailing NUL); a hard scan by len would read out of bounds.
    for (size_t i = 0; i < len && json[i] != '\0'; i++) {
        char c = json[i];
        if (in_str) {
            if (c == '\\' && i + 1 < len) i++;   // skip the escaped character (including \" itself)
            else if (c == '"') in_str = false;
            continue;
        }
        switch (c) {
        case '"': in_str = true; break;
        case '{':
        case '[':
            if (++depth > APP_PROTO_JSON_DEPTH_MAX) return false;
            break;
        case '}':
        case ']':
            if (depth > 0) depth--;
            break;
        default:
            break;
        }
    }
    return true;
}

static void str_take(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

// riskLevel: "low" | "medium" | "high", default medium
static uint8_t parse_risk(const cJSON *o) {
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(o, "riskLevel");
    if (cJSON_IsString(r)) {
        if (strcmp(r->valuestring, "low") == 0)  return APP_RISK_LOW;
        if (strcmp(r->valuestring, "high") == 0) return APP_RISK_HIGH;
    }
    return APP_RISK_MEDIUM;
}

static bool parse_agent_status(const cJSON *o, app_event_t *ev) {
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(o, "state");
    if (!cJSON_IsString(st)) return false;
    uint8_t s;
    if      (strcmp(st->valuestring, "ready") == 0)    s = APP_AGENT_READY;
    else if (strcmp(st->valuestring, "thinking") == 0) s = APP_AGENT_THINKING;
    else if (strcmp(st->valuestring, "running") == 0)  s = APP_AGENT_RUNNING;
    else if (strcmp(st->valuestring, "error") == 0)    s = APP_AGENT_ERROR;
    else if (strcmp(st->valuestring, "done") == 0)     s = APP_AGENT_DONE;
    else return false;
    ev->u.agent_status.state = s;
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(o, "message");
    str_take(ev->u.agent_status.message, sizeof(ev->u.agent_status.message),
             cJSON_IsString(msg) ? msg->valuestring : "");
    ev->type = APP_EV_AGENT_STATUS;
    return true;
}

static bool parse_approval(const cJSON *o, app_event_t *ev) {
    const cJSON *id  = cJSON_GetObjectItemCaseSensitive(o, "taskId");
    const cJSON *ti  = cJSON_GetObjectItemCaseSensitive(o, "title");
    if (!cJSON_IsString(id) || !cJSON_IsString(ti)) return false;
    str_take(ev->u.approval.task_id, sizeof(ev->u.approval.task_id), id->valuestring);
    str_take(ev->u.approval.title, sizeof(ev->u.approval.title), ti->valuestring);
    const cJSON *tg = cJSON_GetObjectItemCaseSensitive(o, "target");
    str_take(ev->u.approval.target, sizeof(ev->u.approval.target),
             cJSON_IsString(tg) ? tg->valuestring : "");
    const cJSON *df = cJSON_GetObjectItemCaseSensitive(o, "diffSummary");
    str_take(ev->u.approval.diff_summary, sizeof(ev->u.approval.diff_summary),
             cJSON_IsString(df) ? df->valuestring : "");
    ev->u.approval.risk = parse_risk(o);
    ev->type = APP_EV_APPROVAL_REQUEST;
    return true;
}

static bool parse_transcript(const cJSON *o, app_event_t *ev) {
    const cJSON *tx = cJSON_GetObjectItemCaseSensitive(o, "text");
    if (!cJSON_IsString(tx)) return false;
    str_take(ev->u.transcript.text, sizeof(ev->u.transcript.text), tx->valuestring);
    const cJSON *im = cJSON_GetObjectItemCaseSensitive(o, "inject_mode");
    ev->u.transcript.inject_mode = (cJSON_IsString(im) && strcmp(im->valuestring, "paste") == 0)
        ? APP_INJECT_PASTE : APP_INJECT_TYPE;
    // final defaults to false: old Mac clients / old frames without this field are always treated as preview state
    const cJSON *fin = cJSON_GetObjectItemCaseSensitive(o, "final");
    ev->u.transcript.final = cJSON_IsBool(fin) && cJSON_IsTrue(fin);
    ev->type = APP_EV_TRANSCRIPT;
    return true;
}

// Time-sync downlink: epoch is UTC seconds. A double carries int64 precision up to 2^53 (~9e15 seconds,
// about 290 million years), the real epoch is ~1.7e9 so precision is lossless; the range check defends against malicious large values
// (cJSON valuedouble can reach 1.8e308, guards against int64 conversion UB).
static bool parse_time_set(const cJSON *o, app_event_t *ev) {
    const cJSON *ep = cJSON_GetObjectItemCaseSensitive(o, "epoch");
    if (!cJSON_IsNumber(ep)) return false;
    double d = ep->valuedouble;
    if (d < -9.2e18 || d > 9.2e18) return false;   // outside int64 range: reject
    ev->u.time_set.epoch = (int64_t)d;
    ev->type = APP_EV_TIME_SET;
    return true;
}

bool app_protocol_parse(const char *json, size_t len, app_event_t *ev) {
    if (!json || len == 0 || len > APP_PROTO_RX_CAP) return false;
    if (!json_depth_ok(json, len)) return false;   // deep nesting: reject, protect the parser's stack
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;
    bool ok = false;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type)) {
        if      (strcmp(type->valuestring, "agent.status") == 0)         ok = parse_agent_status(root, ev);
        else if (strcmp(type->valuestring, "agent.approval_request") == 0) ok = parse_approval(root, ev);
        else if (strcmp(type->valuestring, "transcript") == 0)           ok = parse_transcript(root, ev);
        else if (strcmp(type->valuestring, "time.set") == 0)             ok = parse_time_set(root, ev);
        else if (strcmp(type->valuestring, "pc.status") == 0) {
            const cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audioReady");
            const cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "hotkeyReady");
            if (cJSON_IsBool(audio) && cJSON_IsBool(key)) {
                ev->type = APP_EV_PC_STATUS;
                ev->u.pc_status.audio_ready = cJSON_IsTrue(audio);
                ev->u.pc_status.hotkey_ready = cJSON_IsTrue(key);
                ok = true;
            }
        }
        // unknown type: drop (returns false, the caller logs)
    }
    cJSON_Delete(root);
    return ok;
}

void app_protocol_dispatch_event(const app_event_t *ev)
{
    if (ev->type == APP_EV_APPROVAL_REQUEST) {
        app_event_post_important(ev, 100);   // safety-critical: wait 100ms when full, no drop
    } else {
        app_event_post(ev);                  // regular event: zero-blocking, dropped when full (by design)
    }
}

// ---- Serialization ----
static size_t serialize(cJSON *root, char *buf, size_t cap) {
    if (!root || !buf || cap < 2) return 0;   // with cap<2, (cap-2) underflows into a huge memcpy
    char *s = cJSON_PrintUnformatted(root);
    if (!s) return 0;                         // serialization failed (OOM): do not send a partial frame
    size_t n = strlen(s);
    if (n + 2 > cap) n = cap - 2;             // truncation guard (all frames in this protocol are far smaller than cap)
    memcpy(buf, s, n);
    free(s);
    buf[n++] = '\n';                          // line delimiter
    buf[n] = '\0';
    return n;
}

size_t app_protocol_device_hello(char *buf, size_t cap, int proto) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "device.hello");
    cJSON_AddNumberToObject(o, "proto", proto);
    size_t n = serialize(o, buf, cap);
    cJSON_Delete(o);
    return n;
}

size_t app_protocol_voice_start(char *buf, size_t cap, const char *audio_format) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "voice.start");
    // Audio format (the App dispatches on it): ima_adpcm = each frame is an independent IMA ADPCM block
    // (4B header [int16 predictor][uint8 index][rsv] + 800B of 4-bit data = 804B,
    // 100ms/frame -> 8KB/s), the App decodes back to 16kHz PCM and then uses the existing pcm ASR path;
    // pcm = 3200B PCM block passed through directly (USB bandwidth is ample, no compression).
    // Unknown formats fall back to pcm on the App side; switching codecs (e.g. back to ulaw) needs no protocol change.
    if (!audio_format) audio_format = "pcm";
    cJSON_AddStringToObject(o, "audio", audio_format);
    size_t n = serialize(o, buf, cap);
    cJSON_Delete(o);
    return n;
}

size_t app_protocol_voice_end(char *buf, size_t cap) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "voice.end");
    size_t n = serialize(o, buf, cap);
    cJSON_Delete(o);
    return n;
}

size_t app_protocol_device_status(char *buf, size_t cap, uint32_t drop_count) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "status");
    cJSON_AddNumberToObject(o, "drop", (double)drop_count);
    size_t n = serialize(o, buf, cap);
    cJSON_Delete(o);
    return n;
}

size_t app_protocol_key_action(char *buf, size_t cap, app_key_action_t action) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "key.action");
    cJSON_AddStringToObject(o, "action",
                            action == APP_KEY_CLEAR ? "clear" : "enter");
    size_t n = serialize(o, buf, cap);
    cJSON_Delete(o);
    return n;
}

size_t app_protocol_agent_action(char *buf, size_t cap, const char *task_id,
                                 uint8_t decision) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "agent.action");
    cJSON_AddStringToObject(o, "taskId", task_id ? task_id : "");
    const char *act = "details";
    if (decision == APP_ACTION_APPROVE) act = "approve";
    else if (decision == APP_ACTION_REJECT) act = "reject";
    cJSON_AddStringToObject(o, "action", act);
    size_t n = serialize(o, buf, cap);
    cJSON_Delete(o);
    return n;
}
