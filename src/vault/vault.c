#include "vault.h"

#include <string.h>

#include "hex.h"
#include "sha256.h"

static note_t g_notes[VAULT_MAX_NOTES];
static size_t g_note_count = 0;
static const vault_storage_t *g_storage = NULL;
static vault_time_fn g_now = NULL;

/* Ids the persisted index referenced at boot whose blobs would not load.
 * They are held here, out of g_notes, so persist_index() can carry them
 * through instead of dropping them -- see its comment. */
static char g_unloaded_ids[VAULT_MAX_NOTES][VAULT_ID_BUF];
static size_t g_unloaded_count = 0;

/* False when this boot could not read the persisted index at all, which is a
 * different situation from reading it and finding it empty: we know notes may
 * exist but not which ones. g_unloaded_ids covers the per-note version of
 * this, but it is built FROM the index, so it is empty in exactly this case
 * and protects nothing. True when there is no storage at all -- in-RAM mode
 * has no index to contradict. */
static bool g_index_known = true;

static void copy_trunc(char *dst, const char *src, size_t dstcap) {
    if (dstcap == 0) {
        return;
    }
    size_t i = 0;
    for (; i + 1 < dstcap && src && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static int find_index(const char *id) {
    for (size_t i = 0; i < g_note_count; i++) {
        if (strncmp(g_notes[i].id, id, VAULT_ID_BUF) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void persist_index(void) {
    if (!g_storage || !g_storage->save_index) {
        return;
    }
    /* The index on flash is the only thing that says which notes exist. If
     * this boot could not read it, everything below builds a replacement out
     * of what is in RAM -- which is nothing -- and writing that overwrites
     * the real list with a shorter one. The blobs survive, but nothing
     * references them again. Same reasoning as the carry-through below, one
     * level up: a failed read is not proof the notes are gone. */
    if (!g_index_known) {
        return;
    }
    char ids[VAULT_MAX_NOTES][VAULT_ID_BUF];
    size_t n = 0;
    for (size_t i = 0; i < g_note_count && n < VAULT_MAX_NOTES; i++) {
        memcpy(ids[n++], g_notes[i].id, VAULT_ID_BUF);
    }
    /* Carry through any id whose blob failed to load this boot. A failed read
     * is not proof the note is gone -- flash can fail transiently, and its
     * data may be perfectly intact next boot. Writing an index built only
     * from what happens to be in RAM would drop the reference for good, so a
     * single bad read would silently destroy a note holding real value. Notes
     * are only ever removed from the index by remove_at(), i.e. deliberately. */
    for (size_t i = 0; i < g_unloaded_count && n < VAULT_MAX_NOTES; i++) {
        memcpy(ids[n++], g_unloaded_ids[i], VAULT_ID_BUF);
    }
    g_storage->save_index(ids, n, g_storage->ctx);
}

static void persist_note(const note_t *n) {
    if (!g_storage || !g_storage->save_note) {
        return;
    }
    g_storage->save_note(n, g_storage->ctx);
}

/* Removes g_notes[idx], compacting the array, and persists the deletion. */
static void remove_at(size_t idx) {
    char id[VAULT_ID_BUF];
    memcpy(id, g_notes[idx].id, VAULT_ID_BUF);
    for (size_t i = idx; i + 1 < g_note_count; i++) {
        g_notes[i] = g_notes[i + 1];
    }
    g_note_count--;
    if (g_storage && g_storage->delete_note) {
        g_storage->delete_note(id, g_storage->ctx);
    }
    persist_index();
}

/* `reserved` is an id that is spoken for but not yet in g_notes, so
 * find_index() cannot see it. vault_new_secret_pair() generates both halves
 * of a split before storing either, so without this the second half can be
 * handed the first half's id -- and since find_index() returns the first
 * match, one of the two notes would be permanently unaddressable. NULL when
 * there is no such sibling. */
static bool gen_unique_id(vault_rng_fn rng, char out[VAULT_ID_BUF], const char *reserved) {
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t raw[4];
        if (!rng(raw, sizeof(raw))) {
            return false;
        }
        hex_encode(raw, sizeof(raw), out, VAULT_ID_BUF);
        bool clashes_with_reserved =
            reserved && strncmp(out, reserved, VAULT_ID_BUF) == 0;
        if (find_index(out) < 0 && !clashes_with_reserved) {
            return true;
        }
    }
    return false;
}

static void fill_meta(const note_t *n, note_meta_t *out) {
    memcpy(out->id, n->id, VAULT_ID_BUF);
    out->state = n->state;
    out->amount_msat = n->amount_msat;
    memcpy(out->label, n->label, VAULT_LABEL_BUF);
    memcpy(out->host, n->host, VAULT_HOST_BUF);
    memcpy(out->sig, n->sig, VAULT_SIG_BUF);
    memcpy(out->parent_ids, n->parent_ids, sizeof(out->parent_ids));
    out->parent_count = n->parent_count;
    out->created_at = n->created_at;
    out->updated_at = n->updated_at;
}

void vault_init(const vault_storage_t *storage, vault_time_fn now_fn) {
    g_storage = storage;
    g_now = now_fn;
    g_note_count = 0;
    g_unloaded_count = 0;
    g_index_known = true;

    if (!storage || !storage->load_index) {
        return; /* in-RAM mode: no persisted index to be wrong about */
    }

    char ids[VAULT_MAX_NOTES][VAULT_ID_BUF];
    size_t count = 0;
    if (!storage->load_index(ids, VAULT_MAX_NOTES, &count, storage->ctx)) {
        /* Not the same as an empty vault. Notes may well be on flash; we
         * just cannot say which. Every write is refused from here until a
         * boot that can read the index, so this state cannot be mistaken for
         * "no notes" and then made true by overwriting the index. */
        g_index_known = false;
        return;
    }
    for (size_t i = 0; i < count && g_note_count < VAULT_MAX_NOTES; i++) {
        /* Zeroed, not just declared: load_note fills `out` from storage, and a
         * backend that reports success while writing fewer bytes than a whole
         * note_t (ESP-IDF's nvs_get_blob does exactly that for a blob shorter
         * than the buffer) would otherwise leave the tail of this struct as
         * whatever was last on the stack. parent_count lives in that tail and
         * is a loop bound. */
        note_t n = {0};
        if (storage->load_note && storage->load_note(ids[i], &n, storage->ctx)) {
            /* Nothing downstream re-checks this: new_note() bounds
             * parent_count on the creation path, but a note arriving from
             * storage has never been through it. dispatcher.c serializes
             * parent_ids[0 .. parent_count), so a count past the end of the
             * array walks into the next note in g_notes -- whose first fields
             * are id then secret -- and puts it on the wire. Lineage is
             * explicitly informational (see parse_parent_ids), so clamp it
             * and keep the note rather than hiding real value over bad
             * metadata. */
            if (n.parent_count > VAULT_MAX_PARENTS) {
                n.parent_count = VAULT_MAX_PARENTS;
            }
            g_notes[g_note_count++] = n;
        } else if (g_unloaded_count < VAULT_MAX_NOTES) {
            /* Remember it rather than forgetting it: persist_index() carries
             * it through so a transient read failure cannot orphan the note. */
            memcpy(g_unloaded_ids[g_unloaded_count++], ids[i], VAULT_ID_BUF);
        }
    }
}

bool vault_index_known(void) {
    return g_index_known;
}

static vault_err_t new_note(vault_rng_fn rng, const char parent_ids[][VAULT_ID_BUF],
                             size_t parent_count, const char *label, const char *reserved_id,
                             note_t *out) {
    if (!rng || parent_count > VAULT_MAX_PARENTS) {
        return VAULT_ERR_BAD_REQUEST;
    }
    /* persist_index() will refuse to write while the index is unknown, so a
     * note created now would never be referenced again after a reboot.
     * Refusing is the honest answer: better the host is told storage_full
     * than handed an id for a note that quietly will not survive. */
    if (!g_index_known) {
        return VAULT_ERR_STORAGE_FULL;
    }
    if (g_note_count >= VAULT_MAX_NOTES) {
        return VAULT_ERR_STORAGE_FULL;
    }

    memset(out, 0, sizeof(*out));
    if (!gen_unique_id(rng, out->id, reserved_id)) {
        return VAULT_ERR_STORAGE_FULL;
    }
    if (!rng(out->secret, VAULT_SECRET_LEN)) {
        return VAULT_ERR_BAD_REQUEST;
    }
    out->state = NOTE_STATE_PENDING;
    if (label) {
        copy_trunc(out->label, label, VAULT_LABEL_BUF);
    }
    out->parent_count = (uint8_t)parent_count;
    for (size_t i = 0; i < parent_count; i++) {
        copy_trunc(out->parent_ids[i], parent_ids[i], VAULT_ID_BUF);
    }
    out->created_at = out->updated_at = g_now ? g_now() : 0;
    return VAULT_OK;
}

vault_err_t vault_new_secret(vault_rng_fn rng, const char parent_ids[][VAULT_ID_BUF],
                              size_t parent_count, const char *label, char out_id[VAULT_ID_BUF],
                              char out_h_hex[VAULT_HASH_HEX_BUF]) {
    note_t n;
    vault_err_t err = new_note(rng, parent_ids, parent_count, label, NULL, &n);
    if (err != VAULT_OK) {
        return err;
    }

    uint8_t h[32];
    sha256(n.secret, VAULT_SECRET_LEN, h);
    hex_encode(h, sizeof(h), out_h_hex, VAULT_HASH_HEX_BUF);
    copy_trunc(out_id, n.id, VAULT_ID_BUF);

    g_notes[g_note_count++] = n;
    persist_note(&n);
    persist_index();
    return VAULT_OK;
}

vault_err_t vault_new_secret_pair(vault_rng_fn rng, const char parent_ids[][VAULT_ID_BUF],
                                   size_t parent_count, const char *label,
                                   char out_id[VAULT_ID_BUF], char out_h_hex[VAULT_HASH_HEX_BUF],
                                   char out_id2[VAULT_ID_BUF],
                                   char out_h2_hex[VAULT_HASH_HEX_BUF]) {
    if (g_note_count + 1 >= VAULT_MAX_NOTES) {
        return VAULT_ERR_STORAGE_FULL;
    }

    note_t n1, n2;
    vault_err_t err = new_note(rng, parent_ids, parent_count, label, NULL, &n1);
    if (err != VAULT_OK) {
        return err;
    }
    /* n1 is not in g_notes yet, so its id must be reserved explicitly. */
    err = new_note(rng, parent_ids, parent_count, label, n1.id, &n2);
    if (err != VAULT_OK) {
        return err;
    }

    uint8_t h1[32], h2[32];
    sha256(n1.secret, VAULT_SECRET_LEN, h1);
    sha256(n2.secret, VAULT_SECRET_LEN, h2);
    hex_encode(h1, sizeof(h1), out_h_hex, VAULT_HASH_HEX_BUF);
    hex_encode(h2, sizeof(h2), out_h2_hex, VAULT_HASH_HEX_BUF);
    copy_trunc(out_id, n1.id, VAULT_ID_BUF);
    copy_trunc(out_id2, n2.id, VAULT_ID_BUF);

    g_notes[g_note_count++] = n1;
    g_notes[g_note_count++] = n2;
    persist_note(&n1);
    persist_note(&n2);
    persist_index();
    return VAULT_OK;
}

vault_err_t vault_confirm(const char *id, uint64_t amount_msat, const char *host,
                           const char *sig) {
    int idx = find_index(id);
    if (idx < 0) {
        return VAULT_ERR_NOT_FOUND;
    }
    if (g_notes[idx].state != NOTE_STATE_PENDING) {
        return VAULT_ERR_INVALID_STATE;
    }
    g_notes[idx].state = NOTE_STATE_CONFIRMED;
    g_notes[idx].amount_msat = amount_msat;
    if (host) {
        copy_trunc(g_notes[idx].host, host, VAULT_HOST_BUF);
    }
    if (sig) {
        copy_trunc(g_notes[idx].sig, sig, VAULT_SIG_BUF);
    } else {
        g_notes[idx].sig[0] = '\0';
    }
    g_notes[idx].updated_at = g_now ? g_now() : 0;
    persist_note(&g_notes[idx]);
    return VAULT_OK;
}

vault_err_t vault_discard(const char *id) {
    int idx = find_index(id);
    if (idx < 0) {
        return VAULT_ERR_NOT_FOUND;
    }
    if (g_notes[idx].state != NOTE_STATE_PENDING) {
        return VAULT_ERR_INVALID_STATE;
    }
    remove_at((size_t)idx);
    return VAULT_OK;
}

vault_err_t vault_export_secret(const char *id, char out_hex[VAULT_SECRET_HEX_BUF]) {
    int idx = find_index(id);
    if (idx < 0) {
        return VAULT_ERR_NOT_FOUND;
    }
    if (g_notes[idx].state != NOTE_STATE_CONFIRMED) {
        return VAULT_ERR_INVALID_STATE;
    }
    hex_encode(g_notes[idx].secret, VAULT_SECRET_LEN, out_hex, VAULT_SECRET_HEX_BUF);
    return VAULT_OK;
}

vault_err_t vault_import_secret(vault_rng_fn rng, const char *k1_hex, const char *host,
                                 uint64_t amount_msat, const char *label,
                                 char out_id[VAULT_ID_BUF]) {
    if (!rng || !k1_hex || strlen(k1_hex) != VAULT_SECRET_LEN * 2) {
        return VAULT_ERR_BAD_REQUEST;
    }
    uint8_t secret[VAULT_SECRET_LEN];
    if (!hex_decode(k1_hex, VAULT_SECRET_LEN * 2, secret, sizeof(secret))) {
        return VAULT_ERR_BAD_REQUEST;
    }
    if (!g_index_known) { /* see new_note(); import is the other creation path */
        return VAULT_ERR_STORAGE_FULL;
    }
    if (g_note_count >= VAULT_MAX_NOTES) {
        return VAULT_ERR_STORAGE_FULL;
    }

    /* A bearer note IS its secret, so the vault cannot hold the same one
     * twice -- two entries backed by one secret would report double the
     * value actually held, and spending either would leave the other looking
     * spendable while the mint has already paid it out.
     *
     * This is reached by ordinary retry, not by an attack: import_secret has
     * no idempotency key, so a response lost after the note was committed
     * (a BLE disconnect between the write and the reply) leaves the host
     * with no way to know it landed. Answering with the existing note's id
     * makes the retry return what the first call would have.
     *
     * Nothing about the existing note is updated, deliberately. Letting a
     * re-import rewrite amount_msat or host would give the wire a way to
     * change what the approval screen says about a note already held. First
     * import wins; a duplicate is told which note it already is. */
    for (size_t i = 0; i < g_note_count; i++) {
        if (memcmp(g_notes[i].secret, secret, VAULT_SECRET_LEN) == 0) {
            copy_trunc(out_id, g_notes[i].id, VAULT_ID_BUF);
            return VAULT_OK;
        }
    }

    note_t n;
    memset(&n, 0, sizeof(n));
    if (!gen_unique_id(rng, n.id, NULL)) {
        return VAULT_ERR_STORAGE_FULL;
    }
    memcpy(n.secret, secret, VAULT_SECRET_LEN);
    n.state = NOTE_STATE_CONFIRMED;
    n.amount_msat = amount_msat;
    if (host) {
        copy_trunc(n.host, host, VAULT_HOST_BUF);
    }
    if (label) {
        copy_trunc(n.label, label, VAULT_LABEL_BUF);
    }
    n.created_at = n.updated_at = g_now ? g_now() : 0;
    copy_trunc(out_id, n.id, VAULT_ID_BUF);

    g_notes[g_note_count++] = n;
    persist_note(&n);
    persist_index();
    return VAULT_OK;
}

vault_err_t vault_mark_spent(const char *id) {
    int idx = find_index(id);
    if (idx < 0) {
        return VAULT_ERR_NOT_FOUND;
    }
    if (g_notes[idx].state != NOTE_STATE_CONFIRMED) {
        return VAULT_ERR_INVALID_STATE;
    }
    g_notes[idx].state = NOTE_STATE_SPENT;
    g_notes[idx].updated_at = g_now ? g_now() : 0;
    persist_note(&g_notes[idx]);
    return VAULT_OK;
}

vault_err_t vault_rename(const char *id, const char *label) {
    int idx = find_index(id);
    if (idx < 0) {
        return VAULT_ERR_NOT_FOUND;
    }
    copy_trunc(g_notes[idx].label, label, VAULT_LABEL_BUF);
    g_notes[idx].updated_at = g_now ? g_now() : 0;
    persist_note(&g_notes[idx]);
    return VAULT_OK;
}

vault_err_t vault_delete(const char *id) {
    int idx = find_index(id);
    if (idx < 0) {
        return VAULT_ERR_NOT_FOUND;
    }
    if (g_notes[idx].state != NOTE_STATE_SPENT) {
        return VAULT_ERR_INVALID_STATE;
    }
    remove_at((size_t)idx);
    return VAULT_OK;
}

size_t vault_list(note_meta_t *out, size_t max) {
    size_t n = g_note_count < max ? g_note_count : max;
    for (size_t i = 0; i < n; i++) {
        fill_meta(&g_notes[i], &out[i]);
    }
    return n;
}

bool vault_get_meta(const char *id, note_meta_t *out) {
    int idx = find_index(id);
    if (idx < 0) {
        return false;
    }
    fill_meta(&g_notes[idx], out);
    return true;
}

void vault_forget_all(void) {
    /* Through a volatile pointer so this cannot be optimised away as a store
     * to memory that is never read again -- which is precisely what a
     * compiler is entitled to assume here, and precisely the assumption that
     * would leave secrets sitting in RAM after a wipe. */
    volatile unsigned char *p = (volatile unsigned char *)g_notes;
    for (size_t i = 0; i < sizeof(g_notes); i++) {
        p[i] = 0;
    }
    g_note_count = 0;

    volatile unsigned char *q = (volatile unsigned char *)g_unloaded_ids;
    for (size_t i = 0; i < sizeof(g_unloaded_ids); i++) {
        q[i] = 0;
    }
    g_unloaded_count = 0;
}

size_t vault_count(void) {
    return g_note_count;
}

bool vault_get_meta_at(size_t index, note_meta_t *out) {
    if (index >= g_note_count) {
        return false;
    }
    fill_meta(&g_notes[index], out);
    return true;
}

void vault_get_info(size_t *note_count, size_t *pending_count) {
    if (note_count) {
        *note_count = g_note_count;
    }
    if (pending_count) {
        size_t pending = 0;
        for (size_t i = 0; i < g_note_count; i++) {
            if (g_notes[i].state == NOTE_STATE_PENDING) {
                pending++;
            }
        }
        *pending_count = pending;
    }
}
