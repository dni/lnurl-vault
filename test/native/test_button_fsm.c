#include "button_fsm.h"
#include "unity_lite.h"

static void test_simple_tap(void) {
    button_fsm_t fsm;
    button_fsm_init(&fsm);
    int64_t t = 0;

    UL_CHECK(button_fsm_poll(&fsm, true, false, t) == BTN_EVENT_NONE, "press b1: no event yet");
    t += 50 * 1000;
    UL_CHECK(button_fsm_poll(&fsm, true, false, t) == BTN_EVENT_NONE, "still held: no event");
    t += 10 * 1000; /* 60ms held total, well past the 30ms debounce */
    UL_CHECK(button_fsm_poll(&fsm, false, false, t) == BTN_EVENT_1_TAP,
              "release after debounce fires a tap");
    UL_CHECK(button_fsm_poll(&fsm, false, false, t) == BTN_EVENT_NONE,
              "no repeat event while both stay released");

    button_fsm_init(&fsm);
    t = 0;
    button_fsm_poll(&fsm, false, true, t);
    t += 60 * 1000;
    UL_CHECK(button_fsm_poll(&fsm, false, false, t) == BTN_EVENT_2_TAP, "button 2 taps too");
}

static void test_bounce_filtered(void) {
    button_fsm_t fsm;
    button_fsm_init(&fsm);
    int64_t t = 0;

    button_fsm_poll(&fsm, true, false, t);
    t += 5 * 1000; /* well under the 30ms debounce */
    UL_CHECK(button_fsm_poll(&fsm, false, false, t) == BTN_EVENT_NONE,
              "a too-short press is filtered as a bounce, not reported as a tap");
}

static void test_chord(void) {
    button_fsm_t fsm;
    button_fsm_init(&fsm);
    int64_t t = 0;

    button_fsm_poll(&fsm, true, true, t);
    t += 100 * 1000; /* under the 200ms chord hold */
    UL_CHECK(button_fsm_poll(&fsm, true, true, t) == BTN_EVENT_NONE,
              "both held but under the chord threshold: no event yet");
    t += 150 * 1000; /* 250ms total, past the threshold */
    UL_CHECK(button_fsm_poll(&fsm, true, true, t) == BTN_EVENT_BOTH_CHORD,
              "chord fires once the threshold is met");
    UL_CHECK(button_fsm_poll(&fsm, true, true, t) == BTN_EVENT_NONE,
              "chord does not refire while both stay held");
    UL_CHECK(button_fsm_poll(&fsm, false, false, t + 10000) == BTN_EVENT_NONE,
              "releasing after a chord does not also fire trailing taps for either button");
}

static void test_staggered_chord_entry(void) {
    button_fsm_t fsm;
    button_fsm_init(&fsm);
    int64_t t = 0;

    button_fsm_poll(&fsm, true, false, t); /* b1 down alone first */
    t += 250 * 1000;                       /* b1 held long alone, well past its own threshold */
    UL_CHECK(button_fsm_poll(&fsm, true, false, t) == BTN_EVENT_NONE,
              "b1 held alone, however long, produces no event without a release");

    button_fsm_poll(&fsm, true, true, t); /* b2 joins */
    t += 250 * 1000;                      /* b2 now also past its own hold threshold */
    UL_CHECK(button_fsm_poll(&fsm, true, true, t) == BTN_EVENT_BOTH_CHORD,
              "a staggered chord entry still fires once the later button catches up");

    t += 10000;
    button_fsm_poll(&fsm, false, false, t); /* release both: chord re-arms */
    t += 10000;
    button_fsm_poll(&fsm, true, false, t); /* fresh press of b1 alone */
    t += 50000;
    UL_CHECK(button_fsm_poll(&fsm, false, false, t) == BTN_EVENT_1_TAP,
              "a fresh tap works normally after a prior chord and full release");
}

void test_button_fsm_run(void) {
    test_simple_tap();
    test_bounce_filtered();
    test_chord();
    test_staggered_chord_entry();
}
