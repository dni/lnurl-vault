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

/* Both buttons released for a moment, which is what actually happens between
 * a prompt appearing and somebody reaching for a button. approval_begin()
 * treats a button already down as stale until it has been seen released -- see
 * approval.h -- so a test that presses from the very first tick is modelling a
 * held-over or stuck button, not a person answering. The cases that mean to
 * model that say so. */
static void settle(approval_t *a, int64_t *now);

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

static void settle(approval_t *a, int64_t *now) {
    run(a, false, false, now, 50 * 1000);
}

static void test_a_tap_is_not_enough(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);
    settle(&a, &now);

    /* The old gesture: a brief press of button 1. */
    run(&a, true, false, &now, 120 * 1000);
    approval_state_t st = run(&a, false, false, &now, 500 * 1000);

    UL_CHECK(st == APPROVAL_PENDING, "a tap neither approves nor denies");
}

static void test_a_full_hold_approves(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);
    settle(&a, &now);

    approval_state_t st = run(&a, true, false, &now, APPROVAL_HOLD_US + TICK_US);
    UL_CHECK(st == APPROVAL_GRANTED, "holding button 1 for the full period approves");
}

static void test_hold_just_short_does_not_approve(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);
    settle(&a, &now);

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
    settle(&a, &now);

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
    settle(&a, &now);

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
    settle(&a, &now);

    approval_state_t st = run(&a, false, true, &now, 400 * 1000);
    UL_CHECK(st == APPROVAL_DENIED, "button 2 denies");
}

/* Button 2 must never be an alternative way to say yes, however long it is
 * held -- on a two-button device with no labels that is an easy thing to get
 * subtly wrong. */
static void test_button_2_never_approves(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);
    settle(&a, &now);

    approval_state_t st = run(&a, false, true, &now, APPROVAL_HOLD_US * 2);
    UL_CHECK(st == APPROVAL_DENIED, "holding button 2 denies, however long it is held");
}

/* Both at once resolves toward not disclosing. */
static void test_cancel_wins_over_a_simultaneous_hold(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);
    settle(&a, &now);

    run(&a, true, false, &now, APPROVAL_HOLD_US - (20 * 1000)); /* nearly there */
    approval_state_t st = run(&a, true, true, &now, 400 * 1000); /* now both down */

    UL_CHECK(st == APPROVAL_DENIED, "a cancel arriving with a completing hold still cancels");
}

/* Bounce on the cancel button must not throw away a request the owner has
 * not answered. */
static void test_cancel_bounce_does_not_deny(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);
    settle(&a, &now);

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
    settle(&a, &now);

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
    settle(&a, &now);

    approval_state_t st = run(&a, true, false, &now, APPROVAL_HOLD_US + TICK_US);
    UL_CHECK(st == APPROVAL_GRANTED, "a hold underway when the window lapses may complete");
}

/* But the grace must not let a prompt sit open indefinitely on a jammed or
 * taped-down button. */
static void test_the_grace_is_bounded(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 100);
    settle(&a, &now);

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
    settle(&a, &now);

    run(&a, false, true, &now, 400 * 1000);
    UL_CHECK(a.state == APPROVAL_DENIED, "denied");

    /* Holding afterwards must not talk the machine back into approving. */
    approval_state_t st = run(&a, true, false, &now, APPROVAL_HOLD_US * 2);
    UL_CHECK(st == APPROVAL_DENIED, "a resolved approval never changes its mind");
}

static void test_progress_fills_and_does_not_stutter_on_bounce(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);
    settle(&a, &now);

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

/* ---- a held button is not a decision ---------------------------------- */

/* The one that came from hardware. On an ESP32-S3 the cancel button read as
 * permanently pressed -- wrong pin, or a board revision that moved it -- and
 * every prompt was refused in under a second with nobody touching the device.
 * export_secret could not succeed at all.
 *
 * A stuck button must not be able to answer. It cannot be made to approve
 * anything either, so the prompt simply lapses, which is the safe outcome and
 * a visible one. */
static void test_a_stuck_cancel_button_cannot_answer(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 300);

    /* Held down from before the prompt existed and never released. */
    approval_state_t st = run(&a, false, true, &now, 600 * 1000);

    UL_CHECK(st != APPROVAL_DENIED, "a button stuck down since before the prompt does not deny");
    UL_CHECK(st == APPROVAL_EXPIRED, "the prompt lapses instead, which is safe and visible");
}

/* The other side of the same rule, and the one with teeth: an owner still
 * holding button 1 from approving one request must not silently approve the
 * next request that arrives. They decided about the first one, not this one. */
static void test_a_still_held_approve_button_cannot_approve_the_next_prompt(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    /* Never released since the previous prompt. */
    approval_state_t st = run(&a, true, false, &now, APPROVAL_HOLD_US * 3);
    UL_CHECK(st == APPROVAL_PENDING, "a button held over from the last prompt approves nothing");

    /* Let go, then hold again: that is a fresh decision and must work. */
    run(&a, false, false, &now, 100 * 1000);
    st = run(&a, true, false, &now, APPROVAL_HOLD_US + TICK_US);
    UL_CHECK(st == APPROVAL_GRANTED, "releasing and holding again does approve");
}

/* Cancel behaves the same way: released first, then pressed, is a decision. */
static void test_cancel_after_release_still_works(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    run(&a, false, true, &now, 400 * 1000);   /* held from before: ignored */
    UL_CHECK(a.state == APPROVAL_PENDING, "still pending");

    run(&a, false, false, &now, 100 * 1000);  /* released */
    approval_state_t st = run(&a, false, true, &now, 400 * 1000); /* pressed afresh */
    UL_CHECK(st == APPROVAL_DENIED, "a fresh cancel press denies");
}

/* The question section 7a of the hardware checklist never answered, and the
 * one that decides whether the ESP32-S3 is usable at all.
 *
 * That record -- export_secret refused in 0.92s with nobody touching the
 * board -- was taken against firmware 0.0.2-25-g0b3477c, which PREDATES the
 * fresh-press rule above (it arrived with hold-to-approve, #33). So it
 * describes logic this file no longer contains. What it does not tell us is
 * what the S3 does NOW, and there are two possibilities that look identical
 * from the outside: the wedged cancel line is merely ignored, or it also
 * blocks approval, in which case that board can still never disclose
 * anything and the fault is just quieter.
 *
 * It is the first. cancel_stale never clears on a pin that never reads
 * released, so cancel_down is never set, so nothing stands in the hold's way.
 * A board with a dead cancel button can still approve -- it has simply lost
 * the ability to say no, which is what input_health.c is for reporting.
 *
 * This is a claim about the logic, not a bench result. It is here so that the
 * claim is checked every time the suite runs, rather than believed. */
static void test_a_wedged_cancel_line_does_not_block_a_real_approval(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    /* Cancel wedged low since before the prompt and never released -- the S3.
     * The owner has not touched button 1 yet. */
    run(&a, false, true, &now, 200 * 1000);
    UL_CHECK(a.state == APPROVAL_PENDING, "the wedged line has not answered anything");

    /* Now they hold the approve button, properly, for the full two seconds. */
    approval_state_t st = run(&a, true, true, &now, APPROVAL_HOLD_US + TICK_US);
    UL_CHECK(st == APPROVAL_GRANTED,
              "a real hold still approves past a permanently wedged cancel line");
}

/* The other half of that board's story, and the half nobody checked.
 *
 * The logic above is fine: the wedged line is ignored and a hold still
 * approves. What the owner is TOLD was not. approval_waiting_for_release()
 * used to report either button being stale, cancel_stale never clears on a pin
 * that never reads released, and ui_task.c draws RELEASE_HINT -- "LET GO
 * FIRST" -- whenever it is true. So every gated command on an S3 showed a card
 * instructing its owner to let go, for the whole 30 seconds, and never once
 * named the two-second hold that would have worked. Doing as the screen said
 * guaranteed the timeout.
 *
 * Reported from the field as "I can connect in the wallet but rotating
 * fails", which is exactly what this looks like from the other end. */
static void test_a_wedged_cancel_does_not_ask_the_owner_to_let_go(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);

    /* Cancel wedged low since before the prompt, button 1 untouched -- the S3
     * sitting there waiting to be approved. */
    run(&a, false, true, &now, 200 * 1000);
    UL_CHECK(!approval_waiting_for_release(&a),
              "a wedged cancel line must not put LET GO FIRST on the card");

    /* And the hint still has to work for the case it was written for: a
     * button 1 held over from the last prompt really does stop the bar. */
    approval_t b;
    approval_begin(&b, now, 30000);
    run(&b, true, false, &now, 200 * 1000);
    UL_CHECK(approval_waiting_for_release(&b),
              "an approve button held over from the last prompt still says so");
    run(&b, false, false, &now, 100 * 1000);
    UL_CHECK(!approval_waiting_for_release(&b), "and stops saying so once released");
}

/* Both stuck at once must still lapse rather than resolve either way. */
static void test_both_stuck_lapses(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 300);

    approval_state_t st = run(&a, true, true, &now, 800 * 1000);
    UL_CHECK(st == APPROVAL_EXPIRED, "two stuck buttons decide nothing");
}

/* A brief glitch on the cancel line must not refuse a request. Measured on a
 * classic T-Display: a confirm over serial, then one over BLE, and the second
 * came back refused in about a second with nobody near the device -- while the
 * same prompt on a fresh boot timed out correctly at 31s. Not a stuck line;
 * something makes that input read pressed briefly when the radio is busy,
 * which is precisely what board_t_display.c warns about for GPIO35 (input-only
 * on the classic ESP32, no internal pull resistor at all).
 *
 * A person does not tap cancel for 60ms. A glitch does. */
static void test_a_brief_glitch_does_not_cancel(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);
    settle(&a, &now);

    for (int i = 0; i < 6; i++) {
        run(&a, false, true, &now, 60 * 1000);   /* a glitch-length blip */
        run(&a, false, false, &now, 200 * 1000);
    }
    UL_CHECK(a.state == APPROVAL_PENDING, "repeated brief blips do not refuse the request");

    /* And a real press still does. */
    approval_state_t st = run(&a, false, true, &now, 400 * 1000);
    UL_CHECK(st == APPROVAL_DENIED, "a press of human length still denies");
}

/* The glitch must not steal an approval either: a blip part-way through a hold
 * cannot be allowed to refuse what the owner is in the middle of granting. */
static void test_a_glitch_does_not_interrupt_a_hold(void) {
    approval_t a;
    int64_t now = 1000000;
    approval_begin(&a, now, 30000);
    settle(&a, &now);

    run(&a, true, false, &now, 800 * 1000);
    run(&a, true, true, &now, 60 * 1000);      /* blip while still holding */
    approval_state_t st = run(&a, true, false, &now, APPROVAL_HOLD_US);

    UL_CHECK(st == APPROVAL_GRANTED, "a blip during a hold neither cancels nor restarts it");
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
    test_a_stuck_cancel_button_cannot_answer();
    test_a_still_held_approve_button_cannot_approve_the_next_prompt();
    test_cancel_after_release_still_works();
    test_a_wedged_cancel_line_does_not_block_a_real_approval();
    test_a_wedged_cancel_does_not_ask_the_owner_to_let_go();
    test_both_stuck_lapses();
    test_a_brief_glitch_does_not_cancel();
    test_a_glitch_does_not_interrupt_a_hold();
}
