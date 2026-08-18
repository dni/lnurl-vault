/* One property, swept across the command surface: a note's secret appears in
 * exactly one response in this protocol, export_secret's, and nowhere else.
 *
 * vault.h states it -- note_meta_t is "a metadata view of a note, never
 * carries the secret. This, not note_t, is what list_notes/get commands are
 * allowed to serialize onto the wire" -- and the only checks that existed
 * were of the form
 *
 *     UL_CHECK(!json_has(out, "secret") && !json_has(out, "k1"), ...)
 *
 * which look for FIELD NAMES. That is the wrong shape. A leak does not have
 * to arrive under a field called "secret": the disclosure fixed in the
 * parent_count work put secret bytes on the wire as elements of the
 * parent_ids array, which those checks would have passed straight over.
 *
 * So this looks for the bytes themselves, in both forms they could take:
 *
 *   - hex, the canonical way this protocol renders a secret
 *   - raw, as a serializer copying note_t memory into a JSON string would
 *
 * The secret is chosen so the raw form is printable ASCII ("SECRET" repeated,
 * then "12"), which is what makes the second check possible at all -- a
 * random 32-byte secret leaked raw would be mostly unprintable and would not
 * survive as a findable substring. */
#include <stdio.h>
#include <string.h>

#include "dispatcher.h"
#include "hex.h"
#include "json.h"
#include "unity_lite.h"
#include "vault.h"

/* "SECRETSECRETSECRETSECRETSECRET12" -- 32 bytes, every one printable. */
static const char *SECRET_HEX =
    "534543524554534543524554534543524554534543524554534543524554" "3132";
static const char *SECRET_RAW = "SECRETSECRETSECRETSECRETSECRET12";

static bool rng_stub(uint8_t *out, size_t len) {
    static uint32_t s = 7;
    for (size_t i = 0; i < len; i++) {
        s = s * 1103515245u + 12345u;
        out[i] = (uint8_t)(s >> 16);
    }
    return true;
}

/* Records every breadcrumb the dispatcher emits, so they can be checked for
 * the same thing the responses are. */
#define TRACE_MAX 32
static char g_traced[TRACE_MAX][64];
static int g_trace_count;
static void trace_recorder(const char *cmd) {
    if (cmd && g_trace_count < TRACE_MAX) {
        snprintf(g_traced[g_trace_count++], sizeof(g_traced[0]), "%s", cmd);
    }
}

static confirm_result_t always_yes(const note_meta_t *note) {
    (void)note;
    return CONFIRM_YES;
}

/* Checks one response for either form of the secret. */
static void expect_no_secret(const char *what, const char *out) {
    char msg[128];
    snprintf(msg, sizeof(msg), "%s does not leak the secret as hex", what);
    UL_CHECK(strstr(out, SECRET_HEX) == NULL, msg);
    snprintf(msg, sizeof(msg), "%s does not leak the secret as raw bytes", what);
    UL_CHECK(strstr(out, SECRET_RAW) == NULL, msg);
}

void test_secret_leak_run(void) {
    /* The two constants have to be the same 32 bytes in different forms. If
     * they ever drift apart, every check below silently searches for
     * something that was never in the vault and passes for the wrong reason
     * -- so establish it rather than trusting the arithmetic. */
    uint8_t decoded[VAULT_SECRET_LEN];
    UL_CHECK(hex_decode(SECRET_HEX, strlen(SECRET_HEX), decoded, sizeof(decoded)),
             "the probe secret is valid hex");
    UL_CHECK(strlen(SECRET_RAW) == VAULT_SECRET_LEN &&
                 memcmp(decoded, SECRET_RAW, VAULT_SECRET_LEN) == 0,
             "and its raw form is the same 32 bytes, so both probes look for the real secret");

    vault_init(NULL, NULL);
    dispatcher_deps_t deps = {
        .rng = rng_stub, .confirm_export = always_yes, .trace_cmd = trace_recorder};
    dispatcher_init(&deps);
    g_trace_count = 0;

    char out[2048];
    char cmd[512];

    /* Put a note with a known secret in the vault. */
    snprintf(cmd, sizeof(cmd),
             "{\"cmd\":\"import_secret\",\"k1\":\"%s\",\"host\":\"mint.example\","
             "\"amount_msat\":21000,\"label\":\"leak probe\"}",
             SECRET_HEX);
    dispatcher_handle(cmd, out, sizeof(out));
    bool ok = false;
    char id[VAULT_ID_BUF];
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "the probe note was imported");
    UL_CHECK(json_get_str(out, "id", id, sizeof(id)), "and has an id");
    expect_no_secret("import_secret's own reply", out);

    /* A second note, so list_notes has more than one entry to get wrong and
     * so there is an adjacent note_t in g_notes to read past into. */
    dispatcher_handle("{\"cmd\":\"new_secret\",\"label\":\"neighbour\"}", out, sizeof(out));
    char id2[VAULT_ID_BUF];
    UL_CHECK(json_get_str(out, "id", id2, sizeof(id2)), "a neighbouring note exists");
    expect_no_secret("new_secret", out);

    dispatcher_handle("{\"cmd\":\"get_info\"}", out, sizeof(out));
    expect_no_secret("get_info", out);

    dispatcher_handle("{\"cmd\":\"list_notes\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "list_notes works");
    UL_CHECK(strstr(out, "leak probe") != NULL,
             "and really did serialize the probe note, so the checks below mean something");
    expect_no_secret("list_notes", out);

    dispatcher_handle("{\"cmd\":\"new_secret_pair\",\"label\":\"split\"}", out, sizeof(out));
    expect_no_secret("new_secret_pair", out);

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"rename\",\"id\":\"%s\",\"label\":\"renamed\"}", id);
    dispatcher_handle(cmd, out, sizeof(out));
    expect_no_secret("rename", out);

    snprintf(cmd, sizeof(cmd),
             "{\"cmd\":\"confirm\",\"id\":\"%s\",\"amount_msat\":5,\"host\":\"h\"}", id2);
    dispatcher_handle(cmd, out, sizeof(out));
    expect_no_secret("confirm", out);

    /* Errors are responses too, and a message field is a natural place for a
     * value to be echoed back without anyone thinking of it as disclosure. */
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"confirm\",\"id\":\"%s\",\"amount_msat\":5,\"host\":\"h\"}",
             id);
    dispatcher_handle(cmd, out, sizeof(out));
    expect_no_secret("a rejected confirm", out);

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"discard\",\"id\":\"%s\"}", id);
    dispatcher_handle(cmd, out, sizeof(out));
    expect_no_secret("a rejected discard", out);

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"delete\",\"id\":\"%s\"}", id);
    dispatcher_handle(cmd, out, sizeof(out));
    expect_no_secret("a rejected delete", out);

    /* An import that echoes its own argument back would be the easiest leak
     * of all to write by accident. */
    dispatcher_handle(cmd, out, sizeof(out));
    snprintf(cmd, sizeof(cmd),
             "{\"cmd\":\"import_secret\",\"k1\":\"%s\",\"host\":\"h\",\"amount_msat\":1}",
             SECRET_HEX);
    dispatcher_handle(cmd, out, sizeof(out));
    expect_no_secret("a duplicate import", out);

    dispatcher_handle("{\"cmd\":\"totally_unknown\"}", out, sizeof(out));
    expect_no_secret("an unknown command", out);

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"import_secret\",\"k1\":\"%s\"}", SECRET_HEX);
    dispatcher_handle(cmd, out, sizeof(out));
    expect_no_secret("a malformed import that quotes the secret back", out);

    /* The crash breadcrumb is a second way out of the device, and a quieter
     * one: crash_crumb.h says "WHAT MUST NEVER GO IN HERE: anything derived
     * from a secret", it survives a reboot in RTC memory, and get_info hands
     * it back over the wire as last_cmd_in_flight.
     *
     * dispatcher.c passes the parsed cmd field, never `line` -- which for
     * import_secret is the one string in the protocol containing a raw
     * secret. Nothing tested that. Changing it to trace `line` for better
     * diagnostics is exactly the well-meant edit that would survive review,
     * so pin it: every breadcrumb from this run, including the ones from
     * commands carrying the secret, is checked for both forms of it.
     *
     * The length bound is the same claim from the other side: a breadcrumb
     * that could hold 64 hex characters could hold a secret. */
    UL_CHECK(g_trace_count > 0, "breadcrumbs were recorded at all");
    for (int i = 0; i < g_trace_count; i++) {
        /* Sized for a full-length breadcrumb plus the longest suffix below:
         * GNU gcc's -Werror=format-truncation catches this and clang does not,
         * which is the standing trap in this suite. */
        char what[224];
        snprintf(what, sizeof(what), "breadcrumb \"%s\"", g_traced[i]);
        expect_no_secret(what, g_traced[i]);
        /* crash_crumb's own buffer is 24 bytes, so a whole 64-character hex
         * secret could never fit there anyway -- the realistic leak is a
         * PREFIX. Sixteen characters is eight bytes of the secret, which is
         * a disclosure whether or not the rest followed. */
        char prefix[17];
        memcpy(prefix, SECRET_HEX, 16);
        prefix[16] = '\0';
        snprintf(what, sizeof(what), "breadcrumb \"%s\" holds no prefix of the secret either",
                 g_traced[i]);
        UL_CHECK(strstr(g_traced[i], prefix) == NULL, what);
        snprintf(what, sizeof(what), "breadcrumb \"%s\" is a bare command name, not arguments",
                 g_traced[i]);
        UL_CHECK(strchr(g_traced[i], '{') == NULL && strchr(g_traced[i], '"') == NULL &&
                     strlen(g_traced[i]) < 32,
                 what);
    }

    /* The one command that IS allowed to disclose it -- proof the probe
     * above is capable of finding the secret when it really is present, so a
     * clean sweep means absence rather than a broken check. */
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"export_secret\",\"id\":\"%s\"}", id);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "export_secret is approved");
    UL_CHECK(strstr(out, SECRET_HEX) != NULL,
             "export_secret does disclose it -- so the sweep above can detect a leak");
}
