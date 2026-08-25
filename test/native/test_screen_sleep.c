/* When the screen goes dark, and what it takes to bring it back.
 *
 * Two halves, and both are worth having. src/proto/screen_sleep.c is the
 * clock, tested a tick at a time for the same reason approval.c is -- every
 * interesting case here is a timing case, and a screen that blanks a second
 * early or refuses to wake is miserable to characterise on glass. src/ui/
 * display.c is the light, tested against test/native/hostgfx so that "went
 * dark" means the framebuffer AND the backlight, not one of them.
 *
 * The invariant this cannot test from here, and which matters most, is that
 * a live confirmation never goes dark. That one is structural: ui_task.c only
 * asks screen_sleep_expired() from its main loop, and while a prompt is up
 * that loop is inside service_remote_confirm() and not running. There is no
 * flag to get wrong, so there is nothing here to assert about. */
#include <stdint.h>
#include <stdio.h>

#include "display.h"
#include "hostgfx.h"
#include "screen_sleep.h"
#include "unity_lite.h"

#define TIMEOUT_MS 60000
#define TIMEOUT_US ((int64_t)TIMEOUT_MS * 1000)

/* Not zero, and not round: a state machine that happens to work only because
 * its clock starts at 0 has been written more than once. */
#define T0 ((int64_t)1234567890)

static void test_a_fresh_screen_is_lit(void) {
    screen_sleep_t s;
    screen_sleep_init(&s, T0, TIMEOUT_MS);
    UL_CHECK(!screen_sleep_is_asleep(&s), "a screen starts lit");
    UL_CHECK(!screen_sleep_expired(&s, T0), "and does not blank on the tick it started");
}

static void test_it_blanks_when_the_timeout_is_up_and_not_before(void) {
    screen_sleep_t s;
    screen_sleep_init(&s, T0, TIMEOUT_MS);
    UL_CHECK(!screen_sleep_expired(&s, T0 + TIMEOUT_US - 1), "still lit one microsecond short");
    UL_CHECK(!screen_sleep_is_asleep(&s), "and still reports itself lit");
    UL_CHECK(screen_sleep_expired(&s, T0 + TIMEOUT_US), "blanks on the tick the timeout is up");
    UL_CHECK(screen_sleep_is_asleep(&s), "and reports itself dark afterwards");
}

static void test_the_blank_is_an_edge_not_a_level(void) {
    /* ui_task calls this every 30ms forever. Reporting true on each of them
     * would repaint a black screen fifty times a second, and would make
     * "should I blank?" and "am I blanked?" the same question. */
    screen_sleep_t s;
    screen_sleep_init(&s, T0, TIMEOUT_MS);
    UL_CHECK(screen_sleep_expired(&s, T0 + TIMEOUT_US), "blanks once");
    UL_CHECK(!screen_sleep_expired(&s, T0 + TIMEOUT_US + 1), "and not again on the next tick");
    UL_CHECK(!screen_sleep_expired(&s, T0 + TIMEOUT_US * 100), "nor a long time later");
}

static void test_a_touch_restarts_the_clock(void) {
    screen_sleep_t s;
    screen_sleep_init(&s, T0, TIMEOUT_MS);
    const int64_t late = T0 + TIMEOUT_US - 1;
    UL_CHECK(!screen_sleep_touch(&s, late), "touching a lit screen does not report a wake");
    UL_CHECK(!screen_sleep_expired(&s, late + TIMEOUT_US - 1), "the whole timeout starts again");
    UL_CHECK(screen_sleep_expired(&s, late + TIMEOUT_US), "and runs out from the touch, not the start");
}

static void test_only_the_touch_that_wakes_it_says_so(void) {
    /* ui_task turns the backlight on exactly when this returns true, so a
     * second true would light an already-lit screen -- harmless -- and a
     * missing one would leave a repainted card invisible, which is not. */
    screen_sleep_t s;
    screen_sleep_init(&s, T0, TIMEOUT_MS);
    (void)screen_sleep_expired(&s, T0 + TIMEOUT_US);
    UL_CHECK(screen_sleep_touch(&s, T0 + TIMEOUT_US + 1), "the touch that wakes it reports the wake");
    UL_CHECK(!screen_sleep_is_asleep(&s), "and it is lit again");
    UL_CHECK(!screen_sleep_touch(&s, T0 + TIMEOUT_US + 2), "the next touch reports nothing");
}

static void test_a_woken_screen_gets_a_full_timeout_again(void) {
    screen_sleep_t s;
    screen_sleep_init(&s, T0, TIMEOUT_MS);
    (void)screen_sleep_expired(&s, T0 + TIMEOUT_US);
    const int64_t woke = T0 + TIMEOUT_US + 1;
    (void)screen_sleep_touch(&s, woke);
    UL_CHECK(!screen_sleep_expired(&s, woke + TIMEOUT_US - 1), "lit for the whole window again");
    UL_CHECK(screen_sleep_expired(&s, woke + TIMEOUT_US), "then dark again");
}

static void test_a_zero_timeout_never_blanks(void) {
    /* What a board whose panel did not come up is given: there is nothing to
     * blank, and a device that thinks it is dark eats the first press of
     * every gesture. */
    screen_sleep_t s;
    screen_sleep_init(&s, T0, 0);
    UL_CHECK(!screen_sleep_expired(&s, T0 + TIMEOUT_US * 1000), "a zero timeout stays lit forever");
    UL_CHECK(!screen_sleep_is_asleep(&s), "and never reports itself dark");
}

static void test_a_clock_that_does_not_move_does_not_blank(void) {
    screen_sleep_t s;
    screen_sleep_init(&s, T0, TIMEOUT_MS);
    for (int i = 0; i < 100; i++) {
        UL_CHECK(!screen_sleep_expired(&s, T0), "a stopped clock never reaches the timeout");
    }
    /* And backwards, which is what a caller passing the wrong clock looks
     * like. Blanking on it would be the wrong way to fail. */
    UL_CHECK(!screen_sleep_expired(&s, T0 - TIMEOUT_US * 10), "nor does one running backwards");
}

static void test_a_huge_timeout_does_not_overflow(void) {
    /* The timeout arrives in 32 bits of milliseconds and is compared in 64
     * bits of microseconds. Multiply before widening and the largest values
     * wrap to something small, which blanks the screen almost immediately --
     * the opposite of what was asked for. */
    screen_sleep_t s;
    screen_sleep_init(&s, T0, UINT32_MAX);
    UL_CHECK(!screen_sleep_expired(&s, T0 + 1000), "a month-long timeout is not up after a millisecond");
    UL_CHECK(!screen_sleep_expired(&s, T0 + (int64_t)UINT32_MAX * 1000 - 1), "nor just short of it");
    UL_CHECK(screen_sleep_expired(&s, T0 + (int64_t)UINT32_MAX * 1000), "and is up when it is up");
}

/* --- the light itself ---------------------------------------------------- */

static void panel(void) {
    hostgfx_reset(240, 135);
    display_init();
}

/* Anything that is not black, anywhere. display_sleep() has to leave none of
 * it: the point is to stop holding the same card in the same crystal, which
 * killing the backlight alone does not do. */
static long lit_pixels(void) {
    long n = 0;
    for (int y = 0; y < hostgfx_height(); y++) {
        for (int x = 0; x < hostgfx_width(); x++) {
            if (hostgfx_pixel(x, y) != 0x0000) {
                n++;
            }
        }
    }
    return n;
}

static void test_sleeping_takes_both_the_light_and_the_pixels(void) {
    panel();
    display_message(DISPLAY_STATE_IDLE, "3 NOTES", "TAP TO VIEW", NULL);
    UL_CHECK(lit_pixels() > 0, "the resting card is on the glass to begin with");
    UL_CHECK(hostgfx_backlight(), "and the panel is lit");

    display_sleep();
    UL_CHECK(display_asleep(), "the screen reports itself dark");
    UL_CHECK(!hostgfx_backlight(), "the backlight is off");
    UL_CHECK(lit_pixels() == 0, "and the card is gone, not merely unlit");
}

static void test_waking_lights_the_panel_without_drawing(void) {
    /* display.c deliberately does not repaint on wake; ui_task draws first
     * and lights second, so the owner never sees the card from a minute ago.
     * If wake ever started painting, this is where it would show up. */
    panel();
    display_message(DISPLAY_STATE_IDLE, "3 NOTES", "TAP TO VIEW", NULL);
    display_sleep();
    display_wake();
    UL_CHECK(!display_asleep(), "the screen reports itself lit");
    UL_CHECK(hostgfx_backlight(), "the backlight is back on");
    UL_CHECK(lit_pixels() == 0, "and waking drew nothing of its own");
}

static void test_drawing_while_dark_stays_dark(void) {
    /* ui_task repaints the resting card before it asks for the light. That
     * repaint must not light the panel by itself, or the order stops
     * mattering and the stale-card flash comes back. */
    panel();
    display_sleep();
    display_message(DISPLAY_STATE_IDLE, "3 NOTES", "TAP TO VIEW", NULL);
    UL_CHECK(!hostgfx_backlight(), "drawing does not turn the backlight on");
    UL_CHECK(display_asleep(), "and does not clear the dark flag");
    UL_CHECK(lit_pixels() > 0, "the card is composed, ready for the light");
}

static void test_sleep_and_wake_are_idempotent(void) {
    panel();
    display_sleep();
    display_sleep();
    UL_CHECK(!hostgfx_backlight(), "sleeping twice leaves it dark");
    display_wake();
    display_wake();
    UL_CHECK(hostgfx_backlight(), "waking twice leaves it lit");
    display_wake();
    UL_CHECK(hostgfx_backlight(), "and waking an already-lit screen is a no-op");
}

static void test_a_fresh_panel_starts_lit(void) {
    /* display_init() runs again on every boot, and repeatedly in these tests.
     * A dark flag left over from a previous life means a vault that boots to
     * a black screen and stays there. */
    panel();
    display_sleep();
    panel();
    UL_CHECK(!display_asleep(), "bringing the panel up clears the dark flag");
    UL_CHECK(hostgfx_backlight(), "and the board's own bring-up leaves it lit");
}

void test_screen_sleep_run(void) {
    printf("\n-- screen sleep --\n");
    test_a_fresh_screen_is_lit();
    test_it_blanks_when_the_timeout_is_up_and_not_before();
    test_the_blank_is_an_edge_not_a_level();
    test_a_touch_restarts_the_clock();
    test_only_the_touch_that_wakes_it_says_so();
    test_a_woken_screen_gets_a_full_timeout_again();
    test_a_zero_timeout_never_blanks();
    test_a_clock_that_does_not_move_does_not_blank();
    test_a_huge_timeout_does_not_overflow();
    test_sleeping_takes_both_the_light_and_the_pixels();
    test_waking_lights_the_panel_without_drawing();
    test_drawing_while_dark_stays_dark();
    test_sleep_and_wake_are_idempotent();
    test_a_fresh_panel_starts_lit();
}
