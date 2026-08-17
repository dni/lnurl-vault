#include <stdio.h>
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

static confirm_result_t g_confirm_response = CONFIRM_YES;
static confirm_result_t confirm_stub(const note_meta_t *note) {
    (void)note;
    return g_confirm_response;
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

    dispatcher_handle("{\"cmd\":\"totally_unknown\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "an unknown command is rejected");
    UL_CHECK(json_get_str(out, "error", err, sizeof(err)) && strcmp(err, "bad_request") == 0,
             "an unknown command reports bad_request");

    dispatcher_handle("not even json", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "malformed input is rejected, not crashed on");
}
