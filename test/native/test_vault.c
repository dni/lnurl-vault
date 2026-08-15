#include <stdlib.h>
#include <string.h>

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

static bool fake_save_note(const note_t *note, void *ctx) {
    (void)ctx;
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

static bool fake_load_note(const char *id, note_t *out, void *ctx) {
    (void)ctx;
    int idx = fake_note_find(id);
    if (idx < 0) {
        return false;
    }
    *out = fake_note_store[idx];
    return true;
}

static bool fake_delete_note(const char *id, void *ctx) {
    (void)ctx;
    int idx = fake_note_find(id);
    if (idx < 0) {
        return false;
    }
    fake_note_used[idx] = false;
    return true;
}

static bool fake_load_index(char ids[][VAULT_ID_BUF], size_t max, size_t *count, void *ctx) {
    (void)ctx;
    size_t n = fake_index_count < max ? fake_index_count : max;
    for (size_t i = 0; i < n; i++) {
        memcpy(ids[i], fake_index_ids[i], VAULT_ID_BUF);
    }
    *count = n;
    return true;
}

static bool fake_save_index(const char ids[][VAULT_ID_BUF], size_t count, void *ctx) {
    (void)ctx;
    fake_index_count = count < FAKE_MAX ? count : FAKE_MAX;
    for (size_t i = 0; i < fake_index_count; i++) {
        memcpy(fake_index_ids[i], ids[i], VAULT_ID_BUF);
    }
    return true;
}

static void fake_reset(void) {
    memset(fake_note_used, 0, sizeof(fake_note_used));
    fake_index_count = 0;
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

void test_vault_run(void) {
    test_state_machine();
    test_split_and_merge_lineage();
    test_id_collision_retry();
    test_persistence_roundtrip();
}
