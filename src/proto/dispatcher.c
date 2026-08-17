#include "dispatcher.h"

#include <string.h>

#include "json.h"

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
    if (!json_get_str(line, "cmd", cmd, sizeof(cmd))) {
        write_error(out, outcap, "bad_request", "missing or invalid cmd");
        return;
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
    } else if (strcmp(cmd, "delete") == 0) {
        handle_delete(line, out, outcap);
    } else {
        write_error(out, outcap, "bad_request", "unknown cmd");
    }
}
