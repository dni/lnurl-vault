/* list_notes, which is the only unbounded response in the protocol and the
 * one that used to break silently.
 *
 * Issue #7 measured the old behaviour: output stopped being parseable at 29
 * notes, or 15 once each carried a signature, against a declared
 * VAULT_MAX_NOTES of 128 -- and the client got a string that just stopped,
 * with no error field. A vault could hold four times more notes than it could
 * ever list and said nothing about it.
 *
 * The first test here is the one that matters most: fill a vault past the
 * point where a full listing cannot fit, and assert that whatever comes back
 * is parseable and tells the truth about how much it left out. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dispatcher.h"
#include "json.h"
#include "unity_lite.h"
#include "vault.h"

#define RESP 4096

static uint32_t g_seq;
static bool rng_seq(uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        g_seq = g_seq * 1103515245u + 12345u;
        out[i] = (uint8_t)(g_seq >> 16);
    }
    return true;
}

/* Structurally complete JSON: every bracket closed in order, no unterminated
 * string. The old failure was a response that looked fine until you parsed it,
 * so this is the check that actually catches it. */
static bool well_formed(const char *s) {
    char stack[64];
    int depth = 0;
    bool in_str = false, escaped = false;
    for (const char *p = s; *p; p++) {
        if (in_str) {
            if (escaped) {
                escaped = false;
            } else if (*p == '\\') {
                escaped = true;
            } else if (*p == '"') {
                in_str = false;
            }
            continue;
        }
        if (*p == '"') {
            in_str = true;
        } else if (*p == '{' || *p == '[') {
            if (depth >= (int)sizeof(stack)) {
                return false;
            }
            stack[depth++] = *p;
        } else if (*p == '}' || *p == ']') {
            if (depth == 0) {
                return false;
            }
            char open = stack[--depth];
            if ((*p == '}' && open != '{') || (*p == ']' && open != '[')) {
                return false;
            }
        }
    }
    return depth == 0 && !in_str;
}

/* Counts top-level objects inside the "notes" array by matching on a field
 * every note object carries. */
static int count_notes(const char *json) {
    int n = 0;
    for (const char *p = json; (p = strstr(p, "\"state\":")) != NULL; p++) {
        n++;
    }
    return n;
}

static uint64_t number_field(const char *json, const char *key) {
    uint64_t v = 0;
    return json_get_u64(json, key, &v) ? v : UINT64_MAX;
}

static bool has_field(const char *json, const char *key) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    return strstr(json, pat) != NULL;
}

/* Fills a vault with `n` CONFIRMED notes, each carrying a full-length
 * signature -- the worst case for response size, and the configuration issue
 * #7 measured at 15 notes. */
static void fill(size_t n, bool with_sig) {
    dispatcher_deps_t deps = {.rng = rng_seq};
    dispatcher_init(&deps);
    vault_init(NULL, NULL);
    g_seq = 7;

    char sig[VAULT_SIG_BUF];
    memset(sig, 'a', sizeof(sig) - 1);
    sig[sizeof(sig) - 1] = '\0';

    for (size_t i = 0; i < n; i++) {
        char id[VAULT_ID_BUF], h[VAULT_HASH_HEX_BUF];
        char label[VAULT_LABEL_BUF];
        snprintf(label, sizeof(label), "note-%zu", i);
        if (vault_new_secret(rng_seq, NULL, 0, label, id, h) != VAULT_OK) {
            break;
        }
        vault_confirm(id, 1000ull * (i + 1), "mint.example.com", with_sig ? sig : NULL);
    }
}

/* THE regression. A vault holding more notes than fit in one response must
 * still answer something parseable, and must say how many it holds. */
static void test_a_full_vault_still_answers(void) {
    fill(VAULT_MAX_NOTES, true);
    char out[RESP];
    dispatcher_handle("{\"cmd\":\"list_notes\"}", out, sizeof(out));

    UL_CHECK(well_formed(out), "a listing that cannot fit is still well-formed JSON");
    UL_CHECK(strstr(out, "\"ok\":true") != NULL, "and is a success, not a failure");
    UL_CHECK(number_field(out, "total") == VAULT_MAX_NOTES,
             "and reports the true total, not just what it returned");

    int returned = count_notes(out);
    UL_CHECK(returned > 0, "at least one note came back");
    UL_CHECK(returned < VAULT_MAX_NOTES, "and not all of them, since they do not fit");
    UL_CHECK(has_field(out, "next_offset"), "so it says where to continue from");
    UL_CHECK(number_field(out, "next_offset") == (uint64_t)returned,
             "and next_offset is exactly where this page stopped");
}

/* A small vault must be unaffected: no paging, no next_offset, everything in
 * one response, exactly as before. */
static void test_a_small_vault_is_unchanged(void) {
    fill(3, true);
    char out[RESP];
    dispatcher_handle("{\"cmd\":\"list_notes\"}", out, sizeof(out));

    UL_CHECK(well_formed(out), "well-formed");
    UL_CHECK(count_notes(out) == 3, "all three notes returned");
    UL_CHECK(number_field(out, "total") == 3, "total is 3");
    UL_CHECK(!has_field(out, "next_offset"), "and no next_offset, because there is no more");
}

/* Paging all the way through must visit every note exactly once and terminate.
 * A paging scheme that loops, or skips, is worse than no paging at all. */
static void test_paging_covers_every_note_exactly_once(void) {
    const size_t n = 40;
    fill(n, true);

    bool seen[64] = {false};
    size_t offset = 0;
    int pages = 0;
    char out[RESP];

    for (;;) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"list_notes\",\"offset\":%zu,\"limit\":5}", offset);
        dispatcher_handle(cmd, out, sizeof(out));
        if (!well_formed(out) || strstr(out, "\"ok\":true") == NULL) {
            break;
        }
        pages++;

        /* Record which notes this page carried, by their labels. */
        for (size_t i = 0; i < n; i++) {
            char needle[32];
            snprintf(needle, sizeof(needle), "\"note-%zu\"", i);
            if (strstr(out, needle)) {
                UL_CHECK(!seen[i], "no note is returned on two different pages");
                seen[i] = true;
            }
        }
        if (!has_field(out, "next_offset")) {
            break;
        }
        offset = (size_t)number_field(out, "next_offset");
        if (pages > 100) {
            break; /* a loop would otherwise hang the suite */
        }
    }

    UL_CHECK(pages == 8, "40 notes at 5 per page is 8 pages");
    bool all_seen = true;
    for (size_t i = 0; i < n; i++) {
        if (!seen[i]) {
            all_seen = false;
        }
    }
    UL_CHECK(all_seen, "every note appeared on exactly one page");
}

static void test_explicit_limit_is_honoured(void) {
    fill(20, true);
    char out[RESP];
    dispatcher_handle("{\"cmd\":\"list_notes\",\"limit\":4}", out, sizeof(out));

    UL_CHECK(count_notes(out) == 4, "a limit of 4 returns 4");
    UL_CHECK(number_field(out, "total") == 20, "while still reporting 20 in total");
    UL_CHECK(number_field(out, "next_offset") == 4, "and pointing at the fifth");
}

/* An explicit limit that cannot fit must be refused, not quietly reduced. A
 * client that asked for fifty and silently got eight would build a wrong
 * picture of the vault -- the same class of failure as the truncation this
 * replaced. */
static void test_an_impossible_limit_is_refused_not_shrunk(void) {
    fill(VAULT_MAX_NOTES, true);
    char out[RESP];
    dispatcher_handle("{\"cmd\":\"list_notes\",\"limit\":128}", out, sizeof(out));

    UL_CHECK(well_formed(out), "still well-formed");
    UL_CHECK(strstr(out, "response_too_large") != NULL, "refused explicitly");
    UL_CHECK(strstr(out, "\"ok\":true") == NULL, "and not reported as a success");
}

static void test_limit_zero_returns_just_the_count(void) {
    fill(9, true);
    char out[RESP];
    dispatcher_handle("{\"cmd\":\"list_notes\",\"limit\":0}", out, sizeof(out));

    UL_CHECK(well_formed(out), "well-formed");
    UL_CHECK(count_notes(out) == 0, "no notes returned");
    UL_CHECK(number_field(out, "total") == 9, "but the total is still reported");
    UL_CHECK(has_field(out, "next_offset"), "and there is more to fetch");
}

static void test_offset_at_the_end_is_empty_not_an_error(void) {
    fill(5, true);
    char out[RESP];
    dispatcher_handle("{\"cmd\":\"list_notes\",\"offset\":5}", out, sizeof(out));

    UL_CHECK(strstr(out, "\"ok\":true") != NULL,
             "paging exactly to the end is a valid empty page, not a failure");
    UL_CHECK(count_notes(out) == 0, "with no notes");
    UL_CHECK(!has_field(out, "next_offset"), "and no next_offset");
}

static void test_offset_past_the_end_is_a_bad_request(void) {
    fill(5, true);
    char out[RESP];
    dispatcher_handle("{\"cmd\":\"list_notes\",\"offset\":6}", out, sizeof(out));
    UL_CHECK(strstr(out, "bad_request") != NULL, "an offset past the end is a client bug");
}

/* The signature is what pushed the old limit from 29 notes down to 15, so the
 * two cases are worth distinguishing: without signatures more notes should fit
 * in the same buffer. */
static void test_more_fit_without_signatures(void) {
    char out[RESP];
    fill(VAULT_MAX_NOTES, true);
    dispatcher_handle("{\"cmd\":\"list_notes\"}", out, sizeof(out));
    int with = count_notes(out);
    UL_CHECK(well_formed(out), "well-formed with signatures");

    fill(VAULT_MAX_NOTES, false);
    dispatcher_handle("{\"cmd\":\"list_notes\"}", out, sizeof(out));
    int without = count_notes(out);
    UL_CHECK(well_formed(out), "well-formed without signatures");

    UL_CHECK(without > with, "a page holds more notes when they carry no signature");
}

void test_list_paging_run(void) {
    printf("-- list_notes paging --\n");
    test_a_full_vault_still_answers();
    test_a_small_vault_is_unchanged();
    test_paging_covers_every_note_exactly_once();
    test_explicit_limit_is_honoured();
    test_an_impossible_limit_is_refused_not_shrunk();
    test_limit_zero_returns_just_the_count();
    test_offset_at_the_end_is_empty_not_an_error();
    test_offset_past_the_end_is_a_bad_request();
    test_more_fit_without_signatures();
}
