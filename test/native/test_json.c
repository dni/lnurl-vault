#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "json.h"
#include "unity_lite.h"

static void test_writer(void) {
    char buf[256];
    json_writer_t w;
    jw_init(&w, buf, sizeof(buf));
    jw_begin_obj(&w, NULL);
    jw_bool(&w, "ok", true);
    jw_str(&w, "id", "abc123");
    jw_uint64(&w, "amount_msat", 21000);
    jw_begin_arr(&w, "items");
    jw_str_item(&w, "a");
    jw_str_item(&w, "b");
    jw_end_arr(&w);
    jw_end_obj(&w);

    UL_CHECK(jw_ok(&w), "writer stays ok within capacity");
    UL_CHECK(strcmp(jw_cstr(&w),
                     "{\"ok\":true,\"id\":\"abc123\",\"amount_msat\":21000,\"items\":[\"a\",\"b\"]}") ==
                 0,
             "writer produces exact expected compact JSON");

    char tiny[4];
    json_writer_t w2;
    jw_init(&w2, tiny, sizeof(tiny));
    jw_begin_obj(&w2, NULL);
    jw_str(&w2, "id", "abc123");
    jw_end_obj(&w2);
    UL_CHECK(!jw_ok(&w2), "writer detects buffer overflow instead of corrupting memory");
}

static void test_reader(void) {
    const char *json =
        "{\"cmd\":\"confirm\",\"amount_msat\":21000,\"ok\":true,"
        "\"nested\":{\"x\":1},"
        "\"label\":\"a \\\"quoted\\\" x\","
        "\"parent_ids\":[\"aa\",\"bb\"]}";

    char s[32];
    UL_CHECK(json_get_str(json, "cmd", s, sizeof(s)) && strcmp(s, "confirm") == 0,
             "get_str extracts a plain string field");

    uint64_t amt;
    UL_CHECK(json_get_u64(json, "amount_msat", &amt) && amt == 21000,
             "get_u64 extracts an integer field");

    bool b;
    UL_CHECK(json_get_bool(json, "ok", &b) && b == true, "get_bool extracts true");

    UL_CHECK(!json_has(json, "missing_key"), "has() is false for an absent key");
    UL_CHECK(json_has(json, "nested"), "has() finds a nested-object field without descending into it");

    char label[32];
    UL_CHECK(json_get_str(json, "label", label, sizeof(label)), "get_str parses an escaped string");
    UL_CHECK(strcmp(label, "a \"quoted\" x") == 0, "get_str unescapes \\\" correctly");

    char ids[4][9];
    size_t count = 0;
    UL_CHECK(json_get_str_array(json, "parent_ids", &ids[0][0], 9, 4, &count),
             "get_str_array parses an array of strings");
    UL_CHECK(count == 2 && strcmp(ids[0], "aa") == 0 && strcmp(ids[1], "bb") == 0,
             "get_str_array extracts the right values in order");

    UL_CHECK(!json_has(json, ""), "has() is false for an empty key");
}


/* json_get_u64 reads the integer from the raw token rather than through
 * cJSON's double, so amount_msat round trips exactly (see json.c). That is
 * hand-rolled scanning on a money field, so these pin the cases the scan has
 * to get right -- particularly "find the key at the top level and nowhere
 * else", which is the part most likely to be quietly wrong. */
static void test_u64_raw_scan(void) {
    uint64_t v;

    v = 0;
    UL_CHECK(json_get_u64("{\"amount_msat\":18446744073709551615}", "amount_msat", &v) &&
                 v == UINT64_MAX,
             "the largest uint64 round trips exactly");
    v = 0;
    UL_CHECK(json_get_u64("{\"amount_msat\":9007199254740993}", "amount_msat", &v) &&
                 v == 9007199254740993ULL,
             "a value above 2^53 keeps its exact low bits (a double would not)");

    UL_CHECK(!json_get_u64("{\"a\":18446744073709551616}", "a", &v), "2^64 is refused");
    UL_CHECK(!json_get_u64("{\"a\":-1}", "a", &v), "a negative amount is refused");
    UL_CHECK(!json_get_u64("{\"a\":1.5}", "a", &v), "a fractional amount is refused");
    UL_CHECK(!json_get_u64("{\"a\":1e3}", "a", &v), "exponent notation is refused");
    UL_CHECK(!json_get_u64("{\"a\":\"21000\"}", "a", &v), "a quoted number is not a number");
    UL_CHECK(!json_get_u64("{\"b\":1}", "a", &v), "an absent key fails");

    v = 0;
    UL_CHECK(json_get_u64("{\"a\":0}", "a", &v) && v == 0, "zero parses");
    v = 0;
    UL_CHECK(json_get_u64("{\"a\":007}", "a", &v) && v == 7, "leading zeros parse");
    v = 0;
    UL_CHECK(json_get_u64("{ \"a\" :\t21000 }", "a", &v) && v == 21000,
             "whitespace around the key and colon is tolerated");

    /* The scan must not match a key nested inside another object, or one that
     * merely appears inside a string value. Either would read an attacker's
     * number in place of the real field. */
    v = 0;
    UL_CHECK(json_get_u64("{\"outer\":{\"a\":1},\"a\":42}", "a", &v) && v == 42,
             "a nested same-named key is skipped in favour of the top-level one");
    v = 0;
    UL_CHECK(json_get_u64("{\"label\":\"\\\"a\\\":1\",\"a\":42}", "a", &v) && v == 42,
             "a key-looking sequence inside a string value is not matched");
    UL_CHECK(!json_get_u64("{\"outer\":{\"a\":1}}", "a", &v),
             "a key that exists only inside a nested object is not found");
}

/* A pathologically nested payload must be refused by the depth guard before
 * cJSON recurses, not parsed. On the firmware this is reachable pre-auth from
 * any BLE central, against a 4KB transport-task stack -- a few hundred brackets
 * is enough to overflow it. See json.c's depth_within_limit(). */
static void test_nesting_depth(void) {
    char deep[600];
    size_t n = 0;
    for (; n < 300; n++) deep[n] = '[';
    deep[n] = '\0';
    uint64_t v;
    char s[16];
    UL_CHECK(!json_has(deep, "cmd"), "300 levels of nesting is refused by json_has, not parsed");
    UL_CHECK(!json_get_str(deep, "cmd", s, sizeof(s)), "and by json_get_str");
    UL_CHECK(!json_get_u64(deep, "a", &v), "and by json_get_u64");

    /* A deep object wrapping a real key is refused wholesale, not mined for
     * the key inside it. */
    char nested[256];
    n = 0;
    for (size_t d = 0; d < 40; d++) nested[n++] = '{';
    const char *inner = "\"cmd\":\"get_info\"";
    memcpy(nested + n, inner, strlen(inner));
    n += strlen(inner);
    for (size_t d = 0; d < 40; d++) nested[n++] = '}';
    nested[n] = '\0';
    UL_CHECK(!json_has(nested, "cmd"), "a 40-deep wrapper is refused, its inner key not salvaged");

    /* The legitimate maximum -- a flat object carrying a parent_ids array,
     * depth 2 -- still parses, so the guard has real headroom. */
    const char *legit = "{\"cmd\":\"new_secret\",\"parent_ids\":[\"aa\",\"bb\"]}";
    char cmd[16];
    UL_CHECK(json_get_str(legit, "cmd", cmd, sizeof(cmd)) && strcmp(cmd, "new_secret") == 0,
             "a normal flat command with a parent_ids array still parses");
    char ids[4][9];
    size_t count = 0;
    UL_CHECK(json_get_str_array(legit, "parent_ids", &ids[0][0], 9, 4, &count) && count == 2,
             "and its depth-2 array is read normally");
}

void test_json_run(void) {
    test_u64_raw_scan();
    test_writer();
    test_reader();
    test_nesting_depth();
}
