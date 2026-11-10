// Clausium feed v1 and alert parsers against the wire contract.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apps/clausium/clausium_feed.h"

static clausium_feed_result_t parse(const char *json, clausium_state_t *st) {
    return clausium_feed_parse(json, strlen(json), st);
}
static clausium_feed_result_t parse_alert(const char *json, clausium_alert_msg_t *m) {
    return clausium_alert_parse(json, strlen(json), m);
}

static const char *kSample =
    "{\"v\":1,\"t\":1762770000,\n \"n\":{\"wait\":1,\"work\":2,\"live\":3},\n"
    " \"need\":[{\"p\":\"passport-os\",\"f\":\"battut\",\"k\":\"permission\",\"a\":\"Bash: run tests\"}],\n"
    " \"live\":[{\"p\":\"claudedeck\",\"f\":\"personal\",\"s\":\"working\",\"l\":\"edit Station.swift\"}],\n"
    " \"plan\":[{\"f\":\"battut\",\"h5\":42,\"wk\":67,\"h5r\":1762780000,\"wkr\":1763200000}]}";

// Replace the first occurrence of `from` in the sample with `to`.
static char *variant(const char *from, const char *to) {
    static char buf[4096];
    const char *at = strstr(kSample, from);
    assert(at);
    snprintf(buf, sizeof(buf), "%.*s%s%s", (int)(at - kSample), kSample, to, at + strlen(from));
    return buf;
}

static void expect(const char *json, clausium_feed_result_t want) {
    clausium_state_t st;
    memset(&st, 0x5a, sizeof(st));
    const clausium_feed_result_t got = parse(json, &st);
    if (got != want) { fprintf(stderr, "want %d got %d for %s\n", want, got, json); assert(0); }
    if (want != CLAUSIUM_FEED_OK) {
        static const clausium_state_t zero;
        assert(!memcmp(&st, &zero, sizeof(st))); // failures never leave partial data
    }
}

int main(void) {
    clausium_state_t st;
    // Valid sample from the contract.
    assert(parse(kSample, &st) == CLAUSIUM_FEED_OK);
    assert(st.built_at == 1762770000u && st.wait == 1 && st.work == 2 && st.live_total == 3);
    assert(st.need_count == 1 && st.live_count == 1 && st.plan_count == 1);
    assert(!strcmp(st.need[0].project, "passport-os") && !strcmp(st.need[0].profile, "battut"));
    assert(st.need[0].kind == CLAUSIUM_NEED_PERMISSION && !strcmp(st.need[0].ask, "Bash: run tests"));
    assert(!strcmp(st.live[0].label, "edit Station.swift") && st.live[0].status == CLAUSIUM_LIVE_WORKING);
    assert(st.plan[0].h5_used == 42 && st.plan[0].week_used == 67);
    assert(st.plan[0].h5_reset == 1762780000u && st.plan[0].week_reset == 1763200000u);

    // Every enum value; unknown (-1 / 0) fuel; CJK project; empty arrays.
    assert(parse("{\"v\":1,\"t\":0,\"n\":{\"wait\":0,\"work\":0,\"live\":0},\"need\":["
        "{\"p\":\"a\",\"f\":\"x\",\"k\":\"question\"},{\"p\":\"b\",\"f\":\"x\",\"k\":\"done\"},"
        "{\"p\":\"\xe6\x97\xa5\xe6\x9c\xac\",\"f\":\"x\",\"k\":\"error\"}],\"live\":["
        "{\"p\":\"a\",\"f\":\"\",\"s\":\"idle\",\"l\":\"\"},{\"p\":\"a\",\"f\":\"\",\"s\":\"needs\",\"l\":\"waiting\"},"
        "{\"p\":\"a\",\"f\":\"\",\"s\":\"done\",\"l\":\"done\"},{\"p\":\"a\",\"f\":\"\",\"s\":\"error\",\"l\":\"x\"}],"
        "\"plan\":[{\"f\":\"p\",\"h5\":-1,\"wk\":100,\"h5r\":0,\"wkr\":0}]}", &st) == CLAUSIUM_FEED_OK);
    assert(st.need[0].kind == CLAUSIUM_NEED_QUESTION && st.need[1].kind == CLAUSIUM_NEED_DONE &&
           st.need[2].kind == CLAUSIUM_NEED_ERROR && !strcmp(st.need[2].project, "\xe6\x97\xa5\xe6\x9c\xac"));
    assert(st.live[0].status == CLAUSIUM_LIVE_IDLE && st.live[1].status == CLAUSIUM_LIVE_NEEDS &&
           st.live[2].status == CLAUSIUM_LIVE_DONE && st.live[3].status == CLAUSIUM_LIVE_ERROR);
    assert(st.plan[0].h5_used == -1 && st.plan[0].week_used == 100);

    // Missing optional `a`, null `a`, missing arrays, unknown keys at every level.
    expect(variant(",\"a\":\"Bash: run tests\"", ""), CLAUSIUM_FEED_OK);
    assert(parse(variant(",\"a\":\"Bash: run tests\"", ",\"a\":null"), &st) == CLAUSIUM_FEED_OK && !st.need[0].ask[0]);
    expect("{\"v\":1,\"t\":5,\"n\":{\"wait\":0,\"work\":0,\"live\":0}}", CLAUSIUM_FEED_OK);
    expect(variant("\"v\":1,", "\"v\":1,\"future\":{\"x\":[1,2]},\"z\":\"q\","), CLAUSIUM_FEED_OK);
    expect(variant("\"k\":\"permission\"", "\"k\":\"permission\",\"extra\":7"), CLAUSIUM_FEED_OK);
    expect(variant("\"wait\":1", "\"wait\":1,\"idle\":4"), CLAUSIUM_FEED_OK);

    // Version.
    expect(variant("\"v\":1", "\"v\":2"), CLAUSIUM_FEED_VERSION);
    expect(variant("\"v\":1", "\"v\":\"1\""), CLAUSIUM_FEED_VERSION);
    expect(variant("\"v\":1,", ""), CLAUSIUM_FEED_VERSION);
    expect(variant("\"v\":1", "\"v\":1.5"), CLAUSIUM_FEED_VERSION);

    // Wrong types and ranges.
    expect(variant("\"t\":1762770000", "\"t\":\"now\""), CLAUSIUM_FEED_INVALID);
    expect(variant("\"t\":1762770000", "\"t\":-1"), CLAUSIUM_FEED_INVALID);
    expect(variant("\"n\":{\"wait\":1,\"work\":2,\"live\":3}", "\"n\":[1,2,3]"), CLAUSIUM_FEED_INVALID);
    expect(variant("\"wait\":1", "\"wait\":-1"), CLAUSIUM_FEED_INVALID);
    expect(variant("\"wait\":1", "\"wait\":70000"), CLAUSIUM_FEED_INVALID);
    expect(variant("\"wait\":1,", ""), CLAUSIUM_FEED_INVALID);
    expect(variant("\"need\":[", "\"need\":{\"x\":["), CLAUSIUM_FEED_SYNTAX); // unbalanced after edit
    expect(variant("\"need\":[{\"p\":\"passport-os\",\"f\":\"battut\",\"k\":\"permission\",\"a\":\"Bash: run tests\"}]",
                   "\"need\":{}"), CLAUSIUM_FEED_INVALID);
    expect(variant("\"need\":[{", "\"need\":[7,{"), CLAUSIUM_FEED_INVALID);
    expect(variant("\"k\":\"permission\"", "\"k\":\"approve\""), CLAUSIUM_FEED_INVALID);
    expect(variant("\"k\":\"permission\"", "\"k\":1"), CLAUSIUM_FEED_INVALID);
    expect(variant("\"s\":\"working\"", "\"s\":\"busy\""), CLAUSIUM_FEED_INVALID);
    expect(variant("\"l\":\"edit Station.swift\"", "\"l\":null"), CLAUSIUM_FEED_INVALID);
    expect(variant("\"p\":\"claudedeck\",", ""), CLAUSIUM_FEED_INVALID);
    expect(variant("\"h5\":42", "\"h5\":101"), CLAUSIUM_FEED_INVALID);
    expect(variant("\"h5\":42", "\"h5\":-2"), CLAUSIUM_FEED_INVALID);
    expect(variant("\"wk\":67", "\"wk\":66.5"), CLAUSIUM_FEED_INVALID);
    expect(variant("\"h5r\":1762780000", "\"h5r\":-5"), CLAUSIUM_FEED_INVALID);
    expect(variant("\"wkr\":1763200000", "\"wkr\":4294967296"), CLAUSIUM_FEED_INVALID);
    expect(variant("\"f\":\"battut\",\"h5\"", "\"f\":true,\"h5\""), CLAUSIUM_FEED_INVALID);

    // Oversize and non-ASCII strings.
    expect(variant("\"p\":\"passport-os\"", "\"p\":\"1234567890123456789012345\""), CLAUSIUM_FEED_INVALID);
    expect(variant("\"p\":\"passport-os\"", "\"p\":\"123456789012345678901234\""), CLAUSIUM_FEED_OK);
    expect(variant("\"f\":\"battut\"", "\"f\":\"12345678901234567\""), CLAUSIUM_FEED_INVALID);
    expect(variant("\"f\":\"battut\"", "\"f\":\"1234567890123456\""), CLAUSIUM_FEED_OK);
    expect(variant("\"l\":\"edit Station.swift\"", "\"l\":\"123456789012345678901234567890123\""), CLAUSIUM_FEED_INVALID);
    expect(variant("\"l\":\"edit Station.swift\"", "\"l\":\"12345678901234567890123456789012\""), CLAUSIUM_FEED_OK);
    expect(variant("\"a\":\"Bash: run tests\"",
        "\"a\":\"1234567890123456789012345678901234567890123456789012345678901\""), CLAUSIUM_FEED_INVALID);
    expect(variant("\"a\":\"Bash: run tests\"",
        "\"a\":\"123456789012345678901234567890123456789012345678901234567890\""), CLAUSIUM_FEED_OK);
    expect(variant("\"l\":\"edit Station.swift\"", "\"l\":\"edit \xe2\x9c\x8f\""), CLAUSIUM_FEED_INVALID);
    expect(variant("\"a\":\"Bash: run tests\"", "\"a\":\"line\\nbreak\""), CLAUSIUM_FEED_INVALID);
    expect(variant("\"p\":\"passport-os\"", "\"p\":\"bad\xff\""), CLAUSIUM_FEED_INVALID);
    expect(variant("\"p\":\"passport-os\"", "\"p\":\"\xe6\x97\""), CLAUSIUM_FEED_INVALID); // truncated UTF-8

    // Caps: extra items are ignored, the first ones kept.
    char big[3072];
    int n = snprintf(big, sizeof(big), "{\"v\":1,\"t\":1,\"n\":{\"wait\":9,\"work\":0,\"live\":10},\"need\":[");
    for (int i = 0; i < 9; ++i)
        n += snprintf(big + n, sizeof(big) - n, "%s{\"p\":\"n%d\",\"f\":\"f\",\"k\":\"done\"}", i ? "," : "", i);
    n += snprintf(big + n, sizeof(big) - n, "],\"live\":[");
    for (int i = 0; i < 10; ++i)
        n += snprintf(big + n, sizeof(big) - n, "%s{\"p\":\"l%d\",\"f\":\"f\",\"s\":\"idle\",\"l\":\"x\"}", i ? "," : "", i);
    n += snprintf(big + n, sizeof(big) - n, "],\"plan\":[");
    for (int i = 0; i < 6; ++i)
        n += snprintf(big + n, sizeof(big) - n, "%s{\"f\":\"p%d\",\"h5\":1,\"wk\":2,\"h5r\":0,\"wkr\":0}", i ? "," : "", i);
    snprintf(big + n, sizeof(big) - n, "]}");
    assert(parse(big, &st) == CLAUSIUM_FEED_OK);
    assert(st.need_count == CLAUSIUM_NEED_MAX && st.live_count == CLAUSIUM_LIVE_MAX && st.plan_count == CLAUSIUM_PLAN_MAX);
    assert(!strcmp(st.need[5].project, "n5") && !strcmp(st.live[7].project, "l7") && !strcmp(st.plan[3].profile, "p3"));
    assert(st.wait == 9 && st.live_total == 10);

    // Size limit: exactly 3072 accepted, 3073 rejected before parsing.
    char *pad = malloc(CLAUSIUM_FEED_MAX_BYTES + 2);
    const char *head = "{\"v\":1,\"t\":1,\"n\":{\"wait\":0,\"work\":0,\"live\":0},\"pad\":\"";
    const size_t head_len = strlen(head);
    memcpy(pad, head, head_len);
    memset(pad + head_len, 'x', CLAUSIUM_FEED_MAX_BYTES - head_len - 2);
    memcpy(pad + CLAUSIUM_FEED_MAX_BYTES - 2, "\"}", 3);
    assert(strlen(pad) == CLAUSIUM_FEED_MAX_BYTES);
    assert(clausium_feed_parse(pad, CLAUSIUM_FEED_MAX_BYTES, &st) == CLAUSIUM_FEED_OK);
    memmove(pad + 1, pad, CLAUSIUM_FEED_MAX_BYTES + 1);
    pad[0] = ' ';
    assert(clausium_feed_parse(pad, CLAUSIUM_FEED_MAX_BYTES + 1, &st) == CLAUSIUM_FEED_TOO_LARGE);
    free(pad);

    // Syntax: garbage, trailing data, truncated body, embedded NUL, non-object.
    expect("", CLAUSIUM_FEED_SYNTAX);
    expect("not json", CLAUSIUM_FEED_SYNTAX);
    expect("[1,2]", CLAUSIUM_FEED_SYNTAX);
    expect("{\"v\":1,\"t\":1,\"n\":{\"wait\":0,\"work\":0,\"live\":0}} trailing", CLAUSIUM_FEED_SYNTAX);
    expect("{\"v\":1,\"t\":1,\"n\":{\"wait\":0", CLAUSIUM_FEED_SYNTAX);
    assert(clausium_feed_parse("{}\0{}", 5, &st) == CLAUSIUM_FEED_SYNTAX);
    assert(clausium_feed_parse(NULL, 0, &st) == CLAUSIUM_FEED_SYNTAX);

    // Alert endpoint.
    clausium_alert_msg_t m;
    assert(parse_alert("{\"v\":1,\"boot\":\"a1b2c3\",\"seq\":17,\"wait\":2,\"e\":[{\"q\":17,\"p\":\"passport-os\","
                       "\"f\":\"battut\",\"k\":\"permission\"}]}", &m) == CLAUSIUM_FEED_OK);
    assert(!strcmp(m.boot, "a1b2c3") && m.seq == 17 && m.wait == 2 && m.event_count == 1);
    assert(m.event[0].seq == 17 && m.event[0].kind == CLAUSIUM_NEED_PERMISSION && !strcmp(m.event[0].project, "passport-os"));
    assert(parse_alert("{\"v\":1,\"boot\":\"000000\",\"seq\":0,\"wait\":0,\"e\":[],\"new\":1}", &m) == CLAUSIUM_FEED_OK);
    assert(parse_alert("{\"v\":1,\"boot\":\"000000\",\"seq\":0,\"wait\":0}", &m) == CLAUSIUM_FEED_OK && !m.event_count);
    assert(parse_alert("{\"v\":2,\"boot\":\"a1b2c3\",\"seq\":1,\"wait\":0,\"e\":[]}", &m) == CLAUSIUM_FEED_VERSION);
    assert(parse_alert("{\"v\":1,\"boot\":\"A1B2C3\",\"seq\":1,\"wait\":0,\"e\":[]}", &m) == CLAUSIUM_FEED_INVALID);
    assert(parse_alert("{\"v\":1,\"boot\":\"a1b2c\",\"seq\":1,\"wait\":0,\"e\":[]}", &m) == CLAUSIUM_FEED_INVALID);
    assert(parse_alert("{\"v\":1,\"boot\":\"a1b2c3\",\"seq\":-1,\"wait\":0,\"e\":[]}", &m) == CLAUSIUM_FEED_INVALID);
    assert(parse_alert("{\"v\":1,\"boot\":\"a1b2c3\",\"seq\":3,\"wait\":0,\"e\":[{\"q\":4,\"p\":\"a\",\"f\":\"b\","
                       "\"k\":\"done\"}]}", &m) == CLAUSIUM_FEED_INVALID); // event newer than seq
    assert(parse_alert("{\"v\":1,\"boot\":\"a1b2c3\",\"seq\":3,\"wait\":0,\"e\":[{\"q\":3,\"p\":\"a\",\"f\":\"b\","
                       "\"k\":\"stuck\"}]}", &m) == CLAUSIUM_FEED_INVALID);
    assert(parse_alert("{\"v\":1,\"boot\":\"a1b2c3\",\"seq\":9,\"wait\":0,\"e\":["
        "{\"q\":5,\"p\":\"a\",\"f\":\"b\",\"k\":\"done\"},{\"q\":6,\"p\":\"a\",\"f\":\"b\",\"k\":\"error\"},"
        "{\"q\":7,\"p\":\"a\",\"f\":\"b\",\"k\":\"question\"},{\"q\":8,\"p\":\"a\",\"f\":\"b\",\"k\":\"done\"}]}", &m)
        == CLAUSIUM_FEED_OK && m.event_count == CLAUSIUM_ALERT_EVENTS_MAX);
    char alert_big[CLAUSIUM_ALERT_MAX_BYTES + 2];
    memset(alert_big, ' ', sizeof(alert_big));
    alert_big[sizeof(alert_big) - 1] = 0;
    assert(clausium_alert_parse(alert_big, CLAUSIUM_ALERT_MAX_BYTES + 1, &m) == CLAUSIUM_FEED_TOO_LARGE);

    puts("Clausium feed: contract sample, enums, optional ask, unknown keys, version, types/ranges, "
         "string limits/ASCII/UTF-8, caps, 3072-byte limit, syntax, alert endpoint: PASS");
    return 0;
}
