/* The `wipe` command, and the thing it replaced.
 *
 * Issue #6: vault_nvs_boot() used to erase NVS on ESP_ERR_NVS_NO_FREE_PAGES,
 * which is the idiom from every ESP-IDF example and, on a device holding
 * bearer notes, silent destruction of value. The erase now lives behind this
 * command instead. Which means this command is the only thing in the firmware
 * that can destroy notes, so it is worth being thorough about.
 *
 * The erase itself is ESP-IDF and cannot be reached from here. Everything
 * around it -- the gates, the ordering, and what happens when the erase
 * fails -- is portable, and that is where the ways to get this wrong are. */
#include <stdio.h>
#include <string.h>

#include "dispatcher.h"
#include "json.h"
#include "unity_lite.h"
#include "vault.h"

/* ---- fakes ------------------------------------------------------------- */

static uint32_t g_seq;
static bool rng_seq(uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        g_seq = g_seq * 1103515245u + 12345u;
        out[i] = (uint8_t)(g_seq >> 16);
    }
    return true;
}

static confirm_result_t g_approve_answer;
static int g_approve_calls;
static confirm_result_t fake_approve(uint32_t timeout_ms) {
    (void)timeout_ms;
    g_approve_calls++;
    return g_approve_answer;
}

static bool g_wipe_succeeds;
static int g_wipe_calls;
static bool fake_wipe(void) {
    g_wipe_calls++;
    return g_wipe_succeeds;
}

static int g_reset_calls;
static void fake_reset(void) {
    g_reset_calls++;
}

static const char *g_storage_state = "ok";
static const char *fake_storage_state(void) {
    return g_storage_state;
}

/* Wires the dispatcher with whichever hooks a case needs, then puts one
 * CONFIRMED note in the vault so "did it actually erase anything" is a
 * question with an answer. */
static void setup(bool with_approve, bool with_wipe) {
    dispatcher_deps_t deps = {
        .rng = rng_seq,
        .reset = fake_reset,
        .storage_state = fake_storage_state,
    };
    if (with_approve) {
        deps.wipe_approve = fake_approve;
    }
    if (with_wipe) {
        deps.wipe_storage = fake_wipe;
    }
    dispatcher_init(&deps);

    vault_init(NULL, NULL);
    g_seq = 11;
    g_approve_calls = 0;
    g_wipe_calls = 0;
    g_reset_calls = 0;

    char id[VAULT_ID_BUF], h[VAULT_HASH_HEX_BUF];
    vault_new_secret(rng_seq, NULL, 0, "keepme", id, h);
    vault_confirm(id, 1000, "example.com", NULL);
}

static bool has(const char *json, const char *needle) {
    return strstr(json, needle) != NULL;
}

/* ---- the request-side gate -------------------------------------------- */

/* A bare {"cmd":"wipe"} must do nothing. `wipe` shares a command namespace
 * with get_info, reachable by anything already paired, and is far too easy to
 * emit by accident from a retry loop or a mistyped script. */
static void test_bare_wipe_is_refused(void) {
    setup(true, true);
    char out[512];
    dispatcher_handle("{\"cmd\":\"wipe\"}", out, sizeof(out));

    UL_CHECK(has(out, "bad_request"), "a wipe with no confirmation phrase is a bad request");
    UL_CHECK(g_wipe_calls == 0, "and never reaches storage");
    UL_CHECK(g_approve_calls == 0, "and never even bothers the owner");
    UL_CHECK(vault_count() == 1, "the note is untouched");
}

static void test_wrong_confirmation_phrase_is_refused(void) {
    setup(true, true);
    char out[512];
    dispatcher_handle("{\"cmd\":\"wipe\",\"confirm\":\"yes\"}", out, sizeof(out));

    UL_CHECK(has(out, "bad_request"), "the phrase has to be the right one");
    UL_CHECK(g_wipe_calls == 0, "and a near-miss does not erase");
    UL_CHECK(vault_count() == 1, "the note is untouched");
}

/* ---- the physical gate ------------------------------------------------- */

static void test_declined_wipe_erases_nothing(void) {
    setup(true, true);
    g_approve_answer = CONFIRM_NO;
    char out[512];
    dispatcher_handle("{\"cmd\":\"wipe\",\"confirm\":\"WIPE\"}", out, sizeof(out));

    UL_CHECK(has(out, "user_declined"), "declining is reported as declining");
    UL_CHECK(g_wipe_calls == 0, "and erases nothing");
    UL_CHECK(vault_count() == 1, "the note survives");
}

static void test_unanswered_wipe_erases_nothing(void) {
    setup(true, true);
    g_approve_answer = CONFIRM_TIMEOUT;
    char out[512];
    dispatcher_handle("{\"cmd\":\"wipe\",\"confirm\":\"WIPE\"}", out, sizeof(out));

    UL_CHECK(has(out, "timeout"), "an unanswered prompt is a timeout, not a decline");
    UL_CHECK(g_wipe_calls == 0, "and erases nothing");
    UL_CHECK(vault_count() == 1, "the note survives");
}

/* An unconfirmable wipe is not one to grant. The alternative -- proceeding
 * because no confirm hook happens to be wired -- would make a build
 * misconfiguration into a remote erase. */
static void test_wipe_without_a_confirm_hook_is_refused(void) {
    setup(false, true);
    char out[512];
    dispatcher_handle("{\"cmd\":\"wipe\",\"confirm\":\"WIPE\"}", out, sizeof(out));

    UL_CHECK(has(out, "unsupported"), "no way to confirm means no wipe");
    UL_CHECK(g_wipe_calls == 0, "and certainly no erase");
    UL_CHECK(vault_count() == 1, "the note survives");
}

static void test_wipe_without_storage_is_unsupported(void) {
    setup(true, false);
    g_approve_answer = CONFIRM_YES;
    char out[512];
    dispatcher_handle("{\"cmd\":\"wipe\",\"confirm\":\"WIPE\"}", out, sizeof(out));

    UL_CHECK(has(out, "unsupported"), "a build with no storage says so");
    UL_CHECK(g_approve_calls == 0, "without asking the owner to confirm nothing");
}

/* ---- the part that matters most --------------------------------------- */

/* An erase that could not be verified must not be reported as success, must
 * not clear RAM, and must not reboot. Until the verification passes, RAM
 * holds the only intact copy of the notes -- throwing it away first would
 * turn a failed wipe into a successful one. */
static void test_a_failed_wipe_claims_nothing_and_keeps_the_notes(void) {
    setup(true, true);
    g_approve_answer = CONFIRM_YES;
    g_wipe_succeeds = false;

    char out[512];
    dispatcher_handle("{\"cmd\":\"wipe\",\"confirm\":\"WIPE\"}", out, sizeof(out));

    UL_CHECK(has(out, "wipe_failed"), "a failed wipe is reported as failed");
    UL_CHECK(!has(out, "\"ok\":true"), "and never as ok");
    UL_CHECK(g_wipe_calls == 1, "the erase was attempted");
    UL_CHECK(vault_count() == 1, "RAM still holds the notes -- the only intact copy left");
    UL_CHECK(g_reset_calls == 0, "and the device is not rebooted on an unverified erase");
}

static void test_a_successful_wipe_empties_everything(void) {
    setup(true, true);
    g_approve_answer = CONFIRM_YES;
    g_wipe_succeeds = true;

    char out[512];
    dispatcher_handle("{\"cmd\":\"wipe\",\"confirm\":\"WIPE\"}", out, sizeof(out));

    UL_CHECK(has(out, "\"ok\":true"), "a verified wipe reports success");
    UL_CHECK(has(out, "\"wiped\":true"), "explicitly");
    UL_CHECK(g_wipe_calls == 1, "storage was erased");
    UL_CHECK(vault_count() == 0, "and RAM was cleared too");
    UL_CHECK(g_reset_calls == 1, "and a reboot was scheduled, so nothing derived survives");
}

/* The secret must be gone, not merely uncounted. A vault that reports zero
 * notes while their bytes are still sitting in RAM has not been wiped. */
static void test_wiped_secrets_are_not_exportable(void) {
    setup(true, true);
    g_approve_answer = CONFIRM_YES;
    g_wipe_succeeds = true;

    /* Remember the id before it goes. */
    char id[VAULT_ID_BUF];
    note_meta_t meta;
    UL_CHECK(vault_get_meta_at(0, &meta), "there is a note to begin with");
    memcpy(id, meta.id, sizeof(id));

    char out[512];
    dispatcher_handle("{\"cmd\":\"wipe\",\"confirm\":\"WIPE\"}", out, sizeof(out));

    char k1[VAULT_SECRET_HEX_BUF];
    UL_CHECK(vault_export_secret(id, k1) != VAULT_OK, "the wiped note cannot be exported");
    UL_CHECK(!vault_get_meta(id, &meta), "and is not found at all");

    /* Both checks above pass on `g_note_count = 0` alone, with every secret
     * byte still in RAM -- which is the exact situation this test's own
     * comment says must not count as a wipe. Assert the bytes. */
    UL_CHECK(vault_secrets_cleared(),
             "no byte of any note secret is left in RAM, not merely uncounted");

    bool ok = false;
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "and the wipe reported success");
}

/* The success claim is what the owner acts on -- by selling or handing on the
 * device -- so it has to be conditional on the RAM check, not just on flash.
 * Nothing here can make vault_forget_all() fail, so this pins the reporting
 * path instead: a vault that still holds secrets must not answer ok. */
static void test_wipe_reports_ram_it_could_not_clear(void) {
    setup(true, true);
    g_approve_answer = CONFIRM_YES;
    g_wipe_succeeds = true;

    char out[512];
    dispatcher_handle("{\"cmd\":\"wipe\",\"confirm\":\"WIPE\"}", out, sizeof(out));

    bool ok = false;
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "a wipe that clears RAM reports success");
    UL_CHECK(vault_secrets_cleared(), "and RAM really is clear when it says so");

    /* The two claims are tied together: the response says the wipe is done
     * only in the same run in which the bytes are actually gone. */
    UL_CHECK(ok == vault_secrets_cleared(),
             "the success claim and the state of RAM agree");
}

/* Wiping twice must be safe: the second is a no-op on an empty vault, not an
 * error and not a crash. */
static void test_wiping_twice_is_safe(void) {
    setup(true, true);
    g_approve_answer = CONFIRM_YES;
    g_wipe_succeeds = true;

    char out[512];
    dispatcher_handle("{\"cmd\":\"wipe\",\"confirm\":\"WIPE\"}", out, sizeof(out));
    dispatcher_handle("{\"cmd\":\"wipe\",\"confirm\":\"WIPE\"}", out, sizeof(out));

    UL_CHECK(has(out, "\"ok\":true"), "a second wipe is fine");
    UL_CHECK(vault_count() == 0, "and the vault is still empty");
}

/* ---- surfacing the condition instead of erasing ----------------------- */

/* The other half of issue #6. Refusing to erase a full partition is only
 * useful if the device can say why it has no notes -- otherwise it presents
 * as an empty working vault while holding every note on flash, unread, and
 * the owner concludes they are gone. */
static void test_get_info_reports_storage_state(void) {
    setup(true, true);
    char out[512];

    g_storage_state = "ok";
    dispatcher_handle("{\"cmd\":\"get_info\"}", out, sizeof(out));
    UL_CHECK(has(out, "\"storage\":\"ok\""), "healthy storage is reported as such");

    g_storage_state = "full";
    dispatcher_handle("{\"cmd\":\"get_info\"}", out, sizeof(out));
    UL_CHECK(has(out, "\"storage\":\"full\""),
             "a full partition is reported, not silently erased");

    g_storage_state = "version_unsupported";
    dispatcher_handle("{\"cmd\":\"get_info\"}", out, sizeof(out));
    UL_CHECK(has(out, "\"storage\":\"version_unsupported\""),
             "a newer on-flash format is reported, not silently erased");

    g_storage_state = "ok";
}

/* A build with no storage hook at all must simply omit the field rather than
 * inventing a state -- that is what the native tests themselves are. */
static void test_storage_field_omitted_when_unknown(void) {
    dispatcher_deps_t deps = {.rng = rng_seq};
    dispatcher_init(&deps);
    char out[512];
    dispatcher_handle("{\"cmd\":\"get_info\"}", out, sizeof(out));
    UL_CHECK(!has(out, "\"storage\""), "no storage hook means no storage claim");
}

void test_wipe_run(void) {
    printf("-- wipe --\n");
    test_bare_wipe_is_refused();
    test_wrong_confirmation_phrase_is_refused();
    test_declined_wipe_erases_nothing();
    test_unanswered_wipe_erases_nothing();
    test_wipe_without_a_confirm_hook_is_refused();
    test_wipe_without_storage_is_unsupported();
    test_a_failed_wipe_claims_nothing_and_keeps_the_notes();
    test_a_successful_wipe_empties_everything();
    test_wiped_secrets_are_not_exportable();
    test_wipe_reports_ram_it_could_not_clear();
    test_wiping_twice_is_safe();
    test_get_info_reports_storage_state();
    test_storage_field_omitted_when_unknown();
}
