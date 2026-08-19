#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dispatcher.h"
#include "hex.h"
#include "identity.h"
#include "json.h"
#include "unity_lite.h"
#include "vault.h"

static bool rng_basic(uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(rand() & 0xFF);
    }
    return true;
}

static confirm_result_t g_confirm_response = CONFIRM_YES;
static confirm_result_t confirm_stub(const note_meta_t *note) {
    (void)note;
    return g_confirm_response;
}

/* get_info's `inputs` object: how the device reports the health of its own
 * buttons. See src/proto/input_health.h for what the values mean and why a
 * wedged input is worth naming even though approval.c already makes it
 * harmless. */
static input_report_t g_input_report;
static bool g_input_report_available;
static bool input_report_stub(input_report_t *out) {
    if (!g_input_report_available) {
        return false;
    }
    *out = g_input_report;
    return true;
}

/* get_info's `capabilities`: what the device can physically do, so a client
 * stops guessing which gesture to tell the owner about. */
static capability_report_t g_caps;
static bool g_caps_available;
static bool capability_stub(capability_report_t *out) {
    if (!g_caps_available) {
        return false;
    }
    *out = g_caps;
    return true;
}

/* identify: challenge-response over the device key (#69). */
static bool g_identity_available = true;
static const uint8_t IDENTITY_SEED[IDENTITY_SEED_LEN] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
    0xcc, 0xdd, 0xee, 0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};

static bool identity_seed_stub(uint8_t seed[IDENTITY_SEED_LEN]) {
    if (!g_identity_available) return false;
    memcpy(seed, IDENTITY_SEED, IDENTITY_SEED_LEN);
    return true;
}

static void test_identify_answers_a_challenge(void) {
    char out[512];
    char err[32];
    char pubkey_hex[IDENTITY_PUBKEY_LEN * 2 + 1];
    char sig_hex[IDENTITY_SIG_LEN * 2 + 1];
    bool ok;

    dispatcher_deps_t deps = {
        .rng = rng_basic, .confirm_export = confirm_stub, .identity_seed = identity_seed_stub};
    dispatcher_init(&deps);
    g_identity_available = true;

    const char *nonce_hex = "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf";
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"identify\",\"nonce\":\"%s\"}", nonce_hex);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "identify answers");
    UL_CHECK(json_get_str(out, "pubkey", pubkey_hex, sizeof(pubkey_hex)) &&
                  strlen(pubkey_hex) == IDENTITY_PUBKEY_LEN * 2,
              "with a 32-byte public key");
    UL_CHECK(json_get_str(out, "sig", sig_hex, sizeof(sig_hex)) &&
                  strlen(sig_hex) == IDENTITY_SIG_LEN * 2,
              "and a 64-byte signature");

    /* The answer must verify with the host's own check, not the device's. */
    uint8_t pubkey[IDENTITY_PUBKEY_LEN], sig[IDENTITY_SIG_LEN], nonce[16];
    UL_CHECK(hex_decode(pubkey_hex, strlen(pubkey_hex), pubkey, sizeof(pubkey)), "pubkey decodes");
    UL_CHECK(hex_decode(sig_hex, strlen(sig_hex), sig, sizeof(sig)), "sig decodes");
    UL_CHECK(hex_decode(nonce_hex, strlen(nonce_hex), nonce, sizeof(nonce)), "nonce decodes");
    UL_CHECK(identity_verify(pubkey, nonce, sizeof(nonce), sig),
              "and the signature verifies against the reported key");

    /* The device never discloses its seed, only what it derives. */
    UL_CHECK(strstr(out, "1122334455") == NULL, "the seed is never on the wire");

    /* A nonce the host cannot have chosen freely is refused, not signed. */
    dispatcher_handle("{\"cmd\":\"identify\",\"nonce\":\"abcd\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "a short nonce is refused");
    UL_CHECK(json_get_str(out, "error", err, sizeof(err)) && strcmp(err, "bad_request") == 0,
              "as bad_request");

    dispatcher_handle("{\"cmd\":\"identify\",\"nonce\":\"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\"}",
                       out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "a non-hex nonce is refused");

    /* No key provisioned: unsupported, never a signature from a blank seed. */
    g_identity_available = false;
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "a device with no key refuses");
    UL_CHECK(json_get_str(out, "error", err, sizeof(err)) && strcmp(err, "unsupported") == 0,
              "as unsupported");

    dispatcher_deps_t none = {.rng = rng_basic, .confirm_export = confirm_stub};
    dispatcher_init(&none);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "and a build without the hook refuses too");

    dispatcher_deps_t plain = {.rng = rng_basic, .confirm_export = confirm_stub};
    dispatcher_init(&plain);
}

static void test_get_info_reports_capabilities(void) {
    char out[512];

    g_caps_available = true;
    g_caps = (capability_report_t){.buttons = 2,
                                    .touch = false,
                                    .display_width = 320,
                                    .display_height = 170,
                                    .serial = true,
                                    .ble = true};

    dispatcher_deps_t deps = {
        .rng = rng_basic, .confirm_export = confirm_stub, .capabilities = capability_stub};
    dispatcher_init(&deps);
    dispatcher_handle("{\"cmd\":\"get_info\"}", out, sizeof(out));
    UL_CHECK(strstr(out, "\"buttons\":2") != NULL, "the button count is reported");
    UL_CHECK(strstr(out, "\"width\":320") != NULL, "the panel width is reported");
    UL_CHECK(strstr(out, "\"height\":170") != NULL, "the panel height is reported");
    UL_CHECK(strstr(out, "\"serial\"") != NULL && strstr(out, "\"ble\"") != NULL,
              "both transports are listed");

    /* The flag with teeth. A build with no confirmation hook refuses every
     * physically-gated command, and a client can say so up front instead of
     * letting the owner discover it by having an export refused. It is derived
     * inside the dispatcher, not injected, because the dispatcher is what does
     * the refusing. */
    UL_CHECK(strstr(out, "\"gated\":true") != NULL, "a build that can ask says so");

    dispatcher_deps_t ungated = {
        .rng = rng_basic, .confirm_export = NULL, .capabilities = capability_stub};
    dispatcher_init(&ungated);
    dispatcher_handle("{\"cmd\":\"get_info\"}", out, sizeof(out));
    UL_CHECK(strstr(out, "\"gated\":false") != NULL,
              "and a build that cannot ask admits it rather than staying silent");

    /* A board that cannot describe itself omits the object rather than
     * reporting zeroes, which would read as "no buttons, no screen". */
    g_caps_available = false;
    dispatcher_init(&deps);
    dispatcher_handle("{\"cmd\":\"get_info\"}", out, sizeof(out));
    UL_CHECK(strstr(out, "\"capabilities\"") == NULL,
              "an undescribed board claims nothing rather than claiming nothing works");

    dispatcher_deps_t plain = {.rng = rng_basic, .confirm_export = confirm_stub};
    dispatcher_init(&plain);
}

static void test_get_info_reports_input_health(void) {
    char out[512];

    dispatcher_deps_t deps = {
        .rng = rng_basic, .confirm_export = confirm_stub, .input_report = input_report_stub};
    dispatcher_init(&deps);

    /* A board whose cancel line is wedged low -- the ESP32-S3 of checklist
     * section 7a. The whole point is that this is legible over the wire. */
    g_input_report_available = true;
    g_input_report.confirm = "ok";
    g_input_report.cancel = "stuck";
    dispatcher_handle("{\"cmd\":\"get_info\"}", out, sizeof(out));
    UL_CHECK(strstr(out, "\"inputs\"") != NULL, "get_info carries an inputs object");
    UL_CHECK(strstr(out, "\"cancel\":\"stuck\"") != NULL, "naming the cancel button as stuck");
    UL_CHECK(strstr(out, "\"confirm\":\"ok\"") != NULL, "and the confirm button as healthy");

    /* A healthy board must still carry the field. A client that only ever saw
     * `inputs` on a faulty device could not tell a working cancel button from
     * firmware that does not report one. */
    g_input_report.cancel = "ok";
    dispatcher_handle("{\"cmd\":\"get_info\"}", out, sizeof(out));
    UL_CHECK(strstr(out, "\"inputs\"") != NULL, "a healthy board reports inputs too");
    UL_CHECK(strstr(out, "stuck") == NULL, "and reports nothing stuck");

    /* A board with one button reports one. Claiming a healthy cancel button
     * that is not there would have a client tell its owner to press it. */
    g_input_report.cancel = NULL;
    dispatcher_handle("{\"cmd\":\"get_info\"}", out, sizeof(out));
    UL_CHECK(strstr(out, "\"confirm\":\"ok\"") != NULL, "the button it has is reported");
    UL_CHECK(strstr(out, "\"cancel\"") == NULL, "the one it has not is not");
    g_input_report.cancel = "ok";

    /* No hook wired -- a build with no buttons at all -- omits it entirely
     * rather than claiming health it cannot observe. */
    g_input_report_available = false;
    dispatcher_handle("{\"cmd\":\"get_info\"}", out, sizeof(out));
    UL_CHECK(strstr(out, "\"inputs\"") == NULL,
              "a build that cannot observe its inputs claims nothing about them");

    dispatcher_deps_t plain = {.rng = rng_basic, .confirm_export = confirm_stub};
    dispatcher_init(&plain);
}

void test_dispatcher_run(void) {
    vault_init(NULL, NULL);
    srand(123);
    dispatcher_deps_t deps = {.rng = rng_basic, .confirm_export = confirm_stub};
    dispatcher_init(&deps);
    g_confirm_response = CONFIRM_YES;

    char out[512];
    bool ok;
    char err[32];

    dispatcher_handle("{\"cmd\":\"get_info\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "get_info returns ok:true");
    uint64_t note_count;
    UL_CHECK(json_get_u64(out, "note_count", &note_count) && note_count == 0,
             "get_info reports zero notes on a fresh vault");

    dispatcher_handle("{\"cmd\":\"new_secret\",\"label\":\"first\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "new_secret returns ok:true");
    char id[VAULT_ID_BUF], h[VAULT_HASH_HEX_BUF];
    UL_CHECK(json_get_str(out, "id", id, sizeof(id)), "new_secret response carries an id");
    UL_CHECK(json_get_str(out, "h", h, sizeof(h)) && strlen(h) == 64,
             "new_secret response carries a 32-byte hash, never the secret itself");
    UL_CHECK(!json_has(out, "secret") && !json_has(out, "k1"),
             "new_secret response never discloses the raw secret");

    char cmd_buf[128];
    snprintf(cmd_buf, sizeof(cmd_buf), "{\"cmd\":\"export_secret\",\"id\":\"%s\"}", id);
    dispatcher_handle(cmd_buf, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "exporting a still-PENDING note is rejected");
    UL_CHECK(json_get_str(out, "error", err, sizeof(err)) && strcmp(err, "invalid_state") == 0,
             "rejected export reports invalid_state");

    snprintf(cmd_buf, sizeof(cmd_buf),
             "{\"cmd\":\"confirm\",\"id\":\"%s\",\"amount_msat\":21000,\"host\":\"mint.example\"}",
             id);
    dispatcher_handle(cmd_buf, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "confirm returns ok:true");

    snprintf(cmd_buf, sizeof(cmd_buf), "{\"cmd\":\"export_secret\",\"id\":\"%s\"}", id);

    g_confirm_response = CONFIRM_NO;
    dispatcher_handle(cmd_buf, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "a declined on-device confirm blocks export");
    UL_CHECK(json_get_str(out, "error", err, sizeof(err)) && strcmp(err, "user_declined") == 0,
             "declined export reports user_declined");

    g_confirm_response = CONFIRM_TIMEOUT;
    dispatcher_handle(cmd_buf, out, sizeof(out));
    UL_CHECK(json_get_str(out, "error", err, sizeof(err)) && strcmp(err, "timeout") == 0,
             "a timed-out on-device confirm reports timeout");

    g_confirm_response = CONFIRM_YES;
    dispatcher_handle(cmd_buf, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "an approved on-device confirm allows export");
    char k1[VAULT_SECRET_HEX_BUF];
    UL_CHECK(json_get_str(out, "k1", k1, sizeof(k1)) && strlen(k1) == 64,
             "export_secret response carries the 32-byte secret only after approval");

    dispatcher_handle("{\"cmd\":\"list_notes\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "list_notes returns ok:true");
    UL_CHECK(json_has(out, "notes"), "list_notes response carries a notes field");

    dispatcher_handle("{\"cmd\":\"reset\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok,
             "reset returns ok:true even with no reset_fn wired (native has no device to reboot)");

    /* reset over BLE is refused -- an unauthenticated central could otherwise
     * reboot-loop the device (issue #79). */
    {
        char err[32];
        dispatcher_set_source(DISPATCH_SOURCE_BLE);
        dispatcher_handle("{\"cmd\":\"reset\"}", out, sizeof(out));
        UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "reset over BLE is refused");
        UL_CHECK(json_get_str(out, "error", err, sizeof(err)) && strcmp(err, "unsupported") == 0,
                 "reset over BLE reports unsupported");
        dispatcher_set_source(DISPATCH_SOURCE_SERIAL);
        dispatcher_handle("{\"cmd\":\"reset\"}", out, sizeof(out));
        UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "reset over serial is allowed");
        dispatcher_set_source(DISPATCH_SOURCE_LOCAL); /* restore default for later tests */
    }

    /* import_secret is the one command that takes a secret FROM the wire,
     * and it had no dispatcher-level test at all. */
    const char *k1_a = "\"k1\":\"" "1111111111111111111111111111111111111111111111111111111111111111" "\"";
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "{\"cmd\":\"import_secret\",%s,\"host\":\"mint.example\",\"amount_msat\":5000,"
             "\"label\":\"received\"}",
             k1_a);
    dispatcher_handle(cmd, out, sizeof(out));
    char imported_id[VAULT_ID_BUF];
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "import_secret accepts a well-formed secret");
    UL_CHECK(json_get_str(out, "id", imported_id, sizeof(imported_id)),
             "import_secret answers with the new note's id");

    /* A note IS its secret. Importing the same one again must not produce a
     * second note: two entries backed by one secret report double the value
     * held, and spending either leaves the other looking spendable. The
     * ordinary way to get here is a retry after a lost response, so the
     * second call answers with the same id rather than failing. */
    size_t before_notes, before_pending;
    vault_get_info(&before_notes, &before_pending);
    dispatcher_handle(cmd, out, sizeof(out));
    char again_id[VAULT_ID_BUF];
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "re-importing a held secret is not an error");
    size_t after_notes, after_pending;
    vault_get_info(&after_notes, &after_pending);
    UL_CHECK(after_notes == before_notes,
             "re-importing a held secret creates no second note for the same secret");
    UL_CHECK(json_get_str(out, "id", again_id, sizeof(again_id)) &&
                 strcmp(again_id, imported_id) == 0,
             "re-importing a held secret answers with the note that already has it");

    /* Same secret, different claimed amount: the held note must not be
     * restated, or the wire could change what the approval screen says. */
    char cmd2[256];
    snprintf(cmd2, sizeof(cmd2),
             "{\"cmd\":\"import_secret\",%s,\"host\":\"evil.example\",\"amount_msat\":99999999}",
             k1_a);
    dispatcher_handle(cmd2, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "a re-import with different fields still succeeds");
    vault_get_info(&after_notes, &after_pending);
    UL_CHECK(after_notes == before_notes,
             "a re-import with different fields creates no note either");
    note_meta_t held;
    UL_CHECK(vault_get_meta(imported_id, &held) && held.amount_msat == 5000 &&
                 strcmp(held.host, "mint.example") == 0,
             "a re-import cannot restate a held note's amount or host");

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"import_secret\",\"k1\":\"abcd\",\"host\":\"h\","
                                "\"amount_msat\":1}");
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "import_secret rejects a short secret");
    dispatcher_handle("{\"cmd\":\"import_secret\",\"host\":\"h\",\"amount_msat\":1}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "import_secret requires k1");

    /* new_secret_pair was the other command with no dispatcher-level test.
     * The split builds both notes before committing either, so a failure on
     * the second adds nothing -- that part is sound. What is worth pinning
     * here is the response shape: two distinct notes, and the same
     * no-secrets-on-the-wire guarantee new_secret gets. */
    dispatcher_handle("{\"cmd\":\"new_secret_pair\",\"label\":\"split\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "new_secret_pair returns ok:true");
    char p_id[VAULT_ID_BUF], p_id2[VAULT_ID_BUF];
    char p_h[VAULT_HASH_HEX_BUF], p_h2[VAULT_HASH_HEX_BUF];
    UL_CHECK(json_get_str(out, "id", p_id, sizeof(p_id)) &&
                 json_get_str(out, "id2", p_id2, sizeof(p_id2)) &&
                 strcmp(p_id, p_id2) != 0,
             "new_secret_pair returns two distinct ids");
    UL_CHECK(json_get_str(out, "h", p_h, sizeof(p_h)) &&
                 json_get_str(out, "h2", p_h2, sizeof(p_h2)) && strlen(p_h) == 64 &&
                 strlen(p_h2) == 64 && strcmp(p_h, p_h2) != 0,
             "new_secret_pair returns two distinct 32-byte hashes");
    UL_CHECK(!json_has(out, "secret") && !json_has(out, "k1"),
             "new_secret_pair discloses hashes only, never either raw secret");

    dispatcher_handle("{\"cmd\":\"totally_unknown\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "an unknown command is rejected");
    UL_CHECK(json_get_str(out, "error", err, sizeof(err)) && strcmp(err, "bad_request") == 0,
             "an unknown command reports bad_request");

    dispatcher_handle("not even json", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "malformed input is rejected, not crashed on");

    test_get_info_reports_input_health();
    test_get_info_reports_capabilities();
    test_identify_answers_a_challenge();
}
