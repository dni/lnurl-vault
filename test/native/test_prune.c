/* Forgetting the dead notes, and everything it must refuse to touch.
 *
 * `delete` takes one id and every gated command costs a physical hold, so
 * clearing a few dozen spent notes meant a few dozen deliberate two-second
 * holds -- housekeeping nobody does, on a device that then fills with dead
 * weight until list_notes starts refusing pages for it. This is the same
 * removal done once.
 *
 * Which makes the interesting tests the ones about what it does NOT remove. A
 * bulk delete that is slightly too eager destroys money, and the report it
 * would produce -- "removed 26" instead of "removed 25" -- is not something
 * anyone would notice until the note they wanted was gone. So: PENDING and
 * CONFIRMED survive, a declined prompt changes nothing, and an absent gate
 * refuses rather than proceeds. */
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
static uint32_t g_told_count;

static confirm_result_t fake_prune_approve(uint32_t count, uint32_t timeout_ms) {
    (void)timeout_ms;
    g_asked++;
    g_told_count = count;
    return g_answer;
}

/* Three of each state, interleaved rather than grouped, so a compaction that
 * drops the wrong neighbour shows up. */
static char g_confirmed[3][VAULT_ID_BUF];
static char g_spent[3][VAULT_ID_BUF];
static char g_pending[3][VAULT_ID_BUF];

static void setup(bool gated) {
    dispatcher_deps_t deps = {.rng = rng_seq};
    if (gated) {
        deps.prune_approve = fake_prune_approve;
    }
    dispatcher_init(&deps);
    vault_init(NULL, NULL);
    g_seq = 7;
    g_asked = 0;
    g_told_count = 0;
    g_answer = CONFIRM_YES;

    char h[VAULT_HASH_HEX_BUF];
    for (int i = 0; i < 3; i++) {
        char label[16];
        snprintf(label, sizeof(label), "keep%d", i);
        vault_new_secret(rng_seq, NULL, 0, label, g_confirmed[i], h);
        vault_confirm(g_confirmed[i], 21000, "mint.example/w", NULL);

        snprintf(label, sizeof(label), "dead%d", i);
        vault_new_secret(rng_seq, NULL, 0, label, g_spent[i], h);
        vault_confirm(g_spent[i], 50000, "mint.example/w", NULL);
        vault_mark_spent(g_spent[i]);

        snprintf(label, sizeof(label), "wait%d", i);
        vault_new_secret(rng_seq, NULL, 0, label, g_pending[i], h);
    }
}

static bool holds(const char *id) {
    note_meta_t m;
    return vault_get_meta(id, &m);
}

static void test_it_counts_only_the_dead(void) {
    setup(true);
    UL_CHECK(vault_count() == 9, "nine notes to start");
    UL_CHECK(vault_count_spent() == 3, "three of them spent");
}

static void test_it_removes_the_dead_and_nothing_else(void) {
    setup(true);
    size_t removed = 0;
    UL_CHECK(vault_prune_spent(&removed) == VAULT_OK, "the sweep succeeds");
    UL_CHECK(removed == 3, "and reports three removed");
    UL_CHECK(vault_count() == 6, "six notes left");
    UL_CHECK(vault_count_spent() == 0, "and none of them spent");

    for (int i = 0; i < 3; i++) {
        UL_CHECK(!holds(g_spent[i]), "a spent note is gone");
        UL_CHECK(holds(g_confirmed[i]), "a CONFIRMED note survives -- it is money");
        UL_CHECK(holds(g_pending[i]), "a PENDING note survives -- it may yet confirm");
    }
}

static void test_the_survivors_keep_their_details(void) {
    /* Compaction moves notes down the array. A shift that copied the wrong
     * neighbour would leave the right COUNT of notes carrying the wrong
     * amounts and labels, which no count-based assertion can catch. */
    setup(true);
    UL_CHECK(vault_prune_spent(NULL) == VAULT_OK, "the sweep succeeds");
    for (int i = 0; i < 3; i++) {
        note_meta_t m;
        char want[16];
        UL_CHECK(vault_get_meta(g_confirmed[i], &m), "the confirmed note is still there");
        snprintf(want, sizeof(want), "keep%d", i);
        UL_CHECK(strcmp(m.label, want) == 0, "with its own label");
        UL_CHECK(m.amount_msat == 21000, "and its own amount");
        UL_CHECK(m.state == NOTE_STATE_CONFIRMED, "and its own state");
    }
}

static void test_a_second_sweep_finds_nothing(void) {
    setup(true);
    UL_CHECK(vault_prune_spent(NULL) == VAULT_OK, "first sweep");
    size_t removed = 99;
    UL_CHECK(vault_prune_spent(&removed) == VAULT_OK, "second sweep still succeeds");
    UL_CHECK(removed == 0, "and removes nothing");
    UL_CHECK(vault_count() == 6, "leaving the six alone");
}

/* --- through the command surface ----------------------------------------- */

static void test_the_command_asks_first_and_says_how_many(void) {
    setup(true);
    char out[512];
    dispatcher_handle("{\"cmd\":\"prune_spent\"}", out, sizeof(out));
    UL_CHECK(g_asked == 1, "it asked");
    UL_CHECK(g_told_count == 3, "and told the card how many -- the reviewable fact");
    UL_CHECK(strstr(out, "\"ok\":true") != NULL, "and reports success");
    UL_CHECK(strstr(out, "\"removed\":3") != NULL, "with the number removed");
    UL_CHECK(strstr(out, "\"remaining\":6") != NULL, "and the number left");
    UL_CHECK(vault_count() == 6, "and actually removed them");
}

static void test_a_declined_prompt_changes_nothing(void) {
    setup(true);
    g_answer = CONFIRM_NO;
    char out[512];
    dispatcher_handle("{\"cmd\":\"prune_spent\"}", out, sizeof(out));
    UL_CHECK(g_asked == 1, "it asked");
    UL_CHECK(strstr(out, "user_declined") != NULL, "and reported the refusal");
    UL_CHECK(vault_count() == 9, "with every note still there");
    UL_CHECK(vault_count_spent() == 3, "including the spent ones");
}

static void test_an_unanswered_prompt_changes_nothing(void) {
    setup(true);
    g_answer = CONFIRM_TIMEOUT;
    char out[512];
    dispatcher_handle("{\"cmd\":\"prune_spent\"}", out, sizeof(out));
    UL_CHECK(strstr(out, "\"ok\":true") == NULL, "a timeout is not success");
    UL_CHECK(vault_count() == 9, "and nothing was removed");
}

static void test_no_gate_means_no_sweep(void) {
    /* A gate that disappears because a dependency is NULL is not a gate. The
     * same sentence test_wipe.c writes down, for the same reason: a build
     * misconfiguration must not become a remote delete. */
    setup(false);
    char out[512];
    dispatcher_handle("{\"cmd\":\"prune_spent\"}", out, sizeof(out));
    UL_CHECK(strstr(out, "unsupported") != NULL, "an ungated build refuses");
    UL_CHECK(vault_count() == 9, "and removes nothing");
}

static void test_nothing_to_do_does_not_prompt(void) {
    /* Asking somebody to approve a no-op is how people learn to approve
     * without reading, on a device where the next prompt hands over a bearer
     * secret. */
    setup(true);
    char out[512];
    dispatcher_handle("{\"cmd\":\"prune_spent\"}", out, sizeof(out));
    const int asked_once = g_asked;
    dispatcher_handle("{\"cmd\":\"prune_spent\"}", out, sizeof(out));
    UL_CHECK(g_asked == asked_once, "a second sweep with nothing to do never asks");
    UL_CHECK(strstr(out, "\"ok\":true") != NULL, "and still answers ok");
    UL_CHECK(strstr(out, "\"removed\":0") != NULL, "having removed nothing");
}

static void test_it_cannot_be_talked_into_taking_a_live_note(void) {
    /* The whole risk in one test: no argument, no id, no amount of asking
     * makes this touch a CONFIRMED note. It takes no parameters at all, which
     * is the point -- there is nothing to aim it with. */
    setup(true);
    char out[512];
    dispatcher_handle("{\"cmd\":\"prune_spent\",\"id\":\"whatever\",\"all\":true}", out,
                       sizeof(out));
    for (int i = 0; i < 3; i++) {
        UL_CHECK(holds(g_confirmed[i]), "a CONFIRMED note survives an id it did not ask for");
        UL_CHECK(holds(g_pending[i]), "so does a PENDING one");
    }
    UL_CHECK(vault_count() == 6, "only the three dead notes went");
}

void test_prune_run(void) {
    printf("\n-- forgetting spent notes --\n");
    test_it_counts_only_the_dead();
    test_it_removes_the_dead_and_nothing_else();
    test_the_survivors_keep_their_details();
    test_a_second_sweep_finds_nothing();
    test_the_command_asks_first_and_says_how_many();
    test_a_declined_prompt_changes_nothing();
    test_an_unanswered_prompt_changes_nothing();
    test_no_gate_means_no_sweep();
    test_nothing_to_do_does_not_prompt();
    test_it_cannot_be_talked_into_taking_a_live_note();
}
