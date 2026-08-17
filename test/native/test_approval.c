/* Drives the approval gesture a tick at a time.
 *
 * Every case here is a way a physical approval gate goes wrong rather than a
 * way the code goes wrong, which is the point of keeping the logic free of
 * GPIO: on hardware most of these need a button pressed to within a few
 * milliseconds of something, and a failure looks like "it felt weird once". */
#include <stdio.h>

#include "approval.h"
#include "unity_lite.h"

#define TICK_US 10000 /* 10ms, comfortably finer than ui_task's 30ms poll */

/* Runs the machine forward for `duration_us` holding the given buttons,
 * returning the state it settled in. */
static approval_state_t run(approval_t *a, bool approve, bool cancel, int64_t *now,
                             int64_t duration_us) {
    int64_t until = *now + duration_us;
    approval_state_t st = a->state;
    while (*now < until) {
        st = approval_poll(a, approve, cancel, *now);
        *now += TICK_US;
    }
    return st;
}

static void test_a_tap_is_not_enough(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    /* The old gesture: a brief press of button 1. */
    run(&a, true, false, &now, 120 * 1000);
    approval_state_t st = run(&a, false, false, &now, 500 * 1000);

    UL_CHECK(st == APPROVAL_PENDING, "a tap neither approves nor denies");
}

static void test_a_full_hold_approves(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    approval_state_t st = run(&a, true, false, &now, APPROVAL_HOLD_US + TICK_US);
    UL_CHECK(st == APPROVAL_GRANTED, "holding button 1 for the full period approves");
}

static void test_hold_just_short_does_not_approve(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    approval_state_t st = run(&a, true, false, &now, APPROVAL_HOLD_US - (50 * 1000));
    UL_CHECK(st == APPROVAL_PENDING, "just short of the full hold is not an approval");
}

/* The one the issue calls out by name. Mechanical buttons chatter; a hold
 * that restarted at every skipped contact would feel broken, and would teach
 * the owner to mash the button at a screen that discloses secrets. */
static void test_bounce_mid_hold_does_not_break_the_hold(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    run(&a, true, false, &now, 900 * 1000); /* most of the way there */

    /* Contact skips for 20ms -- under APPROVAL_RELEASE_BOUNCE_US. */
    run(&a, false, false, &now, 20 * 1000);

    /* Finishing the ORIGINAL hold, not a fresh one: only enough time to
     * complete the first hold is given. If the bounce had restarted it, this
     * would still be pending. */
    approval_state_t st = run(&a, true, false, &now, APPROVAL_HOLD_US - (900 * 1000));
    UL_CHECK(st == APPROVAL_GRANTED, "a brief contact skip does not restart the hold");
}

static void test_real_release_abandons_the_hold(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    run(&a, true, false, &now, 900 * 1000);
    run(&a, false, false, &now, 300 * 1000); /* well past the bounce window */

    /* Same remaining time as the bounce case above. This time it must NOT be
     * enough, because the hold genuinely restarts. */
    approval_state_t st = run(&a, true, false, &now, APPROVAL_HOLD_US - (900 * 1000));
    UL_CHECK(st == APPROVAL_PENDING, "letting go really does restart the hold");

    /* And crucially it did not deny: a slipped finger is not a decision. */
    UL_CHECK(a.state != APPROVAL_DENIED, "letting go early never denies -- that is button 2's job");

    /* Still answerable afterwards. */
    st = run(&a, true, false, &now, APPROVAL_HOLD_US + TICK_US);
    UL_CHECK(st == APPROVAL_GRANTED, "and the owner can simply hold again");
}

static void test_button_2_denies(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    approval_state_t st = run(&a, false, true, &now, 200 * 1000);
    UL_CHECK(st == APPROVAL_DENIED, "button 2 denies");
}

/* Button 2 must never be an alternative way to say yes, however long it is
 * held -- on a two-button device with no labels that is an easy thing to get
 * subtly wrong. */
static void test_button_2_never_approves(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    approval_state_t st = run(&a, false, true, &now, APPROVAL_HOLD_US * 2);
    UL_CHECK(st == APPROVAL_DENIED, "holding button 2 denies, however long it is held");
}

/* Both at once resolves toward not disclosing. */
static void test_cancel_wins_over_a_simultaneous_hold(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    run(&a, true, false, &now, APPROVAL_HOLD_US - (20 * 1000)); /* nearly there */
    approval_state_t st = run(&a, true, true, &now, 100 * 1000); /* now both down */

    UL_CHECK(st == APPROVAL_DENIED, "a cancel arriving with a completing hold still cancels");
}

/* Bounce on the cancel button must not throw away a request the owner has
 * not answered. */
static void test_cancel_bounce_does_not_deny(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    for (int i = 0; i < 5; i++) {
        run(&a, false, true, &now, 10 * 1000); /* under the debounce */
        run(&a, false, false, &now, 50 * 1000);
    }
    UL_CHECK(a.state == APPROVAL_PENDING, "chatter on the cancel button is not a decision");
}

/* A timeout is its own outcome. The screen has to be able to say the prompt
 * went stale rather than leaving something that looks live. */
static void test_timeout_is_distinct_from_denial(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 200); /* 200ms */

    approval_state_t st = run(&a, false, false, &now, 400 * 1000);
    UL_CHECK(st == APPROVAL_EXPIRED, "nobody answering expires");
    UL_CHECK(st != APPROVAL_DENIED, "and is not reported as a decision to decline");
}

/* Someone part-way through a hold when the window lapses gets to finish.
 * Losing it at 1.9s of a 2s hold is indistinguishable, from the owner's
 * side, from the device ignoring them. */
static void test_a_hold_in_progress_at_the_deadline_may_finish(void) {
    approval_t a;
    int64_t now = 1000000;
    /* Deadline lands 1s into a 2s hold. */
    approval_begin(&a, now, 1000);

    approval_state_t st = run(&a, true, false, &now, APPROVAL_HOLD_US + TICK_US);
    UL_CHECK(st == APPROVAL_GRANTED, "a hold underway when the window lapses may complete");
}

/* But the grace must not let a prompt sit open indefinitely on a jammed or
 * taped-down button. */
static void test_the_grace_is_bounded(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 100);

    /* Approve is held, but the hold keeps being restarted by real releases,
     * so it never completes. The prompt must still expire. */
    for (int i = 0; i < 40; i++) {
        run(&a, true, false, &now, 300 * 1000);
        run(&a, false, false, &now, 300 * 1000);
    }
    UL_CHECK(a.state == APPROVAL_EXPIRED, "a prompt nobody completes expires regardless");
}

static void test_state_is_terminal(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    run(&a, false, true, &now, 200 * 1000);
    UL_CHECK(a.state == APPROVAL_DENIED, "denied");

    /* Holding afterwards must not talk the machine back into approving. */
    approval_state_t st = run(&a, true, false, &now, APPROVAL_HOLD_US * 2);
    UL_CHECK(st == APPROVAL_DENIED, "a resolved approval never changes its mind");
}

static void test_progress_fills_and_does_not_stutter_on_bounce(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    UL_CHECK(approval_progress_permille(&a, now) == 0, "no progress before anything is held");

    run(&a, true, false, &now, APPROVAL_HOLD_US / 2);
    uint16_t half = approval_progress_permille(&a, now);
    UL_CHECK(half > 400 && half < 600, "progress is about half way through a half hold");

    /* A skipped contact must not drop the bar back to zero. */
    run(&a, false, false, &now, 20 * 1000);
    uint16_t during_bounce = approval_progress_permille(&a, now);
    UL_CHECK(during_bounce >= half, "the bar does not stutter while a bounce is filtered");

    run(&a, true, false, &now, APPROVAL_HOLD_US);
    UL_CHECK(approval_progress_permille(&a, now) == 1000, "full once granted");
}

void test_approval_run(void) {
    printf("-- approval --\n");
    test_a_tap_is_not_enough();
    test_a_full_hold_approves();
    test_hold_just_short_does_not_approve();
    test_bounce_mid_hold_does_not_break_the_hold();
    test_real_release_abandons_the_hold();
    test_button_2_denies();
    test_button_2_never_approves();
    test_cancel_wins_over_a_simultaneous_hold();
    test_cancel_bounce_does_not_deny();
    test_timeout_is_distinct_from_denial();
    test_a_hold_in_progress_at_the_deadline_may_finish();
    test_the_grace_is_bounded();
    test_state_is_terminal();
    test_progress_fills_and_does_not_stutter_on_bounce();
}
