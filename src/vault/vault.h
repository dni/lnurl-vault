#ifndef LNURLVAULT_VAULT_H
#define LNURLVAULT_VAULT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The note ledger: generates LNURLcash bearer-note secrets from a hardware
 * RNG, discloses only their SHA-256 hash until the mint confirms a mutation
 * (rotate/split/merge) succeeded, and tracks each note through
 * PENDING -> CONFIRMED -> SPENT. See docs/PROTOCOL.md for how this maps onto
 * the wire commands, and ../luds/25.md for the LNURLcash spec this mirrors.
 *
 * This module has no ESP-IDF dependency: persistence and randomness are
 * injected (vault_storage_t, vault_rng_fn) so it compiles and runs
 * identically in the native test binary (test/native/) and in the firmware,
 * where main.c wires it to NVS (vault/nvs_storage.c) and esp_fill_random. */

#define VAULT_ID_BUF 9      /* 8 hex chars + NUL */
#define VAULT_SECRET_LEN 32 /* raw bytes */
#define VAULT_HASH_HEX_BUF 65 /* 64 hex chars + NUL */
#define VAULT_SECRET_HEX_BUF 65
#define VAULT_HOST_BUF 64
#define VAULT_LABEL_BUF 32
#define VAULT_SIG_BUF 132
#define VAULT_MAX_PARENTS 16
#define VAULT_MAX_NOTES 128

typedef enum {
    NOTE_STATE_PENDING = 0,   /* secret generated, hash disclosed, awaiting mint confirmation */
    NOTE_STATE_CONFIRMED = 1, /* mint accepted the mutation; a real, spendable note */
    NOTE_STATE_SPENT = 2,     /* melted, or burned as the input to a rotate/split/merge */
} note_state_t;

typedef struct {
    char id[VAULT_ID_BUF];
    uint8_t secret[VAULT_SECRET_LEN];
    char host[VAULT_HOST_BUF];
    uint64_t amount_msat;
    char label[VAULT_LABEL_BUF];
    char sig[VAULT_SIG_BUF]; /* optional LUD-25 offline-verification signature, hex */
    note_state_t state;
    char parent_ids[VAULT_MAX_PARENTS][VAULT_ID_BUF];
    uint8_t parent_count;
    uint32_t created_at;
    uint32_t updated_at;
} note_t;

/* Metadata view of a note — never carries the secret. This, not note_t, is
 * what list_notes/get commands are allowed to serialize onto the wire. */
typedef struct {
    char id[VAULT_ID_BUF];
    char h[VAULT_HASH_HEX_BUF]; /* sha256(secret), safe metadata for recovery/matching */
    note_state_t state;
    uint64_t amount_msat;
    char label[VAULT_LABEL_BUF];
    char host[VAULT_HOST_BUF];
    char sig[VAULT_SIG_BUF];
    char parent_ids[VAULT_MAX_PARENTS][VAULT_ID_BUF];
    uint8_t parent_count;
    uint32_t created_at;
    uint32_t updated_at;
} note_meta_t;

typedef enum {
    VAULT_OK = 0,
    VAULT_ERR_NOT_FOUND,
    VAULT_ERR_INVALID_STATE,
    VAULT_ERR_STORAGE_FULL,
    VAULT_ERR_BAD_REQUEST,
} vault_err_t;

/* Fills `len` bytes at `out` with cryptographically random data; returns
 * false on failure. Firmware wires this to esp_fill_random (see
 * README.md's security posture section on the ESP32-S3 TRNG); native tests
 * use a small deterministic/PRNG stand-in. */
typedef bool (*vault_rng_fn)(uint8_t *out, size_t len);

/* Seconds since some fixed epoch (boot time is fine — these timestamps are
 * informational, never authoritative; the mint's own state is). */
typedef uint32_t (*vault_time_fn)(void);

typedef struct {
    bool (*load_index)(char ids[][VAULT_ID_BUF], size_t max, size_t *count, void *ctx);
    bool (*save_index)(const char ids[][VAULT_ID_BUF], size_t count, void *ctx);
    bool (*load_note)(const char *id, note_t *out, void *ctx);
    bool (*save_note)(const note_t *note, void *ctx);
    bool (*delete_note)(const char *id, void *ctx);
    void *ctx;
} vault_storage_t;

/* Loads all persisted notes into RAM. `storage` and `now_fn` may be NULL
 * (storage NULL => starts empty and every mutation is in-RAM-only, useful
 * for tests that don't care about persistence; now_fn NULL => timestamps
 * are always 0). */
void vault_init(const vault_storage_t *storage, vault_time_fn now_fn);

/* Fail-closed init for firmware whose storage was expected but did not come
 * up: the vault refuses to create notes (they could not be persisted) and
 * reports the index as unknown. NOT the same as vault_init(NULL, ...), which
 * is the in-RAM-by-design mode the native tests use. See vault_index_known(). */
void vault_init_storage_unavailable(vault_time_fn now_fn);

/* Generates one fresh secret (rotate/merge). Stores it PENDING and returns
 * its id plus the hex SHA-256 hash to disclose to the mint as `h`. */
vault_err_t vault_new_secret(vault_rng_fn rng, const char parent_ids[][VAULT_ID_BUF],
                              size_t parent_count, const char *label,
                              char out_id[VAULT_ID_BUF], char out_h_hex[VAULT_HASH_HEX_BUF]);

/* Generates two fresh secrets sharing the same parent lineage (split). */
vault_err_t vault_new_secret_pair(vault_rng_fn rng, const char parent_ids[][VAULT_ID_BUF],
                                   size_t parent_count, const char *label,
                                   char out_id[VAULT_ID_BUF], char out_h_hex[VAULT_HASH_HEX_BUF],
                                   char out_id2[VAULT_ID_BUF],
                                   char out_h2_hex[VAULT_HASH_HEX_BUF]);

/* PENDING -> CONFIRMED once the mint replied {"status":"OK"}. `sig` may be
 * NULL (mint doesn't support offline verification). */
vault_err_t vault_confirm(const char *id, uint64_t amount_msat, const char *host,
                           const char *sig);

/* Drops a PENDING note the mint rejected. */
vault_err_t vault_discard(const char *id);

/* Reveals a CONFIRMED note's raw secret as hex. Callers (dispatcher.c on
 * firmware) MUST gate this behind an on-device physical confirmation before
 * calling it — vault.c itself is UI-agnostic and enforces no such gate, only
 * the state check (a note must actually be spendable to export). */
vault_err_t vault_export_secret(const char *id, char out_hex[VAULT_SECRET_HEX_BUF]);

/* Registers an externally-known secret (a received note, or a fresh mint
 * payment preimage) directly as CONFIRMED.
 *
 * Idempotent on the secret. Importing one the vault already holds returns
 * VAULT_OK with the existing note's id in out_id and creates nothing -- a
 * note is its secret, so holding it twice would report double the value and
 * leave one copy looking spendable after the other was spent. The existing
 * note is not modified, so a re-import cannot restate its amount or host.
 * Callers should therefore treat out_id as "the note for this secret", not
 * "the note I just created", and read its state if that matters (importing
 * an already-SPENT secret returns that note, still SPENT). */
vault_err_t vault_import_secret(vault_rng_fn rng, const char *k1_hex, const char *host,
                                 uint64_t amount_msat, const char *label,
                                 char out_id[VAULT_ID_BUF]);

/* CONFIRMED -> SPENT, once the PWA confirms a melt settled or the note was
 * burned as a rotate/split/merge input. */
vault_err_t vault_mark_spent(const char *id);

vault_err_t vault_rename(const char *id, const char *label);

/* Housekeeping: only a SPENT note may be deleted (a PENDING note is dropped
 * via vault_discard; a CONFIRMED one must be spent first). */
vault_err_t vault_delete(const char *id);

/* How many notes are in NOTE_STATE_SPENT right now.
 *
 * Not an estimate and not a guess about the mint: a note reaches SPENT only
 * because something told this vault so -- a melt the host watched settle, or
 * a rotate/split/merge that burned it as an input. Those notes are dead by
 * the vault's own record and nothing can bring them back. */
size_t vault_count_spent(void);

/* Forgets every SPENT note in one pass, writing how many went to
 * `out_removed` (which may be NULL).
 *
 * This exists because vault_delete() takes one id, and every gated command
 * costs a physical hold on the device -- so clearing the dead weight of a few
 * dozen spent notes meant a few dozen deliberate two-second holds, which is
 * not housekeeping anybody does. It is the same removal, done once.
 *
 * Touches NOTHING else. A PENDING note may yet confirm and a CONFIRMED one is
 * money; neither is this function's business, and the loop below is written
 * so that it cannot become so by accident.
 *
 * Notes the index named but whose blobs would not load are left alone too.
 * They are not known-spent, they are unreadable, and they live outside
 * g_notes precisely so that operations like this one cannot quietly drop
 * them -- see g_unloaded_ids.
 *
 * One index rewrite for the whole pass rather than one per note: calling
 * vault_delete() in a loop would rewrite the persisted index N times and
 * leave N chances to be interrupted half-done.
 *
 * Returns VAULT_ERR_STORAGE_FULL if the removal did not stick on flash. The
 * notes are gone from RAM either way -- same contract as vault_delete() --
 * so the caller reports the failure rather than claiming a clean sweep that a
 * reboot would undo. */
vault_err_t vault_prune_spent(size_t *out_removed);

size_t vault_list(note_meta_t *out, size_t max);
bool vault_get_meta(const char *id, note_meta_t *out);
void vault_get_info(size_t *note_count, size_t *pending_count);

/* False when this boot could not read the persisted note index, so the vault
 * is refusing to create notes and refusing to rewrite the index (either would
 * risk overwriting a list it cannot see). Distinct from a vault that is
 * genuinely empty, and NOT the same as the NVS-level state get_info reports
 * from vault_nvs_state_name(): NVS itself can come up perfectly while this
 * one read fails, in which case storage looks "ok" and the vault looks empty
 * and neither is the useful truth. Callers reporting device state should ask
 * this too. Recovery is a reboot, never a wipe. */
bool vault_index_known(void);

/* Index-based accessors so callers (dispatcher.c in particular, streaming
 * list_notes straight onto the wire) can iterate one note at a time instead
 * of materializing a VAULT_MAX_NOTES-sized array — that's ~50KB, too much
 * for an embedded task stack. */
size_t vault_count(void);

/* Forgets every note held in RAM, overwriting each secret rather than merely
 * dropping the count -- freed-but-not-cleared RAM holding bearer secrets is
 * exactly what a wipe is supposed to remove.
 *
 * Storage is NOT touched: the caller is expected to have already erased and
 * verified it (see nvs_storage.h's vault_nvs_wipe). The order matters and is
 * the caller's responsibility -- erase flash first, and forget RAM only once
 * that has been verified, because until it has, RAM is the only intact copy
 * of the notes. */
void vault_forget_all(void);

/* True when no byte of any note secret remains set in RAM. Exists so a wipe
 * can be VERIFIED rather than trusted, which is the rule the flash half of
 * the wipe already follows: nvs_storage.c re-initialises and re-reads after
 * erasing, and reports failure rather than success if anything is still
 * readable, "because the owner acts on the claim". The RAM half asserted the
 * same thing and checked nothing.
 *
 * Reading the bytes back is also what makes the clearing observable. The
 * volatile stores in vault_forget_all() exist because a compiler is entitled
 * to drop stores to memory that is never read again; a caller that actually
 * reads it removes that entitlement, so this guards the property even if the
 * volatile qualifier is ever lost. */
bool vault_secrets_cleared(void);
bool vault_get_meta_at(size_t index, note_meta_t *out);

#endif
