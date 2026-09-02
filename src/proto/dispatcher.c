#include "dispatcher.h"

#include "identity.h"
#include "monocypher.h"

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
static dispatch_source_t g_source = DISPATCH_SOURCE_LOCAL;

void dispatcher_init(const dispatcher_deps_t *deps) {
    g_deps = *deps;
}

void dispatcher_set_source(dispatch_source_t source) {
    g_source = source;
}

/* Closes a response and, if it did not fit, replaces it with an explicit
 * error instead of shipping a truncated object.
 *
 * Issue #7: handlers used to call jw_end_obj() and return whatever was in the
 * buffer. The writer tracked overflow correctly and nobody asked it. A client
 * then received a string that stopped mid-object with no error field, so a
 * vault that filled up simply stopped being readable and the failure was
 * silent -- the worst shape for a device holding money. Every response now
 * goes through here, including the ones whose fields are fixed-size and
 * cannot overflow today, because "cannot overflow today" is a property that
 * quietly stops being true when someone adds a field. */
static void write_error(char *out, size_t outcap, const char *code, const char *message);

/* The client's `tag`, echoed on whatever reply this command gets -- see
 * docs/PROTOCOL.md's "Transports" section for what it buys a client. Set at
 * the top of dispatcher_handle() for the whole of one command, which
 * cmd_lock already makes the only one in flight. Empty means the command
 * carried none, and then nothing is echoed: a client that never sends tags
 * sees the wire exactly as before. */
#define TAG_MAX_LEN 32
static char g_tag[TAG_MAX_LEN + 1];

/* Every top-level reply closes through here, so the tag lands on all of
 * them alike: a success, an error, and each page of a listing. A client
 * that cannot find its tag on a line has not been answered by that line. */
static void end_response(json_writer_t *w) {
    if (g_tag[0]) {
        jw_str(w, "tag", g_tag);
    }
    jw_end_obj(w);
}

static void finish(json_writer_t *w, char *out, size_t outcap) {
    end_response(w);
    if (!jw_ok(w)) {
        /* Re-initialises the writer over the same buffer, so the partial
         * response is discarded rather than appended to. */
        write_error(out, outcap, "response_too_large", NULL);
    }
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
    end_response(&w);
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

/* The one place a confirm result becomes a wire error. Exhaustive on purpose:
 * every caller asks "is it CONFIRM_YES" and routes everything else through
 * here, so a result this does not name is reported as a refusal rather than
 * silently passing for approval. */
static const char *confirm_error_code(confirm_result_t r) {
    switch (r) {
        case CONFIRM_NO:
            return "user_declined";
        case CONFIRM_TIMEOUT:
            return "timeout";
        case CONFIRM_UNAVAILABLE:
            return "display_unavailable";
        case CONFIRM_YES:
        default:
            /* Not reachable from the call sites, which check for YES first.
             * Naming it refused is the safe answer if it ever is. */
            return "user_declined";
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
    finish(&w, out, outcap);
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
    jw_str(w, "h", n->h);
    jw_str(w, "state", note_state_name(n->state));
    jw_uint64(w, "amount_msat", n->amount_msat);
    jw_str(w, "label", n->label);
    jw_str(w, "host", n->host);
    if (n->sig[0]) {
        jw_str(w, "sig", n->sig);
    }
    jw_begin_arr(w, "parent_ids");
    /* parent_count bounds a read of a fixed-size array straight onto the
     * wire, so it is clamped at the point of use as well as on load. Past
     * the end of parent_ids is the next note in g_notes, and that note
     * starts with its id and secret. */
    uint8_t parent_n =
        n->parent_count > VAULT_MAX_PARENTS ? VAULT_MAX_PARENTS : n->parent_count;
    for (uint8_t i = 0; i < parent_n; i++) {
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

/* Challenge-response over this device's identity key (#69). The host picks
 * the nonce; the device signs "lnurlvault-id-v1" || 0x00 || nonce and returns
 * that with its public key, so a wallet can pin on first pair and notice a
 * swap afterwards.
 *
 * Proves only that whatever is answering holds the same key as last time. Not
 * a defence against someone holding the device -- physical possession is
 * still the model -- and the key never signs a spend. */
static void handle_identify(const char *line, char *out, size_t outcap) {
    if (!g_deps.identity_seed) {
        write_error(out, outcap, "unsupported", "this build has no device identity");
        return;
    }

    char nonce_hex[IDENTITY_NONCE_MAX_LEN * 2 + 1];
    if (!json_get_str(line, "nonce", nonce_hex, sizeof(nonce_hex))) {
        write_error(out, outcap, "bad_request", "identify requires a hex \"nonce\"");
        return;
    }
    const size_t hex_len = strlen(nonce_hex);
    /* The host must choose the nonce, and it must be long enough that answers
     * cannot be usefully precomputed. Bounded at the top so a challenge is
     * never an oracle for signing something longer. */
    if (hex_len % 2 != 0 || hex_len < IDENTITY_NONCE_MIN_LEN * 2 ||
        hex_len > IDENTITY_NONCE_MAX_LEN * 2) {
        write_error(out, outcap, "bad_request", "nonce must be 16 to 32 bytes of hex");
        return;
    }
    uint8_t nonce[IDENTITY_NONCE_MAX_LEN];
    const size_t nonce_len = hex_len / 2;
    if (!hex_decode(nonce_hex, hex_len, nonce, nonce_len)) {
        write_error(out, outcap, "bad_request", "nonce is not hex");
        return;
    }

    uint8_t seed[IDENTITY_SEED_LEN];
    if (!g_deps.identity_seed(seed) || identity_seed_is_blank(seed)) {
        /* An all-zero seed is a valid ed25519 key, so nothing downstream
         * would notice -- and every device that failed to generate one would
         * share an identity, which is worse than having none. */
        crypto_wipe(seed, sizeof(seed));
        write_error(out, outcap, "unsupported", "this device has no identity key");
        return;
    }

    uint8_t pubkey[IDENTITY_PUBKEY_LEN];
    uint8_t sig[IDENTITY_SIG_LEN];
    identity_pubkey(seed, pubkey);
    const bool signed_ok = identity_sign(seed, nonce, nonce_len, sig);
    crypto_wipe(seed, sizeof(seed));
    if (!signed_ok) {
        write_error(out, outcap, "bad_request", "nonce could not be signed");
        return;
    }

    char pubkey_hex[IDENTITY_PUBKEY_LEN * 2 + 1];
    char sig_hex[IDENTITY_SIG_LEN * 2 + 1];
    if (!hex_encode(pubkey, sizeof(pubkey), pubkey_hex, sizeof(pubkey_hex)) ||
        !hex_encode(sig, sizeof(sig), sig_hex, sizeof(sig_hex))) {
        write_error(out, outcap, "bad_request", "could not encode identity");
        return;
    }

    json_writer_t w;
    jw_init(&w, out, outcap);
    jw_begin_obj(&w, NULL);
    jw_bool(&w, "ok", true);
    jw_str(&w, "pubkey", pubkey_hex);
    jw_str(&w, "sig", sig_hex);
    finish(&w, out, outcap);
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
    /* What this link has thrown away, so a session that died can be told from
     * one that was never answered -- see dispatcher.h's transport_drops_fn.
     * Reported for the transport this very command came in on, which is also
     * the one whose numbers the asker can act on. */
    transport_drops_t drops;
    if (g_deps.transport_drops && g_deps.transport_drops(g_source, &drops)) {
        jw_begin_obj(&w, "drops");
        jw_uint64(&w, "rx", drops.rx);
        jw_uint64(&w, "tx", drops.tx);
        jw_uint64(&w, "tx_stalled", drops.tx_stalled);
        jw_end_obj(&w);
    }
    /* What the bus did, as distinct from what the firmware dropped -- see
     * dispatcher.h's usb_link_fn. Not per source: whichever link is still
     * standing is the one that gets to ask. */
    usb_link_t usb;
    if (g_deps.usb_link && g_deps.usb_link(&usb)) {
        jw_begin_obj(&w, "usb");
        jw_uint64(&w, "configured", usb.configured);
        jw_uint64(&w, "unconfigured", usb.unconfigured);
        jw_uint64(&w, "suspends", usb.suspends);
        jw_uint64(&w, "resumes", usb.resumes);
        jw_uint64(&w, "tx_xfers", usb.tx_xfers);
        jw_end_obj(&w);
    }
    /* Loud about storage it cannot read, rather than presenting as an empty
     * working vault -- see dispatcher.h's storage_state_fn.
     *
     * The index check comes first and wins, because storage_state_fn reports
     * how NVS itself came up, and NVS can come up perfectly while the one
     * read of the note index fails. In that case the host would otherwise be
     * told storage "ok" and note_count 0 -- a healthy empty vault -- while
     * every write is being refused. Recovery is a reboot, and specifically
     * NOT a wipe, which is why this must not read as storage_full. */
    if (!vault_index_known()) {
        jw_str(&w, "storage", "index_unreadable");
    } else if (g_deps.storage_state) {
        jw_str(&w, "storage", g_deps.storage_state());
    }
    /* What this device can physically do, so a client stops guessing. The
     * `gated` flag is derived here rather than injected: dispatcher.c is the
     * module that refuses an ungated disclosure, so it is the one that knows
     * whether a confirmation hook is actually wired. A client that reads
     * gated:false knows every physically-gated command on this build will
     * answer `unsupported`, and can say so before the owner tries one. */
    capability_report_t caps;
    if (g_deps.capabilities && g_deps.capabilities(&caps)) {
        jw_begin_obj(&w, "capabilities");
        jw_uint64(&w, "buttons", caps.buttons);
        jw_bool(&w, "touch", caps.touch);
        jw_bool(&w, "gated", g_deps.confirm_export != NULL);
        jw_begin_obj(&w, "display");
        jw_uint64(&w, "width", caps.display_width);
        jw_uint64(&w, "height", caps.display_height);
        jw_end_obj(&w);
        jw_begin_arr(&w, "transports");
        if (caps.serial) {
            jw_str_item(&w, "serial");
        }
        if (caps.ble) {
            jw_str_item(&w, "ble");
        }
        jw_end_arr(&w);
        jw_end_obj(&w);
    }
    /* Which of this device's buttons can be believed. Reported unconditionally
     * rather than only when something is wrong: a client that only ever sees
     * the field on a faulty board cannot tell "this vault's cancel button
     * works" from "this firmware does not report input health". */
    input_report_t inputs = {NULL, NULL};
    if (g_deps.input_report && g_deps.input_report(&inputs)) {
        jw_begin_obj(&w, "inputs");
        /* A NULL field is a button this board has not got. Reporting "ok" for
         * one would be a claim about hardware that is not there, and a client
         * would then tell its owner to press it. */
        if (inputs.confirm) {
            jw_str(&w, "confirm", inputs.confirm);
        }
        if (inputs.cancel) {
            jw_str(&w, "cancel", inputs.cancel);
        }
        jw_end_obj(&w);
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
    finish(&w, out, outcap);
}

/* Builds one page of the listing. Returns false if it did not fit, having
 * left nothing usable in `out` -- the caller decides what to do about that. */
static bool build_listing(char *out, size_t outcap, size_t offset, size_t count, size_t total) {
    json_writer_t w;
    jw_init(&w, out, outcap);
    jw_begin_obj(&w, NULL);
    jw_bool(&w, "ok", true);
    /* total and offset go in unconditionally: a client has to be able to tell
     * "this vault holds three notes" from "this is the first three of thirty",
     * and before paging existed it could not. */
    jw_uint64(&w, "total", total);
    jw_uint64(&w, "offset", offset);
    jw_begin_arr(&w, "notes");
    for (size_t i = 0; i < count; i++) {
        note_meta_t n;
        if (vault_get_meta_at(offset + i, &n)) {
            write_note_obj(&w, &n);
        }
    }
    jw_end_arr(&w);
    if (offset + count < total) {
        jw_uint64(&w, "next_offset", offset + count);
    }
    end_response(&w);
    return jw_ok(&w);
}

/* The only unbounded response in the protocol, and the one that used to break.
 *
 * Issue #7 measured it: output stopped being parseable at 29 notes, or 15 once
 * each carried a signature, against a declared VAULT_MAX_NOTES of 128. So a
 * vault could hold four times more notes than it could ever list, and said
 * nothing about it.
 *
 * `offset` and `limit` are both optional, and omitting them is deliberately
 * NOT an error: a client written against the old protocol gets as many notes
 * as fit -- the same ones it used to get -- plus a `next_offset` telling it
 * there are more. Nothing that worked before works less well, and a client
 * that ignores the new fields is no worse off than it was.
 *
 * An explicit `limit` is honoured or refused, never silently shrunk. A client
 * that asked for fifty and got eight without being told would build a wrong
 * picture of the vault, which is the same class of failure as the truncation
 * this replaced. */
static void handle_list_notes(const char *line, char *out, size_t outcap) {
    const size_t total = vault_count();

    uint64_t offset_u64 = 0;
    if (json_get_u64(line, "offset", &offset_u64) && offset_u64 > total) {
        write_error(out, outcap, "bad_request", "offset is past the end of the list");
        return;
    }
    const size_t offset = (size_t)offset_u64;
    const size_t available = total - offset;

    uint64_t limit_u64 = 0;
    const bool has_limit = json_get_u64(line, "limit", &limit_u64);
    size_t want = has_limit ? (size_t)limit_u64 : available;
    if (want > available) {
        want = available;
    }

    /* The common case: it fits, in one pass. */
    if (build_listing(out, outcap, offset, want, total)) {
        return;
    }
    if (has_limit) {
        write_error(out, outcap, "response_too_large",
                    "too many notes for one response at that limit; ask for fewer");
        return;
    }

    /* No limit given, and the whole remainder does not fit. Find the largest
     * page that does, by bisection rather than by halving: halving would drop
     * a page of 29 to 15 when 29 was only one note too many, and this runs at
     * most a handful of times for VAULT_MAX_NOTES. */
    size_t lo = 0, hi = want;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo + 1) / 2;
        if (build_listing(out, outcap, offset, mid, total)) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    if (lo == 0) {
        /* Not even one note fits. Nothing to page down to. */
        write_error(out, outcap, "response_too_large",
                    "a single note does not fit in one response");
        return;
    }
    build_listing(out, outcap, offset, lo, total);
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
    finish(&w, out, outcap);
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
    finish(&w, out, outcap);
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

/* Asks the owner to approve a command that changes a note they already hold.
 *
 * Returns false having already written the response, so callers read as
 * `if (!confirm_action(...)) return;`. Fails CLOSED when a note cannot be
 * read: refusing is recoverable, and destroying something the owner was never
 * shown is not.
 *
 * When no confirm hook is wired the command proceeds -- that is the old
 * behaviour, and it is what test/native uses. On real firmware main.c always
 * wires one. */
static bool confirm_action(const char *action, const char *id, char *out, size_t outcap) {
    /* No hook means no way to ask, and a gate that disappears because a dep
     * is NULL is not a gate. wipe has always refused in this case, and
     * test_wipe.c gives the reason: "proceeding because no confirm hook
     * happens to be wired would make a build misconfiguration into a remote
     * erase." The same sentence applies here, and to export_secret below,
     * where the misconfiguration becomes a disclosure instead -- an erase
     * destroys value, a disclosure hands it over. */
    if (!g_deps.confirm_action) {
        write_error(out, outcap, "unsupported",
                    "no on-device confirmation available; refusing to act on a held note");
        return false;
    }
    note_meta_t meta;
    const bool known = vault_get_meta(id, &meta);
    confirm_result_t r = g_deps.confirm_action(action, known ? &meta : NULL);
    if (r == CONFIRM_YES) {
        return true;
    }
    write_error(out, outcap, confirm_error_code(r), NULL);
    return false;
}

static void handle_discard(const char *line, char *out, size_t outcap) {
    char id[VAULT_ID_BUF];
    if (!json_get_str(line, "id", id, sizeof(id))) {
        write_error(out, outcap, "bad_request", "discard requires id");
        return;
    }
    if (!confirm_action("discard", id, out, outcap)) {
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

    /* See confirm_action(): the one command that discloses a plaintext secret
     * must not be the one whose gate vanishes when a dep is not wired. */
    if (!g_deps.confirm_export) {
        write_error(out, outcap, "unsupported",
                    "no on-device confirmation available; refusing to disclose a secret");
        return;
    }
    {
        confirm_result_t result = g_deps.confirm_export(&meta);
        /* Only YES proceeds. This used to name the two refusals and fall
         * through on anything else, so a result the enum gained later would
         * have disclosed the secret. */
        if (result != CONFIRM_YES) {
            write_error(out, outcap, confirm_error_code(result), NULL);
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
    finish(&w, out, outcap);
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
    finish(&w, out, outcap);
}

static void handle_mark_spent(const char *line, char *out, size_t outcap) {
    char id[VAULT_ID_BUF];
    if (!json_get_str(line, "id", id, sizeof(id))) {
        write_error(out, outcap, "bad_request", "mark_spent requires id");
        return;
    }
    if (!confirm_action("mark_spent", id, out, outcap)) {
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
    if (!confirm_action("rename", id, out, outcap)) {
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
    /* Refused over BLE: an unauthenticated central could otherwise reboot-loop
     * the device. A physically-wired serial/local client may still use it. */
    if (g_source == DISPATCH_SOURCE_BLE) {
        write_error(out, outcap, "unsupported", "reset is not available over BLE");
        return;
    }
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
#define PRUNE_APPROVE_TIMEOUT_MS 30000 /* likewise */

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

    /* Same rule as confirm_export/confirm_action/wipe: a gate that vanishes
     * when a dep is NULL is not a gate. ota_begin authorises replacing the
     * firmware, so a missing approval hook must refuse rather than silently
     * proceed -- a build misconfiguration (or a port that forgets to wire
     * this) must not become an unapproved firmware update. */
    if (!g_deps.ota_approve) {
        write_error(out, outcap, "unsupported",
                    "no on-device confirmation available; refusing to start an OTA update");
        return;
    }
    {
        confirm_result_t result = g_deps.ota_approve((uint32_t)size_u64, OTA_APPROVE_TIMEOUT_MS);
        if (result != CONFIRM_YES) {
            write_error(out, outcap, confirm_error_code(result), NULL);
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
    if (approved != CONFIRM_YES) {
        write_error(out, outcap, confirm_error_code(approved), NULL);
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
    /* Verified, not assumed -- the same rule wipe_storage() applies to flash.
     * PROTOCOL.md promises ok:true means "erased and verified", and that
     * secrets are gone from RAM as well, but only the flash half was ever
     * checked. Reaching this means vault_forget_all()'s stores did not take
     * effect, so the device must not report the wipe as done. It still
     * reboots below: unlike a failed flash erase, where a reboot would help
     * nothing, a reboot is exactly what clears RAM. */
    if (!vault_secrets_cleared()) {
        write_error(out, outcap, "wipe_failed",
                    "storage was erased and verified, but note secrets could not be confirmed "
                    "cleared from RAM; treat this device as still holding secrets until it has "
                    "rebooted");
        if (g_deps.reset) {
            g_deps.reset();
        }
        return;
    }

    json_writer_t w;
    jw_init(&w, out, outcap);
    jw_begin_obj(&w, NULL);
    jw_bool(&w, "ok", true);
    jw_bool(&w, "wiped", true);
    finish(&w, out, outcap);

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
    if (!confirm_action("delete", id, out, outcap)) {
        return;
    }
    vault_err_t err = vault_delete(id);
    if (err != VAULT_OK) {
        write_vault_error(out, outcap, err);
        return;
    }
    write_ok(out, outcap);
}

/* `remaining` alongside `removed` so a host can reconcile in one round trip
 * rather than following up with list_notes -- and so a sweep that removed
 * nothing is visibly different from one that had nothing to remove. Through
 * the json writer rather than snprintf, so it honours the same truncation
 * contract every other response here does; see finish(). */
static void write_prune_result(char *out, size_t outcap, size_t removed) {
    json_writer_t w;
    jw_init(&w, out, outcap);
    jw_begin_obj(&w, NULL);
    jw_bool(&w, "ok", true);
    jw_uint64(&w, "removed", (uint64_t)removed);
    jw_uint64(&w, "remaining", (uint64_t)vault_count());
    finish(&w, out, outcap);
}

/* Forgets every note the vault already knows is dead.
 *
 * Not a wipe and deliberately nothing like one: it cannot touch a CONFIRMED
 * note, so there is no amount of value it can destroy. A SPENT note is one
 * this vault was TOLD was spent -- a melt a host watched settle, or a
 * rotate/split/merge input -- and it has been dead since.
 *
 * It exists because `delete` takes one id and every gated command costs a
 * physical hold, so clearing a few dozen of them meant a few dozen deliberate
 * two-second holds. That is not housekeeping anyone does, so it does not get
 * done, and the device fills up with dead weight until list_notes starts
 * refusing pages for it.
 *
 * Note what it CANNOT do, and why nothing here tries: it has no idea whether
 * a CONFIRMED note is still outstanding at the mint. Only the mint knows, only
 * a host can ask, and asking means presenting the bearer secret -- see
 * docs/PROTOCOL.md. A vault that quietly dropped notes it merely suspected
 * were spent would be destroying money on a guess. */
static void handle_prune_spent(const char *line, char *out, size_t outcap) {
    (void)line;

    const size_t spent = vault_count_spent();
    if (spent == 0) {
        /* Nothing to do, and deliberately no prompt. Asking somebody to
         * approve a no-op is how people learn to approve without reading,
         * on a device where the next prompt hands over a bearer secret. */
        write_prune_result(out, outcap, 0);
        return;
    }

    if (!g_deps.prune_approve) {
        /* A gate that disappears because a dep is NULL is not a gate -- see
         * confirm_action(). */
        write_error(out, outcap, "unsupported",
                    "no on-device confirmation available; refusing to forget notes");
        return;
    }

    const confirm_result_t approved =
        g_deps.prune_approve((uint32_t)spent, PRUNE_APPROVE_TIMEOUT_MS);
    if (approved != CONFIRM_YES) {
        write_error(out, outcap, confirm_error_code(approved), NULL);
        return;
    }

    size_t removed = 0;
    const vault_err_t err = vault_prune_spent(&removed);
    if (err != VAULT_OK) {
        /* The notes are gone from RAM whatever flash did, so this reports the
         * storage failure rather than a clean sweep a reboot would undo --
         * and it still says how many went, because that is what the owner
         * just watched happen. */
        write_vault_error(out, outcap, err);
        return;
    }
    write_prune_result(out, outcap, removed);
}

void dispatcher_handle(const char *line, char *out, size_t outcap) {
    /* First, before anything can answer, so that even a refusal carries the
     * tag -- a client correlating replies needs the refusal matched too. A
     * tag that cannot be echoed as given (not a string, empty, or over
     * TAG_MAX_LEN) is refused outright rather than echoed truncated, which
     * would match nothing the client sent. */
    g_tag[0] = '\0';
    if (json_has(line, "tag")) {
        if (!json_get_str(line, "tag", g_tag, sizeof(g_tag)) || g_tag[0] == '\0') {
            g_tag[0] = '\0';
            write_error(out, outcap, "bad_request",
                        "tag must be a non-empty string of at most 32 characters");
            return;
        }
    }

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

    if (strcmp(cmd, "identify") == 0) {
        handle_identify(line, out, outcap);
    } else if (strcmp(cmd, "get_info") == 0) {
        handle_get_info(out, outcap);
    } else if (strcmp(cmd, "list_notes") == 0) {
        handle_list_notes(line, out, outcap);
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
    } else if (strcmp(cmd, "prune_spent") == 0) {
        handle_prune_spent(line, out, outcap);
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
