#include <stdlib.h>
#include <string.h>

#include "dispatcher.h"
#include "json.h"
#include "unity_lite.h"
#include "vault.h"

static bool rng_basic(uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(rand() & 0xFF);
    }
    return true;
}

static void test_state_machine(void) {
    vault_init(NULL, NULL);
    srand(42);

    char id[VAULT_ID_BUF], h[VAULT_HASH_HEX_BUF];
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, "test note", id, h) == VAULT_OK,
             "new_secret succeeds");
    UL_CHECK(strlen(h) == 64, "disclosed hash is 32 bytes hex");

    note_meta_t meta;
    UL_CHECK(vault_get_meta(id, &meta) && meta.state == NOTE_STATE_PENDING,
             "a freshly generated note starts PENDING");

    char hex[VAULT_SECRET_HEX_BUF];
    UL_CHECK(vault_export_secret(id, hex) == VAULT_ERR_INVALID_STATE,
             "a PENDING note's secret cannot be exported");
    UL_CHECK(vault_mark_spent(id) == VAULT_ERR_INVALID_STATE,
             "a PENDING note cannot be marked spent");
    UL_CHECK(vault_delete(id) == VAULT_ERR_INVALID_STATE,
             "a PENDING note cannot be deleted (must discard)");

    UL_CHECK(vault_confirm(id, 21000, "mint.example", NULL) == VAULT_OK,
             "confirm transitions PENDING -> CONFIRMED");
    UL_CHECK(vault_confirm(id, 21000, "mint.example", NULL) == VAULT_ERR_INVALID_STATE,
             "confirming an already-CONFIRMED note is rejected");

    UL_CHECK(vault_get_meta(id, &meta) && meta.state == NOTE_STATE_CONFIRMED &&
                 meta.amount_msat == 21000 && strcmp(meta.host, "mint.example") == 0,
             "confirm records amount and host");

    UL_CHECK(vault_export_secret(id, hex) == VAULT_OK, "a CONFIRMED note's secret can be exported");
    UL_CHECK(strlen(hex) == 64, "exported secret is 32 bytes hex");

    UL_CHECK(vault_delete(id) == VAULT_ERR_INVALID_STATE,
             "a CONFIRMED note cannot be deleted (must be spent first)");

    UL_CHECK(vault_mark_spent(id) == VAULT_OK, "mark_spent transitions CONFIRMED -> SPENT");
    UL_CHECK(vault_export_secret(id, hex) == VAULT_ERR_INVALID_STATE,
             "a SPENT note's secret can no longer be exported");
    UL_CHECK(vault_mark_spent(id) == VAULT_ERR_INVALID_STATE, "double-spend is rejected");

    UL_CHECK(vault_delete(id) == VAULT_OK, "a SPENT note can be deleted");
    UL_CHECK(!vault_get_meta(id, &meta), "a deleted note is gone");

    char id2[VAULT_ID_BUF], h2[VAULT_HASH_HEX_BUF];
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, NULL, id2, h2) == VAULT_OK, "second new_secret ok");
    UL_CHECK(vault_discard(id2) == VAULT_OK, "discard removes a PENDING note the mint rejected");
    UL_CHECK(!vault_get_meta(id2, &meta), "a discarded note is gone");

    UL_CHECK(vault_confirm("deadbeef", 0, "h", NULL) == VAULT_ERR_NOT_FOUND,
             "operating on an unknown id fails not_found");
    UL_CHECK(vault_export_secret("deadbeef", hex) == VAULT_ERR_NOT_FOUND,
             "exporting an unknown id fails not_found");
}

static void test_split_and_merge_lineage(void) {
    vault_init(NULL, NULL);
    srand(7);

    char parent[VAULT_ID_BUF], hparent[VAULT_HASH_HEX_BUF];
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, NULL, parent, hparent) == VAULT_OK,
             "parent note created");
    UL_CHECK(vault_confirm(parent, 100000, "mint.example", NULL) == VAULT_OK, "parent confirmed");

    char parents[1][VAULT_ID_BUF];
    memcpy(parents[0], parent, VAULT_ID_BUF);

    char id1[VAULT_ID_BUF], h1[VAULT_HASH_HEX_BUF];
    char id2[VAULT_ID_BUF], h2[VAULT_HASH_HEX_BUF];
    UL_CHECK(vault_new_secret_pair(rng_basic, parents, 1, NULL, id1, h1, id2, h2) == VAULT_OK,
             "split (new_secret_pair) succeeds");
    UL_CHECK(strcmp(id1, id2) != 0, "split produces two distinct note ids");
    UL_CHECK(strcmp(h1, h2) != 0, "split produces two distinct secrets/hashes");

    note_meta_t m1;
    UL_CHECK(vault_get_meta(id1, &m1) && m1.parent_count == 1 &&
                 strcmp(m1.parent_ids[0], parent) == 0,
             "split output records its parent lineage");

    UL_CHECK(vault_confirm(id1, 60000, "mint.example", NULL) == VAULT_OK, "confirm split output 1");
    UL_CHECK(vault_confirm(id2, 40000, "mint.example", NULL) == VAULT_OK, "confirm split output 2");
    UL_CHECK(vault_mark_spent(parent) == VAULT_OK,
             "the parent is only burned after both split outputs are confirmed");

    /* merge: many parents -> one child */
    char m_a[VAULT_ID_BUF], hm_a[VAULT_HASH_HEX_BUF];
    char m_b[VAULT_ID_BUF], hm_b[VAULT_HASH_HEX_BUF];
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, NULL, m_a, hm_a) == VAULT_OK, "merge input a");
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, NULL, m_b, hm_b) == VAULT_OK, "merge input b");
    UL_CHECK(vault_confirm(m_a, 1000, "mint.example", NULL) == VAULT_OK, "confirm merge input a");
    UL_CHECK(vault_confirm(m_b, 2000, "mint.example", NULL) == VAULT_OK, "confirm merge input b");

    char merge_parents[2][VAULT_ID_BUF];
    memcpy(merge_parents[0], m_a, VAULT_ID_BUF);
    memcpy(merge_parents[1], m_b, VAULT_ID_BUF);
    char merged_id[VAULT_ID_BUF], merged_h[VAULT_HASH_HEX_BUF];
    UL_CHECK(vault_new_secret(rng_basic, merge_parents, 2, NULL, merged_id, merged_h) == VAULT_OK,
             "merge (new_secret with many parents) succeeds");
    note_meta_t mm;
    UL_CHECK(vault_get_meta(merged_id, &mm) && mm.parent_count == 2, "merged note records both parents");
    UL_CHECK(vault_confirm(merged_id, 3000, "mint.example", NULL) == VAULT_OK, "confirm merged note");
    UL_CHECK(vault_mark_spent(m_a) == VAULT_OK && vault_mark_spent(m_b) == VAULT_OK,
             "both merge inputs burned once the merged note is confirmed");
}

static int g_id_call_count = 0;

/* Returns id bytes 0xAA on the first call, 0xBB thereafter — used to force
 * an id collision on the first attempt and prove the retry loop recovers. */
static bool rng_force_collision_then_unique(uint8_t *out, size_t len) {
    if (len == 4) {
        g_id_call_count++;
        memset(out, g_id_call_count == 1 ? 0xAA : 0xBB, len);
    } else {
        memset(out, 0x11, len);
    }
    return true;
}

static void test_id_collision_retry(void) {
    vault_init(NULL, NULL);

    g_id_call_count = 0;
    char id1[VAULT_ID_BUF], h1[VAULT_HASH_HEX_BUF];
    UL_CHECK(vault_new_secret(rng_force_collision_then_unique, NULL, 0, NULL, id1, h1) == VAULT_OK,
             "seed note created");
    UL_CHECK(strcmp(id1, "aaaaaaaa") == 0, "seed note got the expected id");

    /* Reset the call counter so the next id generation collides with id1 on
     * its first attempt (0xAA again) before succeeding on retry (0xBB). */
    g_id_call_count = 0;
    char id2[VAULT_ID_BUF], h2[VAULT_HASH_HEX_BUF];
    UL_CHECK(vault_new_secret(rng_force_collision_then_unique, NULL, 0, NULL, id2, h2) == VAULT_OK,
             "second note created despite a colliding first id attempt");
    UL_CHECK(strcmp(id2, "bbbbbbbb") == 0,
             "collision retry produced a different id instead of overwriting the first note");
}

/* ---- persistence round-trip against a fake storage backend ------------ */

#define FAKE_MAX 8
static note_t fake_note_store[FAKE_MAX];
static bool fake_note_used[FAKE_MAX];
static char fake_index_ids[FAKE_MAX][VAULT_ID_BUF];
static size_t fake_index_count;

static int fake_note_find(const char *id) {
    for (int i = 0; i < FAKE_MAX; i++) {
        if (fake_note_used[i] && strcmp(fake_note_store[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* When set, the matching save/delete reports failure -- a full NVS partition
 * or a transient flash write error. The stored data is left as it was, exactly
 * as a failed nvs_set_blob/commit leaves the prior value intact. */
static bool fake_save_note_fails = false;
static bool fake_save_index_fails = false;
static bool fake_delete_note_fails = false;

static bool fake_save_note(const note_t *note, void *ctx) {
    (void)ctx;
    if (fake_save_note_fails) {
        return false;
    }
    int idx = fake_note_find(note->id);
    if (idx < 0) {
        for (int i = 0; i < FAKE_MAX; i++) {
            if (!fake_note_used[i]) {
                idx = i;
                fake_note_used[i] = true;
                break;
            }
        }
        if (idx < 0) {
            return false;
        }
    }
    fake_note_store[idx] = *note;
    return true;
}

/* When set, load_note reports success for this id having written only its
 * leading bytes — what ESP-IDF's nvs_get_blob does for a stored blob shorter
 * than the buffer (it returns ESP_OK and fills only what it had). */
static const char *fake_short_note_id = NULL;

static bool fake_load_note(const char *id, note_t *out, void *ctx) {
    (void)ctx;
    int idx = fake_note_find(id);
    if (idx < 0) {
        return false;
    }
    if (fake_short_note_id && strcmp(id, fake_short_note_id) == 0) {
        memcpy(out, &fake_note_store[idx], VAULT_ID_BUF); /* the id, and nothing after it */
        return true;
    }
    *out = fake_note_store[idx];
    return true;
}

static bool fake_delete_note(const char *id, void *ctx) {
    (void)ctx;
    if (fake_delete_note_fails) {
        return false;
    }
    int idx = fake_note_find(id);
    if (idx < 0) {
        return false;
    }
    fake_note_used[idx] = false;
    return true;
}

/* When set, load_index reports failure — a transient flash read error, or an
 * index blob larger than the buffer (which nvs_get_blob answers with
 * ESP_ERR_NVS_INVALID_LENGTH, e.g. after VAULT_MAX_NOTES shrinks). The stored
 * index is left completely intact; only this read of it fails. */
static bool fake_index_read_fails = false;

static bool fake_load_index(char ids[][VAULT_ID_BUF], size_t max, size_t *count, void *ctx) {
    (void)ctx;
    if (fake_index_read_fails) {
        return false;
    }
    size_t n = fake_index_count < max ? fake_index_count : max;
    for (size_t i = 0; i < n; i++) {
        memcpy(ids[i], fake_index_ids[i], VAULT_ID_BUF);
    }
    *count = n;
    return true;
}

static bool fake_save_index(const char ids[][VAULT_ID_BUF], size_t count, void *ctx) {
    (void)ctx;
    if (fake_save_index_fails) {
        return false;
    }
    fake_index_count = count < FAKE_MAX ? count : FAKE_MAX;
    for (size_t i = 0; i < fake_index_count; i++) {
        memcpy(fake_index_ids[i], ids[i], VAULT_ID_BUF);
    }
    return true;
}

static void fake_reset(void) {
    memset(fake_note_used, 0, sizeof(fake_note_used));
    fake_index_count = 0;
    fake_short_note_id = NULL;
    fake_index_read_fails = false;
    fake_save_note_fails = false;
    fake_save_index_fails = false;
    fake_delete_note_fails = false;
}

/* persist_index() already refuses to drop an id whose *note* failed to load,
 * on the grounds that a failed read is not proof the note is gone. The same
 * argument applies one level up and was not covered: if the *index* itself
 * fails to read, vault_init returns early with both lists empty, and the
 * next mutation persists an index built from what is in RAM -- nothing. The
 * blobs survive on flash with nothing referencing them, which for bearer
 * notes is indistinguishable from destroying them. */
static void test_unreadable_index_is_not_an_empty_vault(void) {
    fake_reset();
    vault_storage_t storage = {
        .load_index = fake_load_index,
        .save_index = fake_save_index,
        .load_note = fake_load_note,
        .save_note = fake_save_note,
        .delete_note = fake_delete_note,
        .ctx = NULL,
    };

    vault_init(&storage, NULL);
    srand(1234);
    char id_a[VAULT_ID_BUF], id_b[VAULT_ID_BUF], h[VAULT_HASH_HEX_BUF];
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, "a", id_a, h) == VAULT_OK, "note A created");
    UL_CHECK(vault_confirm(id_a, 111000, "mint.example", NULL) == VAULT_OK, "note A confirmed");
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, "b", id_b, h) == VAULT_OK, "note B created");
    UL_CHECK(vault_confirm(id_b, 222000, "mint.example", NULL) == VAULT_OK, "note B confirmed");
    size_t saved_count = fake_index_count;
    char saved_index[FAKE_MAX][VAULT_ID_BUF];
    memcpy(saved_index, fake_index_ids, sizeof(saved_index));
    UL_CHECK(saved_count == 2, "both notes are in the persisted index");

    /* Boot once with the index unreadable. The notes are all still there. */
    fake_index_read_fails = true;
    vault_init(&storage, NULL);
    size_t n_count, p_count;
    vault_get_info(&n_count, &p_count);
    UL_CHECK(n_count == 0, "a boot that cannot read the index sees no notes");
    UL_CHECK(!vault_index_known(),
             "and says so, rather than looking like a vault that is simply empty");

    /* The distinction has to reach the host. Zero notes and a refusal to
     * write are both consistent with a healthy empty vault, so get_info has
     * to name the real reason -- and must not say storage_full, whose
     * remedy (delete notes, or wipe) would destroy what is being protected. */
    dispatcher_deps_t info_deps = {0};
    dispatcher_init(&info_deps);
    char info[512];
    dispatcher_handle("{\"cmd\":\"get_info\"}", info, sizeof(info));
    char storage_state[32];
    UL_CHECK(json_get_str(info, "storage", storage_state, sizeof(storage_state)) &&
                 strcmp(storage_state, "index_unreadable") == 0,
             "get_info reports index_unreadable, not a healthy-looking empty vault");

    char id_c[VAULT_ID_BUF];
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, "c", id_c, h) != VAULT_OK,
             "a new note is refused while the index is unknown, not silently unpersisted");
    UL_CHECK(vault_import_secret(rng_basic,
                                  "00112233445566778899aabbccddeeff"
                                  "00112233445566778899aabbccddeeff",
                                  "mint.example", 5000, "imported", id_c) != VAULT_OK,
             "import is refused too -- it is the other creation path");
    /* Compare the ids, not just how many there are: two refused creations
     * would write two entries and leave the count looking untouched while
     * naming entirely different notes. */
    UL_CHECK(fake_index_count == saved_count &&
                 memcmp(saved_index, fake_index_ids, saved_count * VAULT_ID_BUF) == 0,
             "the persisted index still names exactly the notes it did before");

    /* Flash recovers. Both notes must still be reachable. */
    fake_index_read_fails = false;
    vault_init(&storage, NULL);
    UL_CHECK(vault_index_known(), "a boot that can read the index says so again");
    note_meta_t meta;
    UL_CHECK(vault_get_meta(id_a, &meta) && meta.amount_msat == 111000,
             "note A survives a boot that could not read the index");
    UL_CHECK(vault_get_meta(id_b, &meta) && meta.amount_msat == 222000,
             "note B survives a boot that could not read the index");
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, "d", id_c, h) == VAULT_OK,
             "writes resume once a boot can read the index again");
}

/* A note arriving from storage has never been through new_note(), which is
 * the only place parent_count is bounded. dispatcher.c serializes
 * parent_ids[0 .. parent_count) straight onto the wire, and past the end of
 * that fixed 16-entry array is the next note in g_notes — whose first fields
 * are its id and then its secret. list_notes has no physical gate, so an
 * out-of-range count read from flash is a disclosure, not just a wrong
 * answer. */
static void test_load_bounds_parent_count(void) {
    fake_reset();
    vault_storage_t storage = {
        .load_index = fake_load_index,
        .save_index = fake_save_index,
        .load_note = fake_load_note,
        .save_note = fake_save_note,
        .delete_note = fake_delete_note,
        .ctx = NULL,
    };

    vault_init(&storage, NULL);
    srand(4242);
    char id[VAULT_ID_BUF], h[VAULT_HASH_HEX_BUF];
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, "lineage", id, h) == VAULT_OK, "note created");

    /* Corrupt the persisted blob the way a different note_t layout would. */
    int idx = fake_note_find(id);
    UL_CHECK(idx >= 0, "the note reached the fake backend");
    fake_note_store[idx].parent_count = 200;

    vault_init(&storage, NULL);
    note_meta_t meta;
    UL_CHECK(vault_get_meta(id, &meta), "a note with an out-of-range parent_count still loads");
    UL_CHECK(meta.parent_count <= VAULT_MAX_PARENTS,
             "parent_count from storage is clamped to the size of the array it indexes");

    /* A backend that reports success while writing less than a whole note_t
     * must not leave the untouched tail carrying the previous note's fields.
     * vault_init reuses one stack slot for every iteration of its load loop,
     * so without zeroing, the short note inherits the tail of the note loaded
     * just before it — including its parent_count and its amount. */
    fake_reset();
    vault_init(&storage, NULL);
    char full_id[VAULT_ID_BUF], short_id[VAULT_ID_BUF];
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, "full", full_id, h) == VAULT_OK, "first note");
    UL_CHECK(vault_confirm(full_id, 777000, "mint.example", NULL) == VAULT_OK,
             "first note carries a distinctive amount");
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, "short", short_id, h) == VAULT_OK, "second note");
    fake_note_store[fake_note_find(full_id)].parent_count = VAULT_MAX_PARENTS;

    fake_short_note_id = short_id;
    vault_init(&storage, NULL);
    if (vault_get_meta(short_id, &meta)) {
        UL_CHECK(meta.amount_msat == 0,
                 "a partially-read note does not inherit the previous note's amount");
        UL_CHECK(meta.parent_count == 0,
                 "a partially-read note does not inherit the previous note's parent_count");
    } else {
        UL_CHECK(true, "a partially-read note is refused outright");
    }
    fake_short_note_id = NULL;
}

static void test_persistence_roundtrip(void) {
    fake_reset();
    vault_storage_t storage = {
        .load_index = fake_load_index,
        .save_index = fake_save_index,
        .load_note = fake_load_note,
        .save_note = fake_save_note,
        .delete_note = fake_delete_note,
        .ctx = NULL,
    };

    vault_init(&storage, NULL);
    srand(99);
    char id[VAULT_ID_BUF], h[VAULT_HASH_HEX_BUF];
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, "persisted", id, h) == VAULT_OK,
             "note created against a real storage backend");
    UL_CHECK(vault_confirm(id, 5000, "mint.example", NULL) == VAULT_OK, "note confirmed");

    /* Simulate a reboot: reinitialize purely from what the fake backend
     * says was persisted. */
    vault_init(&storage, NULL);
    note_meta_t meta;
    UL_CHECK(vault_get_meta(id, &meta), "note survives a reload from storage");
    UL_CHECK(meta.state == NOTE_STATE_CONFIRMED && meta.amount_msat == 5000,
             "state and amount survive a reload from storage");
    UL_CHECK(strcmp(meta.label, "persisted") == 0, "label survives a reload from storage");

    UL_CHECK(vault_mark_spent(id) == VAULT_OK, "spend");
    UL_CHECK(vault_delete(id) == VAULT_OK, "delete spent note");
    vault_init(&storage, NULL);
    UL_CHECK(!vault_get_meta(id, &meta), "deletion survives a reload from storage");
}

/* A write that does not reach flash must not be reported as success. Storage
 * failing is an ordinary outcome of use -- a full NVS partition is not
 * corruption -- and a note the host is told exists but that never persisted is
 * money destroyed on the next reboot. Every mutation rolls its RAM change back
 * and returns an error when persistence fails, so RAM never disagrees with what
 * a reboot would restore. */
static void test_persist_failure_is_refused(void) {
    fake_reset();
    vault_storage_t storage = {
        .load_index = fake_load_index,
        .save_index = fake_save_index,
        .load_note = fake_load_note,
        .save_note = fake_save_note,
        .delete_note = fake_delete_note,
        .ctx = NULL,
    };
    vault_init(&storage, NULL);
    srand(2024);
    char id[VAULT_ID_BUF], h[VAULT_HASH_HEX_BUF];
    note_meta_t meta;

    /* Creation, blob write fails: refused, and nothing left behind. */
    fake_save_note_fails = true;
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, "doomed", id, h) != VAULT_OK,
             "new_secret whose blob write fails is refused, not reported ok:true");
    fake_save_note_fails = false;
    UL_CHECK(vault_count() == 0, "a refused creation leaves no note in RAM");
    vault_init(&storage, NULL);
    UL_CHECK(vault_count() == 0, "and none on flash after a reload");

    /* Creation, index write fails: refused too, and the written blob is undone
     * so it is not left as an orphan a later boot would resurrect. */
    fake_save_index_fails = true;
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, "doomed2", id, h) != VAULT_OK,
             "new_secret whose index write fails is refused");
    fake_save_index_fails = false;
    UL_CHECK(vault_count() == 0, "a refused creation leaves no note in RAM");
    vault_init(&storage, NULL);
    UL_CHECK(vault_count() == 0 && !vault_get_meta(id, &meta),
             "the rolled-back blob is not left behind as a loadable note");

    /* A note that DID persist, then a spend whose write fails: it stays
     * CONFIRMED in RAM, not SPENT -- no reboot-resurrected double-spend. */
    char keeper[VAULT_ID_BUF];
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, "keeper", keeper, h) == VAULT_OK,
             "a note persists when storage is healthy");
    UL_CHECK(vault_confirm(keeper, 4200, "mint.example", NULL) == VAULT_OK, "and confirms");

    fake_save_note_fails = true;
    UL_CHECK(vault_mark_spent(keeper) != VAULT_OK, "mark_spent whose write fails is refused");
    fake_save_note_fails = false;
    UL_CHECK(vault_get_meta(keeper, &meta) && meta.state == NOTE_STATE_CONFIRMED,
             "the note stays CONFIRMED, not SPENT-only-in-RAM");
    UL_CHECK(vault_mark_spent(keeper) == VAULT_OK, "and a retry once storage recovers succeeds");

    /* confirm whose write fails leaves the note PENDING, so export keeps
     * refusing it until the confirm actually reaches flash. */
    char pending[VAULT_ID_BUF];
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, "pending", pending, h) == VAULT_OK,
             "a fresh PENDING note");
    fake_save_note_fails = true;
    UL_CHECK(vault_confirm(pending, 9000, "mint.example", NULL) != VAULT_OK,
             "confirm whose write fails is refused");
    fake_save_note_fails = false;
    UL_CHECK(vault_get_meta(pending, &meta) && meta.state == NOTE_STATE_PENDING,
             "the note stays PENDING rather than CONFIRMED-only-in-RAM");
}

/* A firmware boot whose storage was expected but could not be brought up must
 * not mint RAM-only notes it will lose at the next reset (issue #72) -- unlike
 * vault_init(NULL), the in-RAM-by-design mode the other tests use. */
static void test_storage_unavailable_fails_closed(void) {
    vault_init_storage_unavailable(NULL);
    UL_CHECK(!vault_index_known(), "a storage-unavailable boot reports the index as unknown");
    char id[VAULT_ID_BUF], h[VAULT_HASH_HEX_BUF];
    UL_CHECK(vault_new_secret(rng_basic, NULL, 0, "x", id, h) != VAULT_OK,
             "new_secret is refused when storage is unavailable, not run in-RAM");
    UL_CHECK(vault_import_secret(rng_basic,
                 "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
                 "mint", 1000, "x", id) != VAULT_OK,
             "import is refused too when storage is unavailable");
    UL_CHECK(vault_count() == 0, "and nothing is created");
}

void test_vault_run(void) {
    test_state_machine();
    test_split_and_merge_lineage();
    test_id_collision_retry();
    test_persistence_roundtrip();
    test_load_bounds_parent_count();
    test_unreadable_index_is_not_an_empty_vault();
    test_persist_failure_is_refused();
    test_storage_unavailable_fails_closed();
}
