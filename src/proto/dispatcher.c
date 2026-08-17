#include "dispatcher.h"

#include <string.h>

#include "base64.h"
#include "hex.h"
#include "json.h"
#include "ota_sign.h"
#include "sha256.h"

#ifndef LNURLVAULT_FW_VERSION
#define LNURLVAULT_FW_VERSION "0.0.0-native"
#endif

static dispatcher_deps_t g_deps;

void dispatcher_init(const dispatcher_deps_t *deps) {
    g_deps = *deps;
}

static void write_error(char *out, size_t outcap, const char *code, const char *message) {
    json_writer_t w;
    jw_init(&w, out, outcap);
    jw_begin_obj(&w, NULL);
    jw_bool(&w, "ok", false);
    jw_str(&w, "error", code);
    if (message) {
        jw_str(&w, "message", message);
    }
    jw_end_obj(&w);
}

static const char *vault_err_code(vault_err_t e) {
    switch (e) {
        case VAULT_ERR_NOT_FOUND:
            return "not_found";
        case VAULT_ERR_INVALID_STATE:
            return "invalid_state";
        case VAULT_ERR_STORAGE_FULL:
            return "storage_full";
        case VAULT_ERR_BAD_REQUEST:
            return "bad_request";
        default:
            return "unknown";
    }
}

static void write_vault_error(char *out, size_t outcap, vault_err_t e) {
    write_error(out, outcap, vault_err_code(e), NULL);
}

static void write_ok(char *out, size_t outcap) {
    json_writer_t w;
    jw_init(&w, out, outcap);
    jw_begin_obj(&w, NULL);
    jw_bool(&w, "ok", true);
    jw_end_obj(&w);
}

static const char *note_state_name(note_state_t s) {
    switch (s) {
        case NOTE_STATE_PENDING:
            return "pending";
        case NOTE_STATE_CONFIRMED:
            return "confirmed";
        case NOTE_STATE_SPENT:
            return "spent";
        default:
            return "unknown";
    }
}

static void write_note_obj(json_writer_t *w, const note_meta_t *n) {
    jw_begin_obj(w, NULL);
    jw_str(w, "id", n->id);
    jw_str(w, "state", note_state_name(n->state));
    jw_uint64(w, "amount_msat", n->amount_msat);
    jw_str(w, "label", n->label);
    jw_str(w, "host", n->host);
    if (n->sig[0]) {
        jw_str(w, "sig", n->sig);
    }
    jw_begin_arr(w, "parent_ids");
    for (uint8_t i = 0; i < n->parent_count; i++) {
        jw_str_item(w, n->parent_ids[i]);
    }
    jw_end_arr(w);
    jw_uint64(w, "created_at", n->created_at);
    jw_uint64(w, "updated_at", n->updated_at);
    jw_end_obj(w);
}

/* parent_ids is optional; absent or malformed is treated as "no parents" —
 * it's informational lineage metadata, not security-critical, so we don't
 * fail the whole request over it. */
static size_t parse_parent_ids(const char *line, char out[VAULT_MAX_PARENTS][VAULT_ID_BUF]) {
    size_t count = 0;
    json_get_str_array(line, "parent_ids", &out[0][0], VAULT_ID_BUF, VAULT_MAX_PARENTS, &count);
    return count;
}

static void handle_get_info(char *out, size_t outcap) {
    size_t note_count, pending_count;
    vault_get_info(&note_count, &pending_count);

    json_writer_t w;
    jw_init(&w, out, outcap);
    jw_begin_obj(&w, NULL);
    jw_bool(&w, "ok", true);
    jw_str(&w, "fw_version", LNURLVAULT_FW_VERSION);
    if (g_deps.board) {
        jw_str(&w, "board", g_deps.board);
    }
    jw_uint64(&w, "note_count", note_count);
    jw_uint64(&w, "pending_count", pending_count);
    if (g_deps.free_heap) {
        jw_uint64(&w, "free_heap_bytes", g_deps.free_heap());
    }
    /* Loud about storage it cannot read, rather than presenting as an empty
     * working vault -- see dispatcher.h's storage_state_fn. */
    if (g_deps.storage_state) {
        jw_str(&w, "storage", g_deps.storage_state());
    }
    /* Why the previous boot ended, so a device that resets in the field can
     * be diagnosed over the wire — on a board whose console is deliberately
     * disabled, this is the only channel there is. See src/crash_crumb.h. */
    boot_report_t boot;
    if (g_deps.boot_report && g_deps.boot_report(&boot)) {
        jw_str(&w, "last_reset_reason", boot.reset_reason);
        jw_uint64(&w, "boot_count", boot.boot_count);
        jw_bool(&w, "last_boot_unexpected", boot.unexpected);
        if (boot.last_cmd) {
            jw_str(&w, "last_cmd_in_flight", boot.last_cmd);
        }
    }
    jw_end_obj(&w);
}

static void handle_list_notes(char *out, size_t outcap) {
    json_writer_t w;
    jw_init(&w, out, outcap);
    jw_begin_obj(&w, NULL);
    jw_bool(&w, "ok", true);
    jw_begin_arr(&w, "notes");
    size_t total = vault_count();
    for (size_t i = 0; i < total; i++) {
        note_meta_t n;
        if (vault_get_meta_at(i, &n)) {
            write_note_obj(&w, &n);
        }
    }
    jw_end_arr(&w);
    jw_end_obj(&w);

    /* The only unbounded response we build: every other handler writes a
     * fixed set of fields that cannot overflow a transport buffer. Without
     * this check an overflowing listing went out as a silently truncated
     * string, so the client hit a JSON syntax error with nothing to
     * distinguish "device is broken" from "you have too many notes".
     * write_error() re-initialises the writer over the same buffer, so the
     * partial listing is discarded rather than appended to. */
    if (!jw_ok(&w)) {
        write_error(out, outcap, "response_too_large",
                    "too many notes to return in one response");
    }
}

static void handle_new_secret(const char *line, char *out, size_t outcap) {
    char parent_ids[VAULT_MAX_PARENTS][VAULT_ID_BUF];
    size_t parent_count = parse_parent_ids(line, parent_ids);
    char label_buf[VAULT_LABEL_BUF];
    const char *label = json_get_str(line, "label", label_buf, sizeof(label_buf)) ? label_buf : NULL;

    char id[VAULT_ID_BUF], h[VAULT_HASH_HEX_BUF];
    vault_err_t err = vault_new_secret(g_deps.rng, parent_ids, parent_count, label, id, h);
    if (err != VAULT_OK) {
        write_vault_error(out, outcap, err);
        return;
    }

    json_writer_t w;
    jw_init(&w, out, outcap);
    jw_begin_obj(&w, NULL);
    jw_bool(&w, "ok", true);
    jw_str(&w, "id", id);
    jw_str(&w, "h", h);
    jw_end_obj(&w);
}

static void handle_new_secret_pair(const char *line, char *out, size_t outcap) {
    char parent_ids[VAULT_MAX_PARENTS][VAULT_ID_BUF];
    size_t parent_count = parse_parent_ids(line, parent_ids);
    char label_buf[VAULT_LABEL_BUF];
    const char *label = json_get_str(line, "label", label_buf, sizeof(label_buf)) ? label_buf : NULL;

    char id[VAULT_ID_BUF], h[VAULT_HASH_HEX_BUF];
    char id2[VAULT_ID_BUF], h2[VAULT_HASH_HEX_BUF];
    vault_err_t err =
        vault_new_secret_pair(g_deps.rng, parent_ids, parent_count, label, id, h, id2, h2);
    if (err != VAULT_OK) {
        write_vault_error(out, outcap, err);
        return;
    }

    json_writer_t w;
    jw_init(&w, out, outcap);
    jw_begin_obj(&w, NULL);
    jw_bool(&w, "ok", true);
    jw_str(&w, "id", id);
    jw_str(&w, "h", h);
    jw_str(&w, "id2", id2);
    jw_str(&w, "h2", h2);
    jw_end_obj(&w);
}

static void handle_confirm(const char *line, char *out, size_t outcap) {
    char id[VAULT_ID_BUF];
    char host[VAULT_HOST_BUF];
    char sig_buf[VAULT_SIG_BUF];
    uint64_t amount_msat;

    if (!json_get_str(line, "id", id, sizeof(id)) ||
        !json_get_u64(line, "amount_msat", &amount_msat) ||
        !json_get_str(line, "host", host, sizeof(host))) {
        write_error(out, outcap, "bad_request", "confirm requires id, amount_msat, host");
        return;
    }
    const char *sig = json_get_str(line, "sig", sig_buf, sizeof(sig_buf)) ? sig_buf : NULL;

    vault_err_t err = vault_confirm(id, amount_msat, host, sig);
    if (err != VAULT_OK) {
        write_vault_error(out, outcap, err);
        return;
    }
    write_ok(out, outcap);
}

static void handle_discard(const char *line, char *out, size_t outcap) {
    char id[VAULT_ID_BUF];
    if (!json_get_str(line, "id", id, sizeof(id))) {
        write_error(out, outcap, "bad_request", "discard requires id");
        return;
    }
    vault_err_t err = vault_discard(id);
    if (err != VAULT_OK) {
        write_vault_error(out, outcap, err);
        return;
    }
    write_ok(out, outcap);
}

static void handle_export_secret(const char *line, char *out, size_t outcap) {
    char id[VAULT_ID_BUF];
    if (!json_get_str(line, "id", id, sizeof(id))) {
        write_error(out, outcap, "bad_request", "export_secret requires id");
        return;
    }

    note_meta_t meta;
    if (!vault_get_meta(id, &meta)) {
        write_error(out, outcap, "not_found", NULL);
        return;
    }
    if (meta.state != NOTE_STATE_CONFIRMED) {
        write_error(out, outcap, "invalid_state", NULL);
        return;
    }

    if (g_deps.confirm_export) {
        confirm_result_t result = g_deps.confirm_export(&meta);
        if (result == CONFIRM_NO) {
            write_error(out, outcap, "user_declined", NULL);
            return;
        }
        if (result == CONFIRM_TIMEOUT) {
            write_error(out, outcap, "timeout", NULL);
            return;
        }
    }

    char k1[VAULT_SECRET_HEX_BUF];
    vault_err_t err = vault_export_secret(id, k1);
    if (err != VAULT_OK) {
        write_vault_error(out, outcap, err);
        return;
    }

    json_writer_t w;
    jw_init(&w, out, outcap);
    jw_begin_obj(&w, NULL);
    jw_bool(&w, "ok", true);
    jw_str(&w, "k1", k1);
    jw_end_obj(&w);
}

static void handle_import_secret(const char *line, char *out, size_t outcap) {
    char k1[VAULT_SECRET_HEX_BUF];
    char host[VAULT_HOST_BUF];
    uint64_t amount_msat;
    if (!json_get_str(line, "k1", k1, sizeof(k1)) ||
        !json_get_str(line, "host", host, sizeof(host)) ||
        !json_get_u64(line, "amount_msat", &amount_msat)) {
        write_error(out, outcap, "bad_request", "import_secret requires k1, host, amount_msat");
        return;
    }
    char label_buf[VAULT_LABEL_BUF];
    const char *label = json_get_str(line, "label", label_buf, sizeof(label_buf)) ? label_buf : NULL;

    char id[VAULT_ID_BUF];
    vault_err_t err = vault_import_secret(g_deps.rng, k1, host, amount_msat, label, id);
    if (err != VAULT_OK) {
        write_vault_error(out, outcap, err);
        return;
    }

    json_writer_t w;
    jw_init(&w, out, outcap);
    jw_begin_obj(&w, NULL);
    jw_bool(&w, "ok", true);
    jw_str(&w, "id", id);
    jw_end_obj(&w);
}

static void handle_mark_spent(const char *line, char *out, size_t outcap) {
    char id[VAULT_ID_BUF];
    if (!json_get_str(line, "id", id, sizeof(id))) {
        write_error(out, outcap, "bad_request", "mark_spent requires id");
        return;
    }
    vault_err_t err = vault_mark_spent(id);
    if (err != VAULT_OK) {
        write_vault_error(out, outcap, err);
        return;
    }
    write_ok(out, outcap);
}

static void handle_rename(const char *line, char *out, size_t outcap) {
    char id[VAULT_ID_BUF];
    char label[VAULT_LABEL_BUF];
    if (!json_get_str(line, "id", id, sizeof(id)) ||
        !json_get_str(line, "label", label, sizeof(label))) {
        write_error(out, outcap, "bad_request", "rename requires id, label");
        return;
    }
    vault_err_t err = vault_rename(id, label);
    if (err != VAULT_OK) {
        write_vault_error(out, outcap, err);
        return;
    }
    write_ok(out, outcap);
}

/* Responds first, reboots second — g_deps.reset (when set) is expected to
 * schedule a *delayed* restart, not call esp_restart() synchronously here,
 * so this response actually has a chance to reach the client first. See
 * dispatcher.h's reset_fn comment. */
static void handle_reset(char *out, size_t outcap) {
    write_ok(out, outcap);
    if (g_deps.reset) {
        g_deps.reset();
    }
}

/* --- OTA: see dispatcher.h's ota_*_fn comments and docs/PROTOCOL.md's
 * ota_begin/ota_chunk/ota_finish for the full design. This block owns
 * everything portable: parsing, base64, signature verification, and
 * sequencing; only the ESP-IDF-specific flash writes and the physical
 * approval prompt are injected via g_deps. */

#define OTA_MAX_SIZE (2 * 1024 * 1024) /* must match partitions.csv's ota_0/ota_1 slot size */
#define OTA_CHUNK_MAX_RAW 1024         /* the protocol limit a chunk's DECODED length must fit — checked
                                         * against the actual decode result, not an estimate (see below) */
#define OTA_CHUNK_B64_BUF 1400         /* text buffer for the base64 "data" field; > base64_encoded_len(OTA_CHUNK_MAX_RAW)+1 */
/* base64_decoded_len() is a naive upper-bound formula ((len/4)*3) that
 * doesn't subtract padding, so it can overestimate a real decode by up to
 * 2 bytes — sizing the decode buffer to it directly and rejecting anything
 * bigger, BEFORE decoding, would wrongly reject a legitimate
 * OTA_CHUNK_MAX_RAW-byte chunk whose base64 happens to be padded. Instead
 * this decode buffer is sized to the worst case that could ever fit in
 * OTA_CHUNK_B64_BUF, and OTA_CHUNK_MAX_RAW is enforced afterward against
 * the real decoded length. */
#define OTA_CHUNK_DECODE_BUF 1050
#define OTA_APPROVE_TIMEOUT_MS 30000 /* matches export_secret's confirm window */
#define WIPE_APPROVE_TIMEOUT_MS 30000 /* likewise */

typedef struct {
    bool active;
    uint32_t total_size;
    uint32_t bytes_received;
    uint8_t signature[OTA_SIGNATURE_LEN];
    sha256_ctx_t hasher;
} ota_session_t;

static ota_session_t g_ota;

/* Never touches the boot partition — the currently running firmware is
 * unaffected. Safe to call whether or not a session is active. */
static void ota_abort_session(void) {
    if (g_ota.active && g_deps.ota_write_abort) {
        g_deps.ota_write_abort();
    }
    g_ota.active = false;
}

static void handle_ota_begin(const char *line, char *out, size_t outcap) {
    /* A new ota_begin implicitly discards any abandoned prior session
     * (e.g. a host that crashed or gave up mid-transfer) rather than
     * requiring a full device reset just to retry. */
    ota_abort_session();

    uint64_t size_u64;
    char sha_hex[65], sig_hex[129];
    if (!json_get_u64(line, "size", &size_u64) ||
        !json_get_str(line, "sha256", sha_hex, sizeof(sha_hex)) || strlen(sha_hex) != 64 ||
        !json_get_str(line, "signature", sig_hex, sizeof(sig_hex)) || strlen(sig_hex) != 128) {
        write_error(out, outcap, "bad_request", "ota_begin requires size, sha256 (64 hex), signature (128 hex)");
        return;
    }
    if (size_u64 == 0 || size_u64 > OTA_MAX_SIZE) {
        write_error(out, outcap, "bad_request", "size out of range");
        return;
    }

    uint8_t digest[OTA_DIGEST_LEN], signature[OTA_SIGNATURE_LEN];
    if (!hex_decode(sha_hex, 64, digest, sizeof(digest)) ||
        !hex_decode(sig_hex, 128, signature, sizeof(signature))) {
        write_error(out, outcap, "bad_request", "sha256/signature must be hex");
        return;
    }

    /* Verified over the CLAIMED digest, before the owner is bothered at
     * all — an unsigned or wrongly-signed image is refused up front, same
     * order as heartwood-esp32's OTA_BEGIN. ota_finish re-verifies this
     * same signature against the digest actually written to flash. */
    if (!g_deps.ota_pubkey || !ota_verify_signature(g_deps.ota_pubkey, digest, signature)) {
        write_error(out, outcap, "bad_signature", NULL);
        return;
    }

    if (g_deps.ota_approve) {
        confirm_result_t result = g_deps.ota_approve((uint32_t)size_u64, OTA_APPROVE_TIMEOUT_MS);
        if (result == CONFIRM_NO) {
            write_error(out, outcap, "user_declined", NULL);
            return;
        }
        if (result == CONFIRM_TIMEOUT) {
            write_error(out, outcap, "timeout", NULL);
            return;
        }
    }

    if (g_deps.ota_write_begin && !g_deps.ota_write_begin((uint32_t)size_u64)) {
        write_error(out, outcap, "ota_failed", "could not open the OTA partition");
        return;
    }

    g_ota.active = true;
    g_ota.total_size = (uint32_t)size_u64;
    g_ota.bytes_received = 0;
    memcpy(g_ota.signature, signature, sizeof(signature));
    sha256_init(&g_ota.hasher);

    write_ok(out, outcap);
}

static void handle_ota_chunk(const char *line, char *out, size_t outcap) {
    if (!g_ota.active) {
        write_error(out, outcap, "invalid_state", "no active OTA session — call ota_begin first");
        return;
    }

    uint64_t offset_u64;
    char data_b64[OTA_CHUNK_B64_BUF];
    if (!json_get_u64(line, "offset", &offset_u64) || !json_get_str(line, "data", data_b64, sizeof(data_b64))) {
        write_error(out, outcap, "bad_request", "ota_chunk requires offset, data");
        return;
    }
    /* Chunks must arrive strictly in order — no reassembly buffer, no
     * random access. A host that needs to retry a stalled chunk just
     * resends the same (already-next-expected) offset; anything else gets
     * a clear bad_request instead of silently corrupting the image. */
    if (offset_u64 != g_ota.bytes_received) {
        write_error(out, outcap, "bad_request", "offset does not match the next expected byte");
        return;
    }

    size_t b64_len = strlen(data_b64);
    uint8_t chunk_raw[OTA_CHUNK_DECODE_BUF];
    if (base64_decoded_len(b64_len) > sizeof(chunk_raw)) {
        /* Should be unreachable — data_b64's own size already bounds this
         * — but never decode into a buffer that might not fit. */
        write_error(out, outcap, "bad_request", "data too long");
        return;
    }
    size_t chunk_len = 0;
    if (!base64_decode(data_b64, b64_len, chunk_raw, &chunk_len)) {
        write_error(out, outcap, "bad_request", "malformed base64 in data");
        return;
    }
    if (chunk_len > OTA_CHUNK_MAX_RAW) {
        write_error(out, outcap, "bad_request", "chunk too large");
        return;
    }
    if ((uint64_t)g_ota.bytes_received + chunk_len > g_ota.total_size) {
        write_error(out, outcap, "bad_request", "chunk would exceed the declared size");
        ota_abort_session();
        return;
    }

    if (g_deps.ota_write_chunk && !g_deps.ota_write_chunk(chunk_raw, chunk_len)) {
        write_error(out, outcap, "ota_failed", "flash write failed");
        ota_abort_session();
        return;
    }
    sha256_update(&g_ota.hasher, chunk_raw, chunk_len);
    g_ota.bytes_received += (uint32_t)chunk_len;

    write_ok(out, outcap);
}

static void handle_ota_finish(char *out, size_t outcap) {
    if (!g_ota.active) {
        write_error(out, outcap, "invalid_state", "no active OTA session — call ota_begin first");
        return;
    }
    if (g_ota.bytes_received != g_ota.total_size) {
        write_error(out, outcap, "bad_request", "fewer bytes received than declared");
        ota_abort_session();
        return;
    }

    /* The check that actually carries the security guarantee: re-verify
     * the signature against the digest of the bytes truly written to
     * flash, not just the claimed one from ota_begin. */
    uint8_t digest[OTA_DIGEST_LEN];
    sha256_final(&g_ota.hasher, digest);
    if (!g_deps.ota_pubkey || !ota_verify_signature(g_deps.ota_pubkey, digest, g_ota.signature)) {
        write_error(out, outcap, "bad_signature", "written bytes do not match the signed digest");
        ota_abort_session();
        return;
    }

    if (g_deps.ota_write_finish && !g_deps.ota_write_finish()) {
        write_error(out, outcap, "ota_failed", "could not finalize the update");
        ota_abort_session();
        return;
    }

    g_ota.active = false;
    write_ok(out, outcap);
}

/* Erases everything. The only thing in this firmware that does.
 *
 * Three gates, none of which is redundant:
 *
 *  1. An explicit {"confirm":"WIPE"} in the request. Not security -- the
 *     physical press below is that -- but intent. `wipe` sits in the same
 *     command namespace as `get_info`, reachable by anything already paired,
 *     and a bare {"cmd":"wipe"} is far too easy to emit by accident from a
 *     retry loop, a fuzzer or a mistyped script.
 *  2. A physical confirmation, which is the actual control, and which is
 *     refused rather than skipped when no confirm hook is wired: an
 *     unconfirmable wipe is not one to grant.
 *  3. Verification, inside the wipe itself. Erasing is not the hard part;
 *     being able to prove it worked is. A wipe that claims a success it
 *     cannot demonstrate is worse than one that admits failure, because the
 *     owner acts on the claim.
 *
 * RAM is cleared only after storage has been erased AND verified, in that
 * order: until the verification passes, RAM holds the only intact copy of
 * the notes, and throwing it away first would turn a failed wipe into a
 * successful one. */
static void handle_wipe(const char *line, char *out, size_t outcap) {
    if (!g_deps.wipe_storage) {
        write_error(out, outcap, "unsupported", "this build has no persistent storage to wipe");
        return;
    }

    char confirm[16];
    if (!json_get_str(line, "confirm", confirm, sizeof(confirm)) ||
        strcmp(confirm, "WIPE") != 0) {
        write_error(out, outcap, "bad_request",
                    "wipe requires \"confirm\":\"WIPE\" -- this erases every note");
        return;
    }

    if (!g_deps.wipe_approve) {
        write_error(out, outcap, "unsupported",
                    "no on-device confirmation available; refusing to wipe");
        return;
    }

    confirm_result_t approved = g_deps.wipe_approve(WIPE_APPROVE_TIMEOUT_MS);
    if (approved == CONFIRM_NO) {
        write_error(out, outcap, "user_declined", NULL);
        return;
    }
    if (approved == CONFIRM_TIMEOUT) {
        write_error(out, outcap, "timeout", NULL);
        return;
    }

    if (!g_deps.wipe_storage()) {
        /* Deliberately does not reboot and does not claim success. The device
         * is left holding whatever survived, which the owner can still read,
         * rather than rebooted into an unknown state on the strength of an
         * unverified erase. */
        write_error(out, outcap, "wipe_failed",
                    "storage could not be erased and verified; nothing has been reported as gone");
        return;
    }

    vault_forget_all();

    json_writer_t w;
    jw_init(&w, out, outcap);
    jw_begin_obj(&w, NULL);
    jw_bool(&w, "ok", true);
    jw_bool(&w, "wiped", true);
    jw_end_obj(&w);

    /* Reboot last, and delayed, so this response leaves first -- same
     * reasoning as handle_reset(). A fresh boot after a wipe means nothing
     * derived from the old contents survives anywhere. */
    if (g_deps.reset) {
        g_deps.reset();
    }
}

static void handle_delete(const char *line, char *out, size_t outcap) {
    char id[VAULT_ID_BUF];
    if (!json_get_str(line, "id", id, sizeof(id))) {
        write_error(out, outcap, "bad_request", "delete requires id");
        return;
    }
    vault_err_t err = vault_delete(id);
    if (err != VAULT_OK) {
        write_vault_error(out, outcap, err);
        return;
    }
    write_ok(out, outcap);
}

void dispatcher_handle(const char *line, char *out, size_t outcap) {
    char cmd[32];
    bool found_cmd = json_get_str(line, "cmd", cmd, sizeof(cmd));
    if (!found_cmd) {
        write_error(out, outcap, "bad_request", "missing or invalid cmd");
        return;
    }

    /* The name only — never `line`, which for import_secret contains a raw
     * secret and for everything else is longer than a breadcrumb should be. */
    if (g_deps.trace_cmd) {
        g_deps.trace_cmd(cmd);
    }

    if (strcmp(cmd, "get_info") == 0) {
        handle_get_info(out, outcap);
    } else if (strcmp(cmd, "list_notes") == 0) {
        handle_list_notes(out, outcap);
    } else if (strcmp(cmd, "new_secret") == 0) {
        handle_new_secret(line, out, outcap);
    } else if (strcmp(cmd, "new_secret_pair") == 0) {
        handle_new_secret_pair(line, out, outcap);
    } else if (strcmp(cmd, "confirm") == 0) {
        handle_confirm(line, out, outcap);
    } else if (strcmp(cmd, "discard") == 0) {
        handle_discard(line, out, outcap);
    } else if (strcmp(cmd, "export_secret") == 0) {
        handle_export_secret(line, out, outcap);
    } else if (strcmp(cmd, "import_secret") == 0) {
        handle_import_secret(line, out, outcap);
    } else if (strcmp(cmd, "mark_spent") == 0) {
        handle_mark_spent(line, out, outcap);
    } else if (strcmp(cmd, "rename") == 0) {
        handle_rename(line, out, outcap);
    } else if (strcmp(cmd, "wipe") == 0) {
        handle_wipe(line, out, outcap);
    } else if (strcmp(cmd, "delete") == 0) {
        handle_delete(line, out, outcap);
    } else if (strcmp(cmd, "reset") == 0) {
        handle_reset(out, outcap);
    } else if (strcmp(cmd, "ota_begin") == 0) {
        handle_ota_begin(line, out, outcap);
    } else if (strcmp(cmd, "ota_chunk") == 0) {
        handle_ota_chunk(line, out, outcap);
    } else if (strcmp(cmd, "ota_finish") == 0) {
        handle_ota_finish(out, outcap);
    } else {
        write_error(out, outcap, "bad_request", "unknown cmd");
    }

    /* Reached only if the command returned. A breadcrumb still set at the
     * next boot therefore names a command that did not. */
    if (g_deps.trace_cmd) {
        g_deps.trace_cmd(NULL);
    }
}
