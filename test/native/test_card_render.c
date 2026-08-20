/* What actually lands on the glass.
 *
 * Every other display test in this suite checks arithmetic -- what scale a
 * line should get, how much room the lines below it need. That arithmetic was
 * individually defensible on each of the three occasions the screen was
 * nonetheless wrong, and the wrongness was only ever found by a person
 * looking at a board: digits too small to read, the whole panel mirrored, and
 * a hold hint that was measured, budgeted for, and then silently dropped one
 * pixel past the bottom edge because the reserving code and the advancing
 * code used gaps that differed by one.
 *
 * These tests run src/ui/display.c itself -- the real drawing, the real font,
 * the real panel geometries -- against a framebuffer (test/native/hostgfx),
 * and ask about pixels. A line that is computed but not drawn fails here.
 *
 * The assertions are deliberately relational rather than absolute. Nothing
 * here hardcodes a margin, a gap or a y-coordinate: those are display.c's
 * business and a test that repeats them just repeats its bugs too. Instead
 * each check varies ONE input and insists the pixels change accordingly --
 * a longer hint must put more ink on screen than a shorter one, and cannot do
 * that if the hint is not drawn at all.
 */
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "font5x7.h"
#include "hostgfx.h"
#include "unity_lite.h"

/* Text is not always black: display.c picks ink per state, because black on
 * the dark grey idle background is close to invisible. Ask it rather than
 * assuming -- a test that hardcoded 0x0000 here passed happily against a
 * browse card drawing white on purple, counting zero pixels both times. */
#define INK display_state_ink(DISPLAY_STATE_CONFIRM_PENDING)
#define BROWSE_INK display_state_ink(DISPLAY_STATE_BROWSE)

/* Both real panels, in the orientation src/board/ hands up: the classic
 * T-Display and the T-Display-S3. A card has to work on the narrow one. */
static const int PANEL_W[2] = {240, 320};
static const int PANEL_H[2] = {135, 170};
#define PANELS 2

static void panel(int i) {
    hostgfx_reset(PANEL_W[i], PANEL_H[i]);
    display_init();
}

/* A representative confirm card: the verb, an amount that needs every digit,
 * a unit, a label, and the gesture. `hint` is the variable under test. */
static void confirm_card(const char *hint) {
    display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, "SHOW SECRET", "21 000", "sats", "rent",
                        NULL, hint);
}

static void test_the_gesture_hint_reaches_the_glass(void) {
    /* The regression this file exists for. Both cards reserve room for a
     * hint, so the amount, the unit and the label are laid out identically in
     * each -- the ONLY thing that can differ in the pixel count is the hint's
     * own glyphs. Equal counts mean the hint was not drawn. */
    for (int i = 0; i < PANELS; i++) {
        panel(i);
        confirm_card("-");
        const long shortest = hostgfx_ink_pixels(INK);

        panel(i);
        confirm_card("HOLD BTN1 2s");
        const long full = hostgfx_ink_pixels(INK);

        UL_CHECK(shortest > 0, "the confirm card draws something at all");
        UL_CHECK(full > shortest, "the hold hint is drawn, not merely budgeted for");
    }
}

static void test_the_gesture_hint_clears_the_progress_bar(void) {
    /* The hint is the lowest line on the card and the bar is drawn over the
     * band beneath it. Where that band starts is display.c's business, so ask
     * rather than assume: snapshot the card, draw the bar, and see which row
     * moved first. */
    for (int i = 0; i < PANELS; i++) {
        panel(i);
        confirm_card("HOLD BTN1 2s");
        const int bottom = hostgfx_last_ink_row(INK);
        hostgfx_snapshot();
        display_progress(1000);
        const int bar_top = hostgfx_first_changed_row();

        UL_CHECK(bottom >= 0, "the card has ink on it");
        UL_CHECK(bar_top > 0, "the progress bar draws somewhere");
        UL_CHECK(bar_top > bottom, "the bar starts below the last line of the card");
    }
}

static void test_the_verb_reaches_the_glass(void) {
    /* Same trick on the action line. It is drawn at a fixed scale and a fixed
     * position whatever it says, so length is the only free variable. Without
     * this line a wipe of every note and the disclosure of one note's secret
     * present identically. */
    for (int i = 0; i < PANELS; i++) {
        panel(i);
        display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, "-", "21 000", "sats", "rent", NULL,
                            "HOLD BTN1 2s");
        const long shortest = hostgfx_ink_pixels(INK);

        panel(i);
        confirm_card("HOLD BTN1 2s");
        const long full = hostgfx_ink_pixels(INK);

        UL_CHECK(full > shortest, "the verb is drawn");
    }
}

static void test_a_card_with_no_note_still_says_both(void) {
    /* WIPE ALL and NEW FIRMWARE have no note behind them. They used to be a
     * flat colour and nothing else. */
    for (int i = 0; i < PANELS; i++) {
        panel(i);
        display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, "WIPE ALL", NULL, NULL, NULL, NULL,
                            "HOLD BTN1 2s");
        const long both = hostgfx_ink_pixels(INK);

        panel(i);
        display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, "WIPE ALL", NULL, NULL, NULL, NULL,
                            NULL);
        const long verb_only = hostgfx_ink_pixels(INK);

        UL_CHECK(verb_only > 0, "a note-less card still names the action");
        UL_CHECK(both > verb_only, "and still says how to approve it");
    }
}

static void test_the_browse_card_shows_which_note(void) {
    /* Browsing is where the id earns its row: the chord that follows
     * discloses that note's bearer secret. */
    for (int i = 0; i < PANELS; i++) {
        panel(i);
        display_note_detail(DISPLAY_STATE_BROWSE, NULL, "21 000", "sats", "rent", "-", NULL);
        const long shortest = hostgfx_ink_pixels(BROWSE_INK);

        panel(i);
        display_note_detail(DISPLAY_STATE_BROWSE, NULL, "21 000", "sats", "rent", "f822a462  3",
                            NULL);
        const long full = hostgfx_ink_pixels(BROWSE_INK);

        UL_CHECK(full > shortest, "the note id and position are drawn");
    }
}

static void test_no_line_runs_past_the_panel_edge(void) {
    /* Labels arrive over the wire and amounts can be seven digits. display.c
     * clips rather than wraps, so the failure mode is a line that reaches the
     * last column and is cut mid-glyph -- "card-check" rendered as
     * "card-chec" plus two columns of an 'k'. Nothing should get within a few
     * pixels of the edge on either panel. */
    static const char *const LABELS[] = {
        "rent",
        "a label far longer than any panel can hold, on and on and on",
        "\xf0\x9f\x92\xa9 unprintable",
    };
    for (int i = 0; i < PANELS; i++) {
        for (size_t l = 0; l < sizeof(LABELS) / sizeof(LABELS[0]); l++) {
            panel(i);
            display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, "SHOW SECRET", "2 100 000",
                                "sats", LABELS[l], NULL, "HOLD BTN1 2s");
            const int right = hostgfx_last_ink_col(INK);
            UL_CHECK(right > 0, "something is drawn");
            UL_CHECK(right < PANEL_W[i] - 2, "no line reaches the panel edge");
            UL_CHECK(hostgfx_offscreen_pixels() == 0, "nothing is drawn off the panel");
        }
    }
}

static void test_the_card_never_draws_below_the_readable_minimum(void) {
    /* Only the amount is drawn, so the ink band IS the digits: its height is
     * the glyph height times the scale. A person on real hardware rejected
     * 21 pixels ("the text on the screen is too small for me to read"), which
     * is the minimum scale -- an ordinary amount should be getting more than
     * that on both panels. */
    for (int i = 0; i < PANELS; i++) {
        panel(i);
        display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, NULL, "21 000", NULL, NULL, NULL,
                            "HOLD BTN1 2s");
        const int top = hostgfx_first_ink_row(INK);
        const int bottom = hostgfx_last_ink_row(INK);
        UL_CHECK(top >= 0 && bottom > top, "the amount is on screen");
        const int height = bottom - top + 1;
        UL_CHECK(height >= FONT5X7_HEIGHT * FONT5X7_MIN_READABLE_SCALE,
                 "the amount is at least the readable minimum");
        UL_CHECK(height > FONT5X7_HEIGHT * FONT5X7_MIN_READABLE_SCALE,
                 "an ordinary amount gets more than the bare minimum");
    }
}

static void test_the_whole_panel_gets_painted(void) {
    /* The framebuffer starts as a colour nothing in this firmware draws, so
     * any of it left over is a region the card never painted. A card that
     * paints only part of the screen leaves whatever the previous screen had
     * there -- which on this device can be the last note's amount, or a QR of
     * a bearer secret. */
    for (int i = 0; i < PANELS; i++) {
        panel(i);
        UL_CHECK(hostgfx_ink_pixels(HOSTGFX_UNPAINTED) == 0,
                 "display_init leaves no unpainted pixel");
        confirm_card("HOLD BTN1 2s");
        UL_CHECK(hostgfx_ink_pixels(HOSTGFX_UNPAINTED) == 0,
                 "a confirm card leaves no unpainted pixel");
        display_progress(400);
        UL_CHECK(hostgfx_ink_pixels(HOSTGFX_UNPAINTED) == 0,
                 "the progress bar leaves no unpainted pixel");
    }
}


/* --- messages: outcomes, boot, idle --------------------------------------- */

/* Rough perceptual brightness of an RGB565 colour, 0..255. Enough to answer
 * "can this text be read on that background", which is the question a card
 * drawing black on #383838 got wrong. */
static int luma565(uint16_t c) {
    const int r = (int)(((c >> 11) & 0x1Fu) << 3);
    const int g = (int)(((c >> 5) & 0x3Fu) << 2);
    const int b = (int)((c & 0x1Fu) << 3);
    return (r * 77 + g * 150 + b * 29) >> 8;
}

static const display_state_t STATES[] = {
    DISPLAY_STATE_IDLE,     DISPLAY_STATE_BROWSE,   DISPLAY_STATE_CONFIRM_PENDING,
    DISPLAY_STATE_APPROVED, DISPLAY_STATE_DECLINED, DISPLAY_STATE_EXPIRED,
};
#define STATE_COUNT (sizeof(STATES) / sizeof(STATES[0]))

static void test_every_state_draws_ink_you_can_read(void) {
    /* The bug this pins: every card drew black regardless of what it drew on,
     * which is fine on amber and green and close to invisible on the dark grey
     * the device rests on -- a screen that had no text on it when that colour
     * was chosen, and now does. */
    for (size_t i = 0; i < STATE_COUNT; i++) {
        const int bg = luma565(display_state_color(STATES[i]));
        const int ink = luma565(display_state_ink(STATES[i]));
        const int delta = bg > ink ? bg - ink : ink - bg;
        UL_CHECK(delta >= 90, "ink contrasts with the background it is drawn on");
    }
}

static void test_a_message_draws_every_line_it_is_given(void) {
    for (int i = 0; i < PANELS; i++) {
        const uint16_t ink = display_state_ink(DISPLAY_STATE_DECLINED);

        panel(i);
        display_message(DISPLAY_STATE_DECLINED, "DECLINED", NULL, NULL);
        const long one = hostgfx_ink_pixels(ink);

        panel(i);
        display_message(DISPLAY_STATE_DECLINED, "DECLINED", "SHOW SECRET", NULL);
        const long two = hostgfx_ink_pixels(ink);

        panel(i);
        display_message(DISPLAY_STATE_DECLINED, "DECLINED", "SHOW SECRET", "NOTHING DONE");
        const long three = hostgfx_ink_pixels(ink);

        UL_CHECK(one > 0, "the title is drawn");
        UL_CHECK(two > one, "the second line is drawn");
        UL_CHECK(three > two, "and so is the third");
    }
}

static void test_a_message_is_centred(void) {
    /* Both margins equal, to within the blank column each glyph cell carries
     * on its right. Nothing here knows what the margin is -- only that the two
     * sides match, which is what "centred" means and what a layout drifting
     * off one edge stops being. */
    for (int i = 0; i < PANELS; i++) {
        const uint16_t ink = display_state_ink(DISPLAY_STATE_APPROVED);
        panel(i);
        display_message(DISPLAY_STATE_APPROVED, "APPROVED", "SHOW SECRET", NULL);
        const int left = hostgfx_first_ink_col(ink);
        const int right = hostgfx_last_ink_col(ink);
        UL_CHECK(left > 0 && right > left, "the message is on screen");
        const int rgap = PANEL_W[i] - 1 - right;
        const int skew = left > rgap ? left - rgap : rgap - left;
        UL_CHECK(skew <= FONT5X7_MIN_READABLE_SCALE * 2,
                 "the block is centred, not hanging off one side");

        const int top = hostgfx_first_ink_row(ink);
        const int bottom = hostgfx_last_ink_row(ink);
        const int bgap = PANEL_H[i] - 1 - bottom;
        const int vskew = top > bgap ? top - bgap : bgap - top;
        UL_CHECK(vskew <= FONT5X7_HEIGHT * FONT5X7_MIN_READABLE_SCALE,
                 "and vertically too, rather than hanging off the top");
    }
}

static void test_a_message_paints_the_whole_panel(void) {
    for (int i = 0; i < PANELS; i++) {
        panel(i);
        display_message(DISPLAY_STATE_EXPIRED, "NO ANSWER", "WIPE ALL", "NOTHING DONE");
        UL_CHECK(hostgfx_ink_pixels(HOSTGFX_UNPAINTED) == 0,
                 "a message leaves no unpainted pixel");
        UL_CHECK(hostgfx_offscreen_pixels() == 0, "and draws nothing off the panel");
    }
}

static void test_a_message_too_wide_for_the_panel_still_draws(void) {
    /* Titles come from this codebase, but the verb under them comes off the
     * wire via dispatcher.c. A line wider than the panel must clip at the
     * edge, not vanish -- draw_centred computes a negative x for it, and
     * display_text refuses to draw at a negative x at all. */
    for (int i = 0; i < PANELS; i++) {
        const uint16_t ink = display_state_ink(DISPLAY_STATE_EXPIRED);
        panel(i);
        display_message(DISPLAY_STATE_EXPIRED, "NO ANSWER",
                        "A VERB FAR LONGER THAN ANY PANEL HERE", NULL);
        UL_CHECK(hostgfx_ink_pixels(ink) > 0, "an over-long line is clipped, not dropped");
        UL_CHECK(hostgfx_offscreen_pixels() == 0, "and still nothing lands off the panel");
    }
}

void test_card_render_run(void) {
    printf("-- card render --\n");
    test_the_gesture_hint_reaches_the_glass();
    test_the_gesture_hint_clears_the_progress_bar();
    test_the_verb_reaches_the_glass();
    test_a_card_with_no_note_still_says_both();
    test_the_browse_card_shows_which_note();
    test_no_line_runs_past_the_panel_edge();
    test_the_card_never_draws_below_the_readable_minimum();
    test_the_whole_panel_gets_painted();
    test_every_state_draws_ink_you_can_read();
    test_a_message_draws_every_line_it_is_given();
    test_a_message_is_centred();
    test_a_message_paints_the_whole_panel();
    test_a_message_too_wide_for_the_panel_still_draws();
}
