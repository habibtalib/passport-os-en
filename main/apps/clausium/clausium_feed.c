#include "clausium_feed.h"

#include <cJSON.h>
#include <math.h>
#include <string.h>

// Strict v1 reader: every kept field must match the contract or the whole feed
// is rejected, so the screen never shows a half-understood snapshot.

static bool get_int(const cJSON *item, double min, double max, double *out) {
    if (!cJSON_IsNumber(item)) return false;
    const double v = item->valuedouble;
    if (!isfinite(v) || v != floor(v) || v < min || v > max) return false;
    *out = v;
    return true;
}

// Printable ASCII only: the device fonts and the contract both expect it.
static bool ascii_ok(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if (*p < 0x20 || *p > 0x7e) return false;
    return true;
}

// Well-formed UTF-8 without control characters (project names may be CJK).
static bool utf8_ok(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        const unsigned char c = *p;
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) { ++p; continue; }
        int extra;
        unsigned cp;
        if (c >= 0xc2 && c <= 0xdf) { extra = 1; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { extra = 2; cp = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { extra = 3; cp = c & 0x07; }
        else return false;
        ++p;
        for (int i = 0; i < extra; ++i, ++p) {
            if ((*p & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (*p & 0x3f);
        }
        if ((extra == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) ||
            (extra == 3 && (cp < 0x10000 || cp > 0x10ffff))) return false;
    }
    return true;
}

static bool copy_str(const cJSON *obj, const char *key, char *dst, size_t max, bool ascii) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    const size_t n = strlen(item->valuestring);
    if (n > max) return false;
    if (ascii ? !ascii_ok(item->valuestring) : !utf8_ok(item->valuestring)) return false;
    memcpy(dst, item->valuestring, n + 1);
    return true;
}

static int lookup(const char *value, const char *const *names, int count) {
    for (int i = 0; i < count; ++i)
        if (!strcmp(value, names[i])) return i;
    return -1;
}

static bool enum_field(const cJSON *obj, const char *key, const char *const *names, int count,
                       uint8_t *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    const int index = lookup(item->valuestring, names, count);
    if (index < 0) return false;
    *out = (uint8_t)index;
    return true;
}

// Missing arrays mean "none"; a present `need`/`live`/`plan` must be an array.
static const cJSON *array_field(const cJSON *root, const char *key, bool *ok) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item || cJSON_IsNull(item)) return NULL;
    if (!cJSON_IsArray(item)) *ok = false;
    return cJSON_IsArray(item) ? item : NULL;
}

static const char *const kNeedKinds[] = {"permission", "question", "done", "error"};
static const char *const kLiveStates[] = {"working", "idle", "needs", "done", "error"};

static bool parse_need(const cJSON *item, clausium_need_t *need) {
    if (!cJSON_IsObject(item)) return false;
    if (!copy_str(item, "p", need->project, CLAUSIUM_PROJECT_MAX, false) ||
        !copy_str(item, "f", need->profile, CLAUSIUM_PROFILE_MAX, true) ||
        !enum_field(item, "k", kNeedKinds, 4, &need->kind)) return false;
    const cJSON *ask = cJSON_GetObjectItemCaseSensitive(item, "a");
    if (!ask || cJSON_IsNull(ask)) { need->ask[0] = 0; return true; }
    return copy_str(item, "a", need->ask, CLAUSIUM_ASK_MAX, true);
}

static bool parse_live(const cJSON *item, clausium_live_t *live) {
    return cJSON_IsObject(item) &&
        copy_str(item, "p", live->project, CLAUSIUM_PROJECT_MAX, false) &&
        copy_str(item, "f", live->profile, CLAUSIUM_PROFILE_MAX, true) &&
        enum_field(item, "s", kLiveStates, 5, &live->status) &&
        copy_str(item, "l", live->label, CLAUSIUM_LABEL_MAX, true);
}

static bool parse_plan(const cJSON *item, clausium_plan_t *plan) {
    double h5, wk, h5r, wkr;
    if (!cJSON_IsObject(item) || !copy_str(item, "f", plan->profile, CLAUSIUM_PROFILE_MAX, true) ||
        !get_int(cJSON_GetObjectItemCaseSensitive(item, "h5"), -1, 100, &h5) ||
        !get_int(cJSON_GetObjectItemCaseSensitive(item, "wk"), -1, 100, &wk) ||
        !get_int(cJSON_GetObjectItemCaseSensitive(item, "h5r"), 0, UINT32_MAX, &h5r) ||
        !get_int(cJSON_GetObjectItemCaseSensitive(item, "wkr"), 0, UINT32_MAX, &wkr)) return false;
    plan->h5_used = (int8_t)h5;
    plan->week_used = (int8_t)wk;
    plan->h5_reset = (uint32_t)h5r;
    plan->week_reset = (uint32_t)wkr;
    return true;
}

static clausium_feed_result_t parse_root(const cJSON *root, clausium_state_t *out) {
    if (!cJSON_IsObject(root)) return CLAUSIUM_FEED_SYNTAX;
    double v, t, wait, work, live;
    if (!get_int(cJSON_GetObjectItemCaseSensitive(root, "v"), 1, 1, &v)) return CLAUSIUM_FEED_VERSION;
    if (!get_int(cJSON_GetObjectItemCaseSensitive(root, "t"), 0, UINT32_MAX, &t))
        return CLAUSIUM_FEED_INVALID;
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "n");
    if (!cJSON_IsObject(n) ||
        !get_int(cJSON_GetObjectItemCaseSensitive(n, "wait"), 0, UINT16_MAX, &wait) ||
        !get_int(cJSON_GetObjectItemCaseSensitive(n, "work"), 0, UINT16_MAX, &work) ||
        !get_int(cJSON_GetObjectItemCaseSensitive(n, "live"), 0, UINT16_MAX, &live))
        return CLAUSIUM_FEED_INVALID;
    out->built_at = (uint32_t)t;
    out->wait = (uint16_t)wait;
    out->work = (uint16_t)work;
    out->live_total = (uint16_t)live;

    bool ok = true;
    const cJSON *needs = array_field(root, "need", &ok);
    const cJSON *lives = array_field(root, "live", &ok);
    const cJSON *plans = array_field(root, "plan", &ok);
    if (!ok) return CLAUSIUM_FEED_INVALID;
    const cJSON *item;
    cJSON_ArrayForEach(item, needs) {
        if (out->need_count >= CLAUSIUM_NEED_MAX) break;
        if (!parse_need(item, &out->need[out->need_count])) return CLAUSIUM_FEED_INVALID;
        ++out->need_count;
    }
    cJSON_ArrayForEach(item, lives) {
        if (out->live_count >= CLAUSIUM_LIVE_MAX) break;
        if (!parse_live(item, &out->live[out->live_count])) return CLAUSIUM_FEED_INVALID;
        ++out->live_count;
    }
    cJSON_ArrayForEach(item, plans) {
        if (out->plan_count >= CLAUSIUM_PLAN_MAX) break;
        if (!parse_plan(item, &out->plan[out->plan_count])) return CLAUSIUM_FEED_INVALID;
        ++out->plan_count;
    }
    return CLAUSIUM_FEED_OK;
}

clausium_feed_result_t clausium_feed_parse(const char *body, size_t len, clausium_state_t *out) {
    memset(out, 0, sizeof(*out));
    if (!body) return CLAUSIUM_FEED_SYNTAX;
    if (len > CLAUSIUM_FEED_MAX_BYTES) return CLAUSIUM_FEED_TOO_LARGE;
    if (strlen(body) != len) return CLAUSIUM_FEED_SYNTAX; // embedded NUL or missing terminator
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts(body, &end, 1);
    if (!root) return CLAUSIUM_FEED_SYNTAX; // also covers cJSON allocation failure
    const clausium_feed_result_t result = parse_root(root, out);
    cJSON_Delete(root);
    if (result != CLAUSIUM_FEED_OK) memset(out, 0, sizeof(*out));
    return result;
}

static bool parse_event(const cJSON *item, uint32_t seq, clausium_event_t *event) {
    double q;
    if (!cJSON_IsObject(item) ||
        !get_int(cJSON_GetObjectItemCaseSensitive(item, "q"), 1, seq, &q) ||
        !copy_str(item, "p", event->project, CLAUSIUM_PROJECT_MAX, false) ||
        !copy_str(item, "f", event->profile, CLAUSIUM_PROFILE_MAX, true) ||
        !enum_field(item, "k", kNeedKinds, 4, &event->kind)) return false;
    event->seq = (uint32_t)q;
    return true;
}

static clausium_feed_result_t parse_alert_root(const cJSON *root, clausium_alert_msg_t *out) {
    if (!cJSON_IsObject(root)) return CLAUSIUM_FEED_SYNTAX;
    double v, seq, wait;
    if (!get_int(cJSON_GetObjectItemCaseSensitive(root, "v"), 1, 1, &v)) return CLAUSIUM_FEED_VERSION;
    const cJSON *boot = cJSON_GetObjectItemCaseSensitive(root, "boot");
    if (!cJSON_IsString(boot) || !boot->valuestring || strlen(boot->valuestring) != CLAUSIUM_BOOT_LEN)
        return CLAUSIUM_FEED_INVALID;
    for (int i = 0; i < CLAUSIUM_BOOT_LEN; ++i) {
        const char c = boot->valuestring[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return CLAUSIUM_FEED_INVALID;
    }
    if (!get_int(cJSON_GetObjectItemCaseSensitive(root, "seq"), 0, UINT32_MAX, &seq) ||
        !get_int(cJSON_GetObjectItemCaseSensitive(root, "wait"), 0, UINT16_MAX, &wait))
        return CLAUSIUM_FEED_INVALID;
    memcpy(out->boot, boot->valuestring, CLAUSIUM_BOOT_LEN + 1);
    out->seq = (uint32_t)seq;
    out->wait = (uint16_t)wait;
    bool ok = true;
    const cJSON *events = array_field(root, "e", &ok);
    if (!ok) return CLAUSIUM_FEED_INVALID;
    const cJSON *item;
    cJSON_ArrayForEach(item, events) {
        if (out->event_count >= CLAUSIUM_ALERT_EVENTS_MAX) break;
        if (!parse_event(item, out->seq, &out->event[out->event_count])) return CLAUSIUM_FEED_INVALID;
        ++out->event_count;
    }
    return CLAUSIUM_FEED_OK;
}

clausium_feed_result_t clausium_alert_parse(const char *body, size_t len, clausium_alert_msg_t *out) {
    memset(out, 0, sizeof(*out));
    if (!body) return CLAUSIUM_FEED_SYNTAX;
    if (len > CLAUSIUM_ALERT_MAX_BYTES) return CLAUSIUM_FEED_TOO_LARGE;
    if (strlen(body) != len) return CLAUSIUM_FEED_SYNTAX;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts(body, &end, 1);
    if (!root) return CLAUSIUM_FEED_SYNTAX;
    const clausium_feed_result_t result = parse_alert_root(root, out);
    cJSON_Delete(root);
    if (result != CLAUSIUM_FEED_OK) memset(out, 0, sizeof(*out));
    return result;
}
