/* What actually lands on the glass.
 *
 * The other display tests check arithmetic, which was individually defensible
 * on all three occasions the screen was nonetheless wrong. These run
 * src/ui/display.c itself against a framebuffer (test/native/hostgfx) and ask
 * about pixels, so a line that is computed but not drawn fails here.
 *
 * Assertions are relational, never absolute: nothing hardcodes a margin, a gap
 * or a y-coordinate, because a test that repeats display.c's arithmetic
 * repeats its bugs too. Each varies one input and insists the pixels move. */
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "font5x7.h"
#include "hostgfx.h"
#include "unity_lite.h"

/* Ink is per state, not always black. Hardcoding 0x0000 here passed happily
 * against a browse card drawing white on purple: zero pixels both times. */
#define INK display_state_ink(DISPLAY_STATE_CONFIRM_PENDING)
#define BROWSE_INK display_state_ink(DISPLAY_STATE_BROWSE)
/* The state colour is no longer the background -- it is the header band and
 * the progress bar. So "was the verb drawn?" is now asked of the band: text
 * in the card's own ground colour EATS accent pixels, and a longer verb eats
 * more. See palette.h. */
#define ACCENT display_state_accent(DISPLAY_STATE_CONFIRM_PENDING)
#define BROWSE_ACCENT display_state_accent(DISPLAY_STATE_BROWSE)

/* Both real panels. A card has to work on the narrow one. */
static const int PANEL_W[2] = {240, 320};
static const int PANEL_H[2] = {135, 170};
#define PANELS 2

static void panel(int i) {
    hostgfx_reset(PANEL_W[i], PANEL_H[i]);
    display_init();
}

/* `hint` is the variable under test; everything else is held fixed. */
static void confirm_card(const char *hint) {
    display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, "SHOW SECRET", "21 000", "sats", "rent",
                        NULL, hint);
}

static void test_the_gesture_hint_reaches_the_glass(void) {
    /* Both cards reserve a hint, so everything else lays out identically and
     * only the hint's own glyphs can differ. Equal counts mean it was not
     * drawn. */
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
    /* Where the bar starts is display.c's business: snapshot the card, draw
     * the bar, see which row moved first. */
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
    /* Fixed scale and position whatever it says, so length is the only free
     * variable. Without it a wipe and a disclosure present identically. */
    for (int i = 0; i < PANELS; i++) {
        panel(i);
        display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, "-", "21 000", "sats", "rent", NULL,
                            "HOLD BTN1 2s");
        const long shortest = hostgfx_ink_pixels(ACCENT);

        panel(i);
        confirm_card("HOLD BTN1 2s");
        const long full = hostgfx_ink_pixels(ACCENT);

        UL_CHECK(shortest > 0, "the band is on the glass at all");
        UL_CHECK(full < shortest, "the verb is drawn into it -- a longer one covers more band");
    }
}

static void test_every_card_has_its_band(void) {
    /* Browse has no verb, and used to get no first line at all -- which left
     * it as a wall of colour with text at the top and a third of the panel
     * empty. The band is the card's spine whether or not there is anything to
     * write on it. */
    for (int i = 0; i < PANELS; i++) {
        panel(i);
        display_note_detail(DISPLAY_STATE_BROWSE, NULL, "21 000", "sats", "rent", "f822a462",
                            NULL);
        UL_CHECK(hostgfx_ink_pixels(BROWSE_ACCENT) > 0, "a browse card still gets a band");

        panel(i);
        display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, "WIPE ALL", NULL, NULL, NULL, NULL,
                            NULL);
        UL_CHECK(hostgfx_ink_pixels(ACCENT) > 0, "so does one with nothing but a verb");
    }
}

static void test_a_card_is_dark_and_an_outcome_is_not(void) {
    /* The whole point of the redesign: things you read up close sit on a dark
     * ground with the colour as structure; things you read across a room are
     * the colour. Asserted as a relationship rather than against literals, so
     * the palette can be retuned without rewriting this. */
    for (int i = 0; i < PANELS; i++) {
        const long area = (long)PANEL_W[i] * PANEL_H[i];

        panel(i);
        confirm_card("HOLD BTN1 2s");
        const long card_accent = hostgfx_ink_pixels(ACCENT);
        UL_CHECK(card_accent > 0 && card_accent < area / 3,
                 "a card wears its colour as a band, not as wallpaper");

        panel(i);
        display_message(DISPLAY_STATE_APPROVED, "APPROVED", "SHOW SECRET", NULL);
        const long field = hostgfx_ink_pixels(display_state_accent(DISPLAY_STATE_APPROVED));
        UL_CHECK(field > area / 2, "an outcome is still a field of colour");
    }
}

static void test_a_card_with_no_note_still_says_both(void) {
    /* WIPE ALL and NEW FIRMWARE have no note behind them. */
    for (int i = 0; i < PANELS; i++) {
        panel(i);
        display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, "WIPE ALL", NULL, NULL, NULL, NULL,
                            "HOLD BTN1 2s");
        const long both = hostgfx_ink_pixels(INK);
        const long named = hostgfx_ink_pixels(ACCENT);

        panel(i);
        display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, NULL, NULL, NULL, NULL, NULL,
                            "HOLD BTN1 2s");
        const long unnamed = hostgfx_ink_pixels(ACCENT);

        UL_CHECK(named < unnamed, "a note-less card still names the action, in the band");
        UL_CHECK(both > 0, "and still says how to approve it");
    }
}

static void test_the_browse_card_shows_which_note(void) {
    /* The chord that follows discloses that note's bearer secret. */
    for (int i = 0; i < PANELS; i++) {
        panel(i);
        display_note_detail(DISPLAY_STATE_BROWSE, NULL, "21 000", "sats", "rent", "-", NULL);
        const long shortest = hostgfx_ink_pixels(display_state_ink_dim(DISPLAY_STATE_BROWSE));

        panel(i);
        display_note_detail(DISPLAY_STATE_BROWSE, NULL, "21 000", "sats", "rent", "f822a462  3",
                            NULL);
        const long full = hostgfx_ink_pixels(display_state_ink_dim(DISPLAY_STATE_BROWSE));

        UL_CHECK(full > shortest, "the note id and position are drawn");
    }
}

static void test_no_line_runs_past_the_panel_edge(void) {
    /* display.c clips rather than wraps, so the failure mode is a line cut
     * mid-glyph at the last column -- which reads as a different label, not a
     * shortened one. */
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
    /* Only the amount is drawn, so the ink band is the digits. 21px was
     * rejected on real hardware as too small to read. */
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
    /* Anything left in the sentinel colour is a region the card never
     * painted, so it still holds the previous screen -- which here can be a
     * QR of a bearer secret. */
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

/* Rough perceptual brightness, 0..255: enough to ask whether text can be read
 * on a background, which black on #383838 got wrong. */
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
    /* A contrast floor rather than a colour list, so a state added later
     * cannot quietly fail it. */
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
    /* Both margins equal, to within the blank column each glyph cell carries.
     * Nothing here knows what the margin is, only that the sides match. */
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
    /* The verb under the title comes off the wire. Wider than the panel must
     * clip, not vanish: draw_centred would compute a negative x, and
     * display_text refuses those outright. */
    for (int i = 0; i < PANELS; i++) {
        const uint16_t ink = display_state_ink(DISPLAY_STATE_EXPIRED);
        panel(i);
        display_message(DISPLAY_STATE_EXPIRED, "NO ANSWER",
                        "A VERB FAR LONGER THAN ANY PANEL HERE", NULL);
        UL_CHECK(hostgfx_ink_pixels(ink) > 0, "an over-long line is clipped, not dropped");
        UL_CHECK(hostgfx_offscreen_pixels() == 0, "and still nothing lands off the panel");
    }
}

static void test_a_message_survives_a_panel_too_small_for_it(void) {
    /* The T-Dongle-S3 is 80x160 and a dead panel reports 0. Neither may draw
     * off the edge, leave a gap, or wedge trying to centre something that
     * does not fit. */
    static const int TINY[][2] = {{80, 160}, {80, 40}, {40, 30}};
    for (size_t i = 0; i < sizeof(TINY) / sizeof(TINY[0]); i++) {
        hostgfx_reset(TINY[i][0], TINY[i][1]);
        display_init();
        display_message(DISPLAY_STATE_EXPIRED, "NO ANSWER", "SHOW SECRET", "NOTHING DONE");
        UL_CHECK(hostgfx_offscreen_pixels() == 0, "nothing drawn off a small panel");
        UL_CHECK(hostgfx_ink_pixels(HOSTGFX_UNPAINTED) == 0, "and none of it left unpainted");
    }
}

static void test_a_message_with_nothing_to_say_still_clears_the_screen(void) {
    /* Every caller passes at least a title today, but the screen underneath
     * can be a QR of a bearer secret, so an empty message must not leave it
     * there. */
    for (int i = 0; i < PANELS; i++) {
        panel(i);
        display_note_detail(DISPLAY_STATE_BROWSE, NULL, "21 000", "sats", "rent", "f822a462",
                            NULL);
        display_message(DISPLAY_STATE_IDLE, NULL, NULL, NULL);
        UL_CHECK(hostgfx_ink_pixels(display_state_ink(DISPLAY_STATE_BROWSE)) == 0,
                 "the previous screen is gone");
        UL_CHECK(hostgfx_ink_pixels(HOSTGFX_UNPAINTED) == 0, "and the panel is fully painted");
    }
}

void test_card_render_run(void) {
    printf("-- card render --\n");
    test_the_gesture_hint_reaches_the_glass();
    test_the_gesture_hint_clears_the_progress_bar();
    test_the_verb_reaches_the_glass();
    test_every_card_has_its_band();
    test_a_card_is_dark_and_an_outcome_is_not();
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
    test_a_message_survives_a_panel_too_small_for_it();
    test_a_message_with_nothing_to_say_still_clears_the_screen();
}
