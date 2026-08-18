/* The physical gate in front of commands that destroy value.
 *
 * Issue #16. BLE has no bonding and no passkey, so any central in radio range
 * is already a client. The README's reasoning for accepting that was that a
 * secret cannot be extracted without a physical gesture -- true, and beside
 * the point: mark_spent, delete, discard and rename were all ungated, so an
 * attacker who could not read a single secret could still mark every live note
 * spent and then delete them. Real value destroyed, no secret ever learned.
 *
 * The property under test throughout: a declined or unanswered prompt must
 * leave the vault exactly as it was. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dispatcher.h"
#include "unity_lite.h"
#include "vault.h"

static uint32_t g_seq;
static bool rng_seq(uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        g_seq = g_seq * 1103515245u + 12345u;
        out[i] = (uint8_t)(g_seq >> 16);
    }
    return true;
}

static confirm_result_t g_answer;
static int g_asked;
static char g_last_action[32];
static char g_last_note_id[VAULT_ID_BUF];
static bool g_saw_note;

static confirm_result_t fake_confirm_export(const note_meta_t *note) {
    (void)note;
    g_asked++;
    return g_answer;
}

static confirm_result_t fake_confirm(const char *action, const note_meta_t *note) {
    g_asked++;
    snprintf(g_last_action, sizeof(g_last_action), "%s", action ? action : "");
    g_saw_note = (note != NULL);
    if (note) {
        memcpy(g_last_note_id, note->id, sizeof(g_last_note_id));
    }
    return g_answer;
}

/* One CONFIRMED note, and one PENDING note for discard to work on. */
static char g_confirmed[VAULT_ID_BUF];
static char g_pending[VAULT_ID_BUF];

static void setup(bool gated) {
    dispatcher_deps_t deps = {.rng = rng_seq};
    if (gated) {
        deps.confirm_action = fake_confirm;
    }
    dispatcher_init(&deps);
    vault_init(NULL, NULL);
    g_seq = 3;
    g_asked = 0;
    g_saw_note = false;
    g_last_action[0] = '\0';

    char h[VAULT_HASH_HEX_BUF];
    vault_new_secret(rng_seq, NULL, 0, "keepme", g_confirmed, h);
    vault_confirm(g_confirmed, 21000, "mint.example.com", NULL);
    vault_new_secret(rng_seq, NULL, 0, "unsettled", g_pending, h);
}

static bool state_is(const char *id, note_state_t want) {
    note_meta_t m;
    return vault_get_meta(id, &m) && m.state == want;
}

/* ---- the attack the issue describes ------------------------------------ */

/* Someone in radio range, with no bond and no passkey, tries to destroy a
 * live note. Every step must be refused, and the note must survive intact. */
static void test_a_stranger_cannot_destroy_a_live_note(void) {
    setup(true);
    g_answer = CONFIRM_NO;
    char out[512];

    dispatcher_handle("{\"cmd\":\"mark_spent\",\"id\":\"PLACEHOLDER\"}", out, sizeof(out));
    char cmd[128];

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"mark_spent\",\"id\":\"%s\"}", g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strstr(out, "user_declined") != NULL, "mark_spent is refused");
    UL_CHECK(state_is(g_confirmed, NOTE_STATE_CONFIRMED), "and the note is still spendable");

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"delete\",\"id\":\"%s\"}", g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strstr(out, "user_declined") != NULL, "delete is refused");
    UL_CHECK(vault_count() == 2, "and nothing was deleted");

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"discard\",\"id\":\"%s\"}", g_pending);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strstr(out, "user_declined") != NULL, "discard is refused");
    UL_CHECK(state_is(g_pending, NOTE_STATE_PENDING), "and the pending note survives");
}

/* Renaming is not destructive of value, but it IS destructive of meaning: the
 * approval screen shows a note's label, so an attacker who can rename freely
 * can make that screen lie about what is being disclosed. Gated for that
 * reason, not for tidiness. */
static void test_rename_is_gated_too(void) {
    setup(true);
    g_answer = CONFIRM_NO;
    char out[512], cmd[160];

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"rename\",\"id\":\"%s\",\"label\":\"totally safe\"}",
             g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strstr(out, "user_declined") != NULL, "rename is refused");

    note_meta_t m;
    vault_get_meta(g_confirmed, &m);
    UL_CHECK(strcmp(m.label, "keepme") == 0, "and the label the owner set is untouched");
}

/* An unanswered prompt is a timeout, and must be as safe as a refusal. */
static void test_an_unanswered_prompt_changes_nothing(void) {
    setup(true);
    g_answer = CONFIRM_TIMEOUT;
    char out[512], cmd[128];

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"delete\",\"id\":\"%s\"}", g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strstr(out, "timeout") != NULL, "reported as a timeout, not a decline");
    UL_CHECK(vault_count() == 2, "and nothing was destroyed");
}

/* A device whose panel never came up cannot ask, and a hold nobody can read
 * the meaning of is not consent -- display.h says so, and #64 is what happens
 * when nothing enforces it. What matters just as much here is HOW the
 * dispatcher treated an unfamiliar result: every gate used to name the two
 * refusals and proceed if it was neither, so the safety of every disclosure
 * rested on confirm_result_t never gaining a value. Adding one would have
 * disclosed the secret rather than refused it. They ask for CONFIRM_YES now.
 *
 * Each of these fails on the old code by succeeding. */
static void test_a_device_that_cannot_ask_refuses_everything(void) {
    setup(true);
    /* export_secret has its own hook, so wire it too -- without it the gate
     * is skipped entirely and this would be testing nothing. */
    dispatcher_deps_t deps = {
        .rng = rng_seq, .confirm_action = fake_confirm, .confirm_export = fake_confirm_export};
    dispatcher_init(&deps);
    g_answer = CONFIRM_UNAVAILABLE;
    char out[512], cmd[160];

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"export_secret\",\"id\":\"%s\"}", g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strstr(out, "display_unavailable") != NULL,
             "export is refused when the device cannot show the prompt");
    UL_CHECK(strstr(out, "\"k1\"") == NULL, "and no secret is disclosed");
    UL_CHECK(strstr(out, "user_declined") == NULL,
             "and it does not claim the owner declined -- nobody was asked");

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"delete\",\"id\":\"%s\"}", g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strstr(out, "display_unavailable") != NULL, "delete is refused too");
    UL_CHECK(vault_count() == 2, "and nothing was destroyed");

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"rename\",\"id\":\"%s\",\"label\":\"x\"}",
             g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strstr(out, "display_unavailable") != NULL, "so is rename");
    note_meta_t m;
    vault_get_meta(g_confirmed, &m);
    UL_CHECK(strcmp(m.label, "keepme") == 0, "and the label is untouched");
}

/* ---- the screen has to be able to say what it is asking about ---------- */

/* Approving a delete while believing you are approving an export would be
 * worse than not asking at all, so the action name reaches the gate. */
static void test_the_gate_is_told_which_action(void) {
    char out[512], cmd[160];

    setup(true);
    g_answer = CONFIRM_YES;
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"mark_spent\",\"id\":\"%s\"}", g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strcmp(g_last_action, "mark_spent") == 0, "mark_spent names itself");

    setup(true);
    g_answer = CONFIRM_YES;
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"delete\",\"id\":\"%s\"}", g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strcmp(g_last_action, "delete") == 0, "delete names itself");

    setup(true);
    g_answer = CONFIRM_YES;
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"rename\",\"id\":\"%s\",\"label\":\"x\"}", g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strcmp(g_last_action, "rename") == 0, "rename names itself");
}

/* And which note, so the screen can show the amount at stake. */
static void test_the_gate_is_told_which_note(void) {
    setup(true);
    g_answer = CONFIRM_YES;
    char out[512], cmd[128];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"mark_spent\",\"id\":\"%s\"}", g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));

    UL_CHECK(g_saw_note, "the note is passed to the gate");
    UL_CHECK(strcmp(g_last_note_id, g_confirmed) == 0, "and it is the right one");
}

/* ---- approving still works -------------------------------------------- */

static void test_approval_lets_the_command_through(void) {
    setup(true);
    g_answer = CONFIRM_YES;
    char out[512], cmd[128];

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"mark_spent\",\"id\":\"%s\"}", g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strstr(out, "\"ok\":true") != NULL, "an approved mark_spent succeeds");
    UL_CHECK(state_is(g_confirmed, NOTE_STATE_SPENT), "and the note really is spent");

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"delete\",\"id\":\"%s\"}", g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strstr(out, "\"ok\":true") != NULL, "an approved delete succeeds");
    UL_CHECK(vault_count() == 1, "and the note is gone");
}

/* A malformed request must be rejected before anyone is asked to approve
 * anything -- prompting the owner for a command that was never valid trains
 * them to dismiss prompts. */
static void test_a_bad_request_never_reaches_the_owner(void) {
    setup(true);
    g_answer = CONFIRM_YES;
    char out[512];
    dispatcher_handle("{\"cmd\":\"delete\"}", out, sizeof(out));
    UL_CHECK(strstr(out, "bad_request") != NULL, "a delete with no id is a bad request");
    UL_CHECK(g_asked == 0, "and the owner is never bothered with it");
}

/* Commands that create rather than destroy stay ungated: minting a note costs
 * the owner nothing, and a prompt per mint would make split/merge unusable. */
static void test_creating_is_not_gated(void) {
    setup(true);
    g_answer = CONFIRM_NO;
    char out[1024];

    dispatcher_handle("{\"cmd\":\"new_secret\",\"label\":\"fresh\"}", out, sizeof(out));
    UL_CHECK(strstr(out, "\"ok\":true") != NULL, "new_secret needs no approval");
    UL_CHECK(g_asked == 0, "and asks for none");

    dispatcher_handle("{\"cmd\":\"get_info\"}", out, sizeof(out));
    UL_CHECK(g_asked == 0, "nor does get_info");

    dispatcher_handle("{\"cmd\":\"list_notes\"}", out, sizeof(out));
    UL_CHECK(g_asked == 0, "nor list_notes");
}

/* A build with no way to ask refuses, rather than proceeding ungated.
 *
 * This test used to assert the opposite, on the grounds that the old
 * behaviour "is what keeps the rest of the native suite meaningful". That is
 * no longer so: every test that drives a gated command through the dispatcher
 * now wires its own hook, so nothing else depends on the ungated path. What
 * was left was a gate whose existence was a property of main.c's wiring
 * rather than of the command -- losable by a port or a refactor, with nothing
 * failing to say so.
 *
 * wipe has always refused here, and test_wipe.c argues why: proceeding
 * because no hook happens to be wired turns a build misconfiguration into a
 * remote erase. For export_secret it turns one into a remote disclosure. */
static void test_a_build_that_cannot_ask_refuses(void) {
    setup(false);
    char out[512], cmd[128];

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"mark_spent\",\"id\":\"%s\"}", g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strstr(out, "unsupported") != NULL, "no hook means the command is refused");
    UL_CHECK(state_is(g_confirmed, NOTE_STATE_CONFIRMED), "and the note is untouched");

    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"export_secret\",\"id\":\"%s\"}", g_confirmed);
    dispatcher_handle(cmd, out, sizeof(out));
    UL_CHECK(strstr(out, "unsupported") != NULL, "and so is a disclosure");
    UL_CHECK(strstr(out, "\"k1\"") == NULL, "with no secret handed out");
}

void test_gated_actions_run(void) {
    printf("-- gated destructive actions --\n");
    test_a_stranger_cannot_destroy_a_live_note();
    test_rename_is_gated_too();
    test_an_unanswered_prompt_changes_nothing();
    test_a_device_that_cannot_ask_refuses_everything();
    test_the_gate_is_told_which_action();
    test_the_gate_is_told_which_note();
    test_approval_lets_the_command_through();
    test_a_bad_request_never_reaches_the_owner();
    test_creating_is_not_gated();
    test_a_build_that_cannot_ask_refuses();
}
