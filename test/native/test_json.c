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

void test_json_run(void) {
    test_writer();
    test_reader();
}
