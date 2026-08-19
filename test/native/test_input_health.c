/* input_health.c: can this device's buttons be believed?
 *
 * The rule under test is deliberately narrow, and the tests are here to keep
 * it that way. Reading an input released is proof it is not wedged low, and
 * that is the ONLY thing this module ever claims. It must never report a
 * fault for the ordinary approval gesture (a two-second hold), and it must
 * never quietly upgrade "I have seen nothing" into "everything is fine". */
#include <stdio.h>

#include "input_health.h"
#include "unity_lite.h"

#define SECOND_US ((int64_t)1000 * 1000)

/* An input nobody is touching is proven good on the very first tick -- the
 * common case, and it must not need the stuck window to elapse first. */
static void test_a_released_input_is_ok_immediately(void) {
    input_health_t h;
    int64_t now = 500000;
    input_health_init(&h, now);

    input_health_poll(&h, false, false);

    UL_CHECK(input_health_state(&h, INPUT_CONFIRM, now) == INPUT_OK, "confirm reads ok at once");
    UL_CHECK(input_health_state(&h, INPUT_CANCEL, now) == INPUT_OK, "cancel reads ok at once");
    UL_CHECK(!input_health_any_stuck(&h, now), "nothing stuck");
}

/* Held from boot: undecided until the window elapses, then stuck. The
 * undecided phase matters -- reporting "stuck" the moment a pin reads low
 * would libel every board someone powers up with a finger on BOOT. */
static void test_held_from_boot_is_unknown_then_stuck(void) {
    input_health_t h;
    int64_t now = 0;
    input_health_init(&h, now);

    input_health_poll(&h, false, true);

    now += INPUT_HEALTH_STUCK_US - 1;
    input_health_poll(&h, false, true);
    UL_CHECK(input_health_state(&h, INPUT_CANCEL, now) == INPUT_UNKNOWN,
              "not yet judged one tick before the window closes");
    UL_CHECK(!input_health_any_stuck(&h, now), "and not yet reported as a fault");

    now += 2;
    UL_CHECK(input_health_state(&h, INPUT_CANCEL, now) == INPUT_STUCK,
              "past the window with no release ever seen, the pin is stuck");
    UL_CHECK(input_health_any_stuck(&h, now), "and the summary says so");
    UL_CHECK(input_health_state(&h, INPUT_CONFIRM, now) == INPUT_OK,
              "the other input is unaffected");
}

/* A person holding BOOT while the board comes up must not be recorded as a
 * hardware fault. They let go; the input is proven good and stays that way. */
static void test_a_person_holding_boot_is_not_a_fault(void) {
    input_health_t h;
    int64_t now = 0;
    input_health_init(&h, now);

    for (int64_t t = 0; t < 3 * SECOND_US; t += 10000) {
        input_health_poll(&h, true, false); /* button 1 is BOOT on both boards */
    }
    now += 3 * SECOND_US;
    UL_CHECK(input_health_state(&h, INPUT_CONFIRM, now) == INPUT_UNKNOWN,
              "still undecided at three seconds");

    input_health_poll(&h, false, false); /* they let go */
    now += 10 * SECOND_US;
    UL_CHECK(input_health_state(&h, INPUT_CONFIRM, now) == INPUT_OK,
              "released once is proof enough, however long ago");
}

/* The reason INPUT_OK is sticky. Approving anything means holding button 1
 * for two seconds, every time. If a long hold could re-raise the flag, the
 * device would report a fault for working exactly as designed -- and a fault
 * light that comes on during normal use is one nobody reads. */
static void test_a_long_hold_after_release_never_reports_stuck(void) {
    input_health_t h;
    int64_t now = 0;
    input_health_init(&h, now);

    input_health_poll(&h, false, false);

    /* Now hold the approve button for a minute, far past the stuck window. */
    for (int64_t t = 0; t < 60 * SECOND_US; t += 100000) {
        input_health_poll(&h, true, false);
    }
    now += 60 * SECOND_US;

    UL_CHECK(input_health_state(&h, INPUT_CONFIRM, now) == INPUT_OK,
              "a proven input stays proven, however long it is held");
    UL_CHECK(!input_health_any_stuck(&h, now), "and no fault is reported");
}

/* Recovery: a pin flagged stuck that later moves is no longer stuck. Someone
 * unsticking a button, or a fixed board, should not need a power cycle to
 * stop being reported as broken. */
static void test_a_stuck_input_clears_once_it_moves(void) {
    input_health_t h;
    int64_t now = 0;
    input_health_init(&h, now);

    input_health_poll(&h, false, true);
    now += INPUT_HEALTH_STUCK_US + 1;
    UL_CHECK(input_health_state(&h, INPUT_CANCEL, now) == INPUT_STUCK, "stuck first");

    input_health_poll(&h, false, false);
    UL_CHECK(input_health_state(&h, INPUT_CANCEL, now) == INPUT_OK, "and clears when it moves");
}

/* The shape of the real fault, end to end: the ESP32-S3's cancel line reads
 * permanently pressed while its confirm button is fine. What get_info reports
 * for that board is the whole point of this module -- a diagnosis over the
 * wire instead of an afternoon with the board in download mode. */
static void test_the_s3_shape_is_reported_precisely(void) {
    input_health_t h;
    int64_t now = 0;
    input_health_init(&h, now);

    for (int64_t t = 0; t < 10 * SECOND_US; t += 50000) {
        input_health_poll(&h, false, true); /* cancel wedged low, confirm idle */
    }
    now += 10 * SECOND_US;

    UL_CHECK(input_health_state(&h, INPUT_CONFIRM, now) == INPUT_OK, "confirm is fine");
    UL_CHECK(input_health_state(&h, INPUT_CANCEL, now) == INPUT_STUCK, "cancel is not");
}

static void test_names_cover_every_state(void) {
    UL_CHECK(input_health_name(INPUT_OK)[0] == 'o', "ok");
    UL_CHECK(input_health_name(INPUT_STUCK)[0] == 's', "stuck");
    UL_CHECK(input_health_name(INPUT_UNKNOWN)[0] == 'u', "unknown");
    /* Never NULL, so a caller cannot omit the field by accident on a value
     * this enum has not got yet. */
    UL_CHECK(input_health_name((input_state_t)99) != NULL, "an unexpected value still names");
}

/* An out-of-range index must not read off the end of the array. */
static void test_an_out_of_range_index_is_refused(void) {
    input_health_t h;
    input_health_init(&h, 0);
    input_health_poll(&h, false, false);
    UL_CHECK(input_health_state(&h, (input_index_t)INPUT_COUNT, 0) == INPUT_UNKNOWN,
              "the count itself is not an input");
    UL_CHECK(input_health_state(&h, (input_index_t)7, 0) == INPUT_UNKNOWN, "nor is anything past it");
}

void test_input_health_run(void) {
    printf("-- input health --\n");
    test_a_released_input_is_ok_immediately();
    test_held_from_boot_is_unknown_then_stuck();
    test_a_person_holding_boot_is_not_a_fault();
    test_a_long_hold_after_release_never_reports_stuck();
    test_a_stuck_input_clears_once_it_moves();
    test_the_s3_shape_is_reported_precisely();
    test_names_cover_every_state();
    test_an_out_of_range_index_is_refused();
}
