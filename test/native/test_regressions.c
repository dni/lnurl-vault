/* Regression tests for bugs found in a production-readiness review of the
 * firmware.
 *
 * These are deliberately written *before* their fixes, so a failure here is
 * the bug being demonstrated rather than something newly broken. Each test
 * names the issue it pins down. Once the corresponding fix lands, the test
 * stays behind as the guard against it coming back.
 *
 *   test_list_notes_stays_well_formed    issue #7   truncated JSON response
 *   test_u64_rejects_overflow            issue #17  amount_msat wraps silently
 *   test_pair_never_mints_duplicate_id   issue #17  split can reuse an id
 *   test_partial_load_does_not_orphan    issue #6   a read failure loses a note
 */
#include <stdint.h>
#include <string.h>

#include "dispatcher.h"
#include "json.h"
#include "unity_lite.h"
#include "vault.h"

/* ---- helpers ---------------------------------------------------------- */

/* Deterministic PRNG. Not cryptographic and not meant to be: these tests care
 * that ids differ from each other, not that they are unpredictable. */
static uint32_t g_seq;

static bool rng_seq(uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        g_seq = g_seq * 1103515245u + 12345u;
        out[i] = (uint8_t)(g_seq >> 16);
    }
    return true;
}

/* True if `s` is a structurally complete JSON document: every bracket closed,
 * in the right order, with no unterminated string. Deliberately not a full
 * validator -- it exists to tell a finished response from a truncated one,
 * which is exactly the failure mode issue #7 describes. */
static bool json_is_complete(const char *s) {
    char stack[16];
    size_t depth = 0;
    bool in_str = false;
    bool saw_open = false;

    for (const char *p = s; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1]) {
                p++;
            } else if (*p == '"') {
                in_str = false;
            }
            continue;
        }
        switch (*p) {
            case '"':
                in_str = true;
                break;
            case '{':
            case '[':
                if (depth >= sizeof(stack)) {
                    return false;
                }
                stack[depth++] = *p;
                saw_open = true;
                break;
            case '}':
                if (depth == 0 || stack[--depth] != '{') {
                    return false;
                }
                break;
            case ']':
                if (depth == 0 || stack[--depth] != '[') {
                    return false;
                }
                break;
            default:
                break;
        }
    }
    return saw_open && depth == 0 && !in_str;
}

/* A 132-char hex signature, the widest a note can carry (VAULT_SIG_BUF). Notes
 * that carry one are far bulkier on the wire, which is what makes the
 * list_notes ceiling so much lower in practice than VAULT_MAX_NOTES suggests. */
static const char *WIDE_SIG =
    "3045022100aa0220bb3045022100aa0220bb3045022100aa0220bb3045022100aa0220bb"
    "3045022100aa0220bb3045022100aa0220bb3045022100aa0220bbccdd";

/* ---- issue #7: list_notes must never emit a truncated response --------- */

static void check_list_notes_at(size_t n, bool with_sig, const char *what) {
    vault_init(NULL, NULL);
    g_seq = 1;

    for (size_t i = 0; i < n; i++) {
        char id[VAULT_ID_BUF], h[VAULT_HASH_HEX_BUF];
        if (vault_new_secret(rng_seq, NULL, 0, "note", id, h) != VAULT_OK) {
            break;
        }
        vault_confirm(id, 21000, "mint.example.com", with_sig ? WIDE_SIG : NULL);
    }

    /* 4096 bytes is what serial_cdc.c and ble_gatt.c actually hand the
     * dispatcher, so this is the real ceiling rather than a synthetic one. */
    static char out[4096];
    dispatcher_handle("{\"cmd\":\"list_notes\"}", out, sizeof(out));

    /* The invariant is not "every note fits" -- paging is a perfectly good
     * answer. It is that whatever comes back is parseable, so a client can
     * tell success from failure instead of hitting a JSON syntax error. */
    UL_CHECK(json_is_complete(out), what);
}

static void test_list_notes_stays_well_formed(void) {
    check_list_notes_at(1, false, "list_notes is well-formed with 1 note");
    check_list_notes_at(20, false, "list_notes is well-formed with 20 notes");
    check_list_notes_at(40, false, "list_notes is well-formed with 40 notes");
    check_list_notes_at(16, true, "list_notes is well-formed with 16 signed notes");
    check_list_notes_at(64, true, "list_notes is well-formed with 64 signed notes");
    check_list_notes_at(VAULT_MAX_NOTES, true,
                        "list_notes is well-formed at VAULT_MAX_NOTES signed notes");
}

/* ---- issue #17: amount_msat must not wrap ------------------------------ */

static void test_u64_rejects_overflow(void) {
    uint64_t v;

    v = 0;
    UL_CHECK(json_get_u64("{\"amount_msat\":18446744073709551615}", "amount_msat", &v) &&
                 v == UINT64_MAX,
             "the largest valid uint64 is still accepted");

    v = 12345;
    UL_CHECK(!json_get_u64("{\"amount_msat\":18446744073709551616}", "amount_msat", &v),
             "2^64 is rejected rather than wrapping");

    v = 12345;
    UL_CHECK(!json_get_u64("{\"amount_msat\":99999999999999999999999}", "amount_msat", &v),
             "a wildly oversized amount is rejected rather than wrapping");
}

/* ---- issue #17: a split must not mint two notes sharing one id --------- */

/* Always returns the same bytes. gen_unique_id() asks for 4 bytes at a time,
 * so both halves of a split are handed an identical candidate id -- and the
 * second one is generated before the first has been stored, so the
 * uniqueness check cannot see it. */
static bool rng_fixed(uint8_t *out, size_t len) {
    memset(out, len == 4 ? 0xC0 : 0x22, len);
    return true;
}

static void test_pair_never_mints_duplicate_id(void) {
    vault_init(NULL, NULL);

    char id1[VAULT_ID_BUF], h1[VAULT_HASH_HEX_BUF];
    char id2[VAULT_ID_BUF], h2[VAULT_HASH_HEX_BUF];
    vault_err_t err = vault_new_secret_pair(rng_fixed, NULL, 0, NULL, id1, h1, id2, h2);

    /* Refusing outright is an acceptable answer; handing back a collision is
     * not. find_index() returns the first match, so a duplicate makes one of
     * the two notes permanently unaddressable. */
    UL_CHECK(err != VAULT_OK || strcmp(id1, id2) != 0,
             "new_secret_pair does not mint two notes sharing one id");

    if (err == VAULT_OK) {
        UL_CHECK(vault_confirm(id1, 60000, "mint.example", NULL) == VAULT_OK,
                 "the first split output confirms");
        note_meta_t m2;
        UL_CHECK(vault_get_meta(id2, &m2) && m2.state == NOTE_STATE_PENDING,
                 "confirming one split output does not silently confirm the other");
    }
}

/* ---- issue #6: a failed note read must not orphan the note ------------- */

#define RS_MAX 8

static note_t rs_notes[RS_MAX];
static bool rs_used[RS_MAX];
static char rs_index[RS_MAX][VAULT_ID_BUF];
static size_t rs_index_count;

/* When set, load_note() fails for this id and only this id, standing in for a
 * transient read error rather than a deleted blob. */
static const char *rs_fail_load_id;

static int rs_find(const char *id) {
    for (int i = 0; i < RS_MAX; i++) {
        if (rs_used[i] && strcmp(rs_notes[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static bool rs_save_note(const note_t *note, void *ctx) {
    (void)ctx;
    int idx = rs_find(note->id);
    if (idx < 0) {
        for (int i = 0; i < RS_MAX; i++) {
            if (!rs_used[i]) {
                idx = i;
                rs_used[i] = true;
                break;
            }
        }
        if (idx < 0) {
            return false;
        }
    }
    rs_notes[idx] = *note;
    return true;
}

static bool rs_load_note(const char *id, note_t *out, void *ctx) {
    (void)ctx;
    if (rs_fail_load_id && strcmp(id, rs_fail_load_id) == 0) {
        return false;
    }
    int idx = rs_find(id);
    if (idx < 0) {
        return false;
    }
    *out = rs_notes[idx];
    return true;
}

static bool rs_delete_note(const char *id, void *ctx) {
    (void)ctx;
    int idx = rs_find(id);
    if (idx < 0) {
        return false;
    }
    rs_used[idx] = false;
    return true;
}

static bool rs_load_index(char ids[][VAULT_ID_BUF], size_t max, size_t *count, void *ctx) {
    (void)ctx;
    size_t n = rs_index_count < max ? rs_index_count : max;
    for (size_t i = 0; i < n; i++) {
        memcpy(ids[i], rs_index[i], VAULT_ID_BUF);
    }
    *count = n;
    return true;
}

static bool rs_save_index(const char ids[][VAULT_ID_BUF], size_t count, void *ctx) {
    (void)ctx;
    rs_index_count = count < RS_MAX ? count : RS_MAX;
    for (size_t i = 0; i < rs_index_count; i++) {
        memcpy(rs_index[i], ids[i], VAULT_ID_BUF);
    }
    return true;
}

static void test_partial_load_does_not_orphan(void) {
    memset(rs_used, 0, sizeof(rs_used));
    rs_index_count = 0;
    rs_fail_load_id = NULL;

    vault_storage_t storage = {
        .load_index = rs_load_index,
        .save_index = rs_save_index,
        .load_note = rs_load_note,
        .save_note = rs_save_note,
        .delete_note = rs_delete_note,
        .ctx = NULL,
    };

    vault_init(&storage, NULL);
    g_seq = 5;

    char ids[3][VAULT_ID_BUF];
    char h[VAULT_HASH_HEX_BUF];
    for (int i = 0; i < 3; i++) {
        UL_CHECK(vault_new_secret(rng_seq, NULL, 0, "n", ids[i], h) == VAULT_OK,
                 "note persisted before the simulated read failure");
        vault_confirm(ids[i], 1000, "mint.example", NULL);
    }

    /* Reboot, with the middle note's blob temporarily unreadable. */
    rs_fail_load_id = ids[1];
    vault_init(&storage, NULL);

    /* The owner carries on using the device. Creating a note rewrites the
     * index from what is currently in RAM -- which no longer mentions the
     * note that failed to load. */
    char newid[VAULT_ID_BUF];
    UL_CHECK(vault_new_secret(rng_seq, NULL, 0, "after reboot", newid, h) == VAULT_OK,
             "a new note can still be created after a partial load");

    /* The read error clears. The blob was never deleted, so the note should
     * still be recoverable. */
    rs_fail_load_id = NULL;
    vault_init(&storage, NULL);

    note_meta_t m;
    UL_CHECK(vault_get_meta(ids[1], &m),
             "a transient read failure does not permanently orphan a note");
}

void test_regressions_run(void) {
    /* Self-contained rather than relying on test_dispatcher.c having run
     * first and left the dispatcher's deps populated. */
    dispatcher_deps_t deps = {.rng = rng_seq, .confirm_export = NULL};
    dispatcher_init(&deps);

    test_list_notes_stays_well_formed();
    test_u64_rejects_overflow();
    test_pair_never_mints_duplicate_id();
    test_partial_load_does_not_orphan();
}
