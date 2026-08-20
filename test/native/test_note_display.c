/* The strings on the approval screen.
 *
 * Issue #9: before this, approving a disclosure meant looking at a plain amber
 * screen and being told nothing -- not which note, not for how much. Now the
 * screen carries an amount and a label, which makes these strings part of the
 * security control rather than decoration. An amount a factor of a thousand
 * out, or ungrouped so 21000 and 210000 look alike, is a wrong number at the
 * exact moment somebody is deciding whether to hand over money. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "font5x7.h"
#include "note_display.h"
#include "unity_lite.h"

static void expect_amount(uint64_t msat, const char *want) {
    char buf[NOTE_AMOUNT_BUF];
    note_format_amount(msat, buf, sizeof(buf));
    char msg[160];
    snprintf(msg, sizeof(msg), "%llu msat renders as \"%s\" (got \"%s\")",
             (unsigned long long)msat, want, buf);
    UL_CHECK(strcmp(buf, want) == 0, msg);
}

static void test_whole_sats(void) {
    expect_amount(0, "0 sats");
    expect_amount(1000, "1 sat");           /* singular, because a person reads it */
    expect_amount(2000, "2 sats");
    expect_amount(21000, "21 sats");
    expect_amount(210000, "210 sats");      /* must not look like 21 sats */
    expect_amount(1000000, "1 000 sats");
    expect_amount(2100000, "2 100 sats");
    expect_amount(100000000000, "100 000 000 sats");
}

/* Rounding a sub-sat remainder away would put an amount on screen that the
 * note does not carry. Show msat instead and say so. */
static void test_sub_sat_amounts_are_not_rounded(void) {
    expect_amount(1, "1 msat");
    expect_amount(999, "999 msat");
    expect_amount(1500, "1 500 msat");
    expect_amount(21001, "21 001 msat");
}

/* The grouping is the whole point of the format: a magnitude misread is the
 * failure that costs money, and these four differ only in length. */
static void test_magnitudes_are_visibly_different(void) {
    char a[NOTE_AMOUNT_BUF], b[NOTE_AMOUNT_BUF], c[NOTE_AMOUNT_BUF], d[NOTE_AMOUNT_BUF];
    note_format_amount(21000, a, sizeof(a));
    note_format_amount(210000, b, sizeof(b));
    note_format_amount(2100000, c, sizeof(c));
    note_format_amount(21000000, d, sizeof(d));
    UL_CHECK(strcmp(a, "21 sats") == 0, "21 sats");
    UL_CHECK(strcmp(b, "210 sats") == 0, "210 sats");
    UL_CHECK(strcmp(c, "2 100 sats") == 0, "2 100 sats");
    UL_CHECK(strcmp(d, "21 000 sats") == 0, "21 000 sats");
    UL_CHECK(strcmp(a, b) && strcmp(b, c) && strcmp(c, d), "all four are distinct strings");
}

static void test_largest_amount_fits(void) {
    char buf[NOTE_AMOUNT_BUF];
    note_format_amount(UINT64_MAX, buf, sizeof(buf));
    UL_CHECK(strlen(buf) < sizeof(buf), "the largest possible amount fits its buffer");
    UL_CHECK(strstr(buf, "msat") != NULL, "and is labelled (UINT64_MAX is not a whole sat)");
}

/* A short buffer must truncate safely rather than run off the end -- the
 * caller here is drawing code with a fixed-size stack buffer. */
static void test_short_buffers_are_safe(void) {
    char tiny[4];
    memset(tiny, 'X', sizeof(tiny));
    note_format_amount(2100000, tiny, sizeof(tiny));
    UL_CHECK(tiny[sizeof(tiny) - 1] == '\0' || strlen(tiny) < sizeof(tiny),
             "a short buffer is still NUL-terminated");

    char one[1];
    one[0] = 'X';
    note_format_amount(1000, one, sizeof(one));
    UL_CHECK(one[0] == '\0', "a one-byte buffer gets just a terminator");

    note_format_amount(1000, NULL, 0); /* must not crash */
    UL_CHECK(true, "a NULL buffer is ignored rather than dereferenced");
}

/* The digits and the unit are drawn at different sizes, so the drawing code
 * needs them apart. A person on real hardware could not read the first
 * version, where the whole string shared one scale and " sats" ate five
 * characters of a line that has to fit in 240 pixels. */
static void test_amount_splits_into_digits_and_unit(void) {
    char num[NOTE_AMOUNT_BUF], unit[8];

    note_format_amount_parts(21000, num, sizeof(num), unit, sizeof(unit));
    UL_CHECK(strcmp(num, "21") == 0 && strcmp(unit, "sats") == 0, "21000 msat -> \"21\" + \"sats\"");

    note_format_amount_parts(1000, num, sizeof(num), unit, sizeof(unit));
    UL_CHECK(strcmp(num, "1") == 0 && strcmp(unit, "sat") == 0, "singular unit survives the split");

    note_format_amount_parts(100000000, num, sizeof(num), unit, sizeof(unit));
    UL_CHECK(strcmp(num, "100 000") == 0 && strcmp(unit, "sats") == 0, "grouping survives the split");

    note_format_amount_parts(1500, num, sizeof(num), unit, sizeof(unit));
    UL_CHECK(strcmp(num, "1 500") == 0 && strcmp(unit, "msat") == 0, "sub-sat keeps its msat unit");

    note_format_amount_parts(0, num, sizeof(num), unit, sizeof(unit));
    UL_CHECK(strcmp(num, "0") == 0 && strcmp(unit, "sats") == 0, "zero");

    /* The split must never disagree with the single-string form. */
    char whole[NOTE_AMOUNT_BUF];
    /* Room for the longest number, a space, the longest unit and a NUL --
     * sized so GCC's -Wformat-truncation is satisfied that it cannot cut. */
    char joined[NOTE_AMOUNT_BUF + 16];
    const uint64_t cases[] = {0, 1, 999, 1000, 2000, 21000, 2100000, 100000000, UINT64_MAX};
    bool agree = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        note_format_amount(cases[i], whole, sizeof(whole));
        note_format_amount_parts(cases[i], num, sizeof(num), unit, sizeof(unit));
        snprintf(joined, sizeof(joined), "%s %s", num, unit);
        if (strcmp(whole, joined) != 0) {
            agree = false;
        }
    }
    UL_CHECK(agree, "the split rejoined always equals the single-string form");

    /* Tiny buffers must not overrun. */
    char tn[2], tu[2];
    note_format_amount_parts(100000000, tn, sizeof(tn), tu, sizeof(tu));
    UL_CHECK(tn[1] == '\0' && tu[1] == '\0', "short buffers stay terminated");
    note_format_amount_parts(1000, NULL, 0, NULL, 0);
    UL_CHECK(true, "NULL buffers are ignored rather than dereferenced");
}

/* ---- labels ------------------------------------------------------------ */

static void expect_label(const char *in, const char *want) {
    char buf[64];
    note_format_label(in, buf, sizeof(buf));
    char msg[192];
    snprintf(msg, sizeof(msg), "label %s renders as \"%s\" (got \"%s\")",
             in ? "given" : "NULL", want, buf);
    UL_CHECK(strcmp(buf, want) == 0, msg);
}

static void test_ordinary_labels(void) {
    expect_label("rent", "rent");
    expect_label("Coffee #3", "Coffee #3");
}

/* An empty line reads as "the screen did not load", not as "this note has no
 * label". Say which it is. */
static void test_missing_label_is_visible(void) {
    expect_label("", "(no label)");
    expect_label(NULL, "(no label)");
}

/* A label arrives over the wire and is drawn at the moment of a disclosure
 * decision. Unprintable bytes become a visible '?' rather than being dropped:
 * dropping them would let two different labels display identically. */
static void test_unprintable_bytes_become_visible(void) {
    expect_label("a\nb", "a?b");
    expect_label("a\tb", "a?b");
    expect_label("caf\xc3\xa9", "caf??");   /* UTF-8 is not renderable by a 5x7 ASCII font */
    expect_label("\x01\x02\x03", "???");
}

/* Two labels differing only in an unprintable byte must not render the same,
 * which is what dropping the byte would do. */
static void test_labels_do_not_collide(void) {
    char a[64], b[64];
    note_format_label("pay", a, sizeof(a));
    note_format_label("pay\x01", b, sizeof(b));
    UL_CHECK(strcmp(a, b) != 0, "a trailing control byte is visible, not swallowed");
}

static void test_long_label_is_truncated_safely(void) {
    char in[256];
    memset(in, 'A', sizeof(in) - 1);
    in[sizeof(in) - 1] = '\0';
    char out[16];
    note_format_label(in, out, sizeof(out));
    UL_CHECK(strlen(out) == sizeof(out) - 1, "truncated to fit");
    UL_CHECK(out[sizeof(out) - 1] == '\0', "and terminated");
}

/* ---- the font ---------------------------------------------------------- */

/* Every byte value must map to a glyph without reading past the table. The
 * label path above feeds this function attacker-influenced bytes. */
static void test_every_byte_has_a_glyph(void) {
    bool all_present = true;
    for (int c = 0; c < 256; c++) {
        const uint8_t *g = font5x7_glyph((char)c);
        if (!g) {
            all_present = false;
        }
    }
    UL_CHECK(all_present, "every one of 256 byte values returns a glyph, never NULL");

    /* Out-of-range renders as '?', which is what makes that safe. */
    const uint8_t *q = font5x7_glyph('?');
    UL_CHECK(memcmp(font5x7_glyph((char)0x01), q, FONT5X7_WIDTH) == 0,
             "a control byte renders as '?'");
    UL_CHECK(memcmp(font5x7_glyph((char)0xFF), q, FONT5X7_WIDTH) == 0,
             "so does a high byte");
}

/* Spot-check actual shapes, so a mistyped table byte fails here rather than
 * being noticed on a screen. Digits first: they are what an amount is made
 * of. Rows are bits 0..6 of each column byte. */
static void test_glyph_shapes(void) {
    /* A blank space really is blank -- otherwise every gap has speckle. */
    const uint8_t *sp = font5x7_glyph(' ');
    bool blank = true;
    for (int i = 0; i < FONT5X7_WIDTH; i++) {
        if (sp[i] != 0) {
            blank = false;
        }
    }
    UL_CHECK(blank, "space is empty");

    /* '0' is a closed ring: top and bottom rows have their middle set, and
     * the outer columns are solid down the middle rows. */
    const uint8_t *zero = font5x7_glyph('0');
    UL_CHECK(zero[0] == 0x3E && zero[4] == 0x3E, "'0' has matching solid sides");
    UL_CHECK(zero[2] == 0x49, "'0' has its diagonal");

    /* '1' has a single tall stem. */
    const uint8_t *one = font5x7_glyph('1');
    UL_CHECK(one[2] == 0x7F, "'1' has a full-height stem");

    /* '8' is symmetric left-to-right, which a mistyped byte would break. */
    const uint8_t *eight = font5x7_glyph('8');
    UL_CHECK(eight[0] == eight[4], "'8' is symmetric");

    /* Every digit must differ from every other digit. Two digits sharing a
     * bitmap is exactly the bug that puts a wrong amount on screen. */
    bool all_distinct = true;
    for (char a = '0'; a <= '9'; a++) {
        for (char b = (char)(a + 1); b <= '9'; b++) {
            if (memcmp(font5x7_glyph(a), font5x7_glyph(b), FONT5X7_WIDTH) == 0) {
                all_distinct = false;
            }
        }
    }
    UL_CHECK(all_distinct, "no two digits share a bitmap");

    /* And no two printable characters at all collide, other than by design. */
    int collisions = 0;
    for (int a = FONT5X7_FIRST_CHAR; a <= FONT5X7_LAST_CHAR; a++) {
        for (int b = a + 1; b <= FONT5X7_LAST_CHAR; b++) {
            if (a == ' ' || b == ' ') {
                continue;
            }
            if (memcmp(font5x7_glyph((char)a), font5x7_glyph((char)b), FONT5X7_WIDTH) == 0) {
                collisions++;
            }
        }
    }
    UL_CHECK(collisions == 0, "no two printable glyphs are identical");
}

/* Nothing may set the eighth bit: only seven rows exist, and a stray bit 7
 * would draw a row outside the cell. */
static void test_no_glyph_overflows_its_cell(void) {
    bool clean = true;
    for (int c = FONT5X7_FIRST_CHAR; c <= FONT5X7_LAST_CHAR; c++) {
        const uint8_t *g = font5x7_glyph((char)c);
        for (int i = 0; i < FONT5X7_WIDTH; i++) {
            if (g[i] & 0x80) {
                clean = false;
            }
        }
    }
    UL_CHECK(clean, "no glyph sets a bit outside its seven rows");
}

/* ---- the layout arithmetic ------------------------------------------- */

/* Both of today's display failures were here, not in the drawing. The first
 * derived a scale from the panel HEIGHT and gave 21-pixel digits a person
 * could not read; the second reserved room for a unit on the same line, ate 90
 * of 228 pixels, and held a seven-digit amount down to that same size. Neither
 * could be checked without a board until this moved into portable code. */

/* The last glyph occupies its cell but not the blank column after it. Getting
 * that wrong by one column per line silently drops the last character off a
 * screen edge. */
static void test_text_width_excludes_the_trailing_gap(void) {
    UL_CHECK(font5x7_text_width("A", 1) == FONT5X7_WIDTH, "one glyph is its own width");
    UL_CHECK(font5x7_text_width("AB", 1) == FONT5X7_ADVANCE + FONT5X7_WIDTH,
             "two glyphs are one advance plus one width, not two advances");
    UL_CHECK(font5x7_text_width("A", 3) == FONT5X7_WIDTH * 3, "scale multiplies");
    UL_CHECK(font5x7_text_width("", 3) == 0 && font5x7_text_width(NULL, 3) == 0,
             "nothing is zero wide");
}

/* The property the whole thing exists for: the returned scale fits, and the
 * next one up does not. Checked across every width a real panel might have. */
static void test_fit_scale_returns_the_largest_that_fits(void) {
    const char *samples[] = {"21", "2 100", "100 000", "sats  big one", "0"};
    bool always_fits = true, always_largest = true;

    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        for (int avail = 40; avail <= 320; avail += 4) {
            const int s = font5x7_fit_scale(samples[i], avail, FONT5X7_MAX_SCALE);

            if (s > FONT5X7_MIN_READABLE_SCALE && font5x7_text_width(samples[i], s) > avail) {
                always_fits = false;
            }
            if (s < FONT5X7_MAX_SCALE &&
                font5x7_text_width(samples[i], s + 1) <= avail) {
                always_largest = false;
            }
        }
    }
    UL_CHECK(always_fits, "the chosen scale always fits the space it was given");
    UL_CHECK(always_largest, "and no larger scale would also have fitted");
}

/* Never below the readable floor, even when that overflows -- shortening beats
 * shrinking, because a line nobody can read conveys nothing whether or not it
 * is complete. */
static void test_fit_scale_never_goes_below_readable(void) {
    UL_CHECK(font5x7_fit_scale("a very long label indeed", 40, FONT5X7_MAX_SCALE) ==
                 FONT5X7_MIN_READABLE_SCALE,
             "text that cannot fit is still drawn at the readable minimum");
    UL_CHECK(font5x7_fit_scale("x", 1, FONT5X7_MAX_SCALE) == FONT5X7_MIN_READABLE_SCALE,
             "even in one pixel of space");
    UL_CHECK(font5x7_fit_scale("x", 10000, 1) == FONT5X7_MIN_READABLE_SCALE,
             "a max_scale below the floor is raised to it, not honoured");
}

/* The regression proper: on the classic panel a short amount must come out
 * BIG. The version a person could not read gave everything scale 3. */
static void test_the_amount_gets_a_big_scale_on_a_real_panel(void) {
    const int avail = 240 - 12; /* 240px panel, 6px margins */

    UL_CHECK(font5x7_fit_scale("21", avail, FONT5X7_MAX_SCALE) == FONT5X7_MAX_SCALE,
             "a two-digit amount gets the largest scale there is");
    UL_CHECK(font5x7_fit_scale("2 100", avail, FONT5X7_MAX_SCALE) >= 5,
             "a four-digit amount is still far above the old 3");
    UL_CHECK(font5x7_fit_scale("100 000", avail, FONT5X7_MAX_SCALE) >= 5,
             "and so is a six-digit one -- this is the case that regressed");

    /* Sanity: at that scale it really does fit the panel. */
    const int s = font5x7_fit_scale("100 000", avail, FONT5X7_MAX_SCALE);
    UL_CHECK(font5x7_text_width("100 000", s) <= avail, "and genuinely fits");
}

/* The confirm card on the real panel: 240x135, 6px margins, and a usable
 * height of 102 once display_progress()'s band is kept clear. The action line
 * sits above the amount, so the amount starts at 6 + 21 + gap.
 *
 * This is the geometry the hold hint was dropped on, and then the geometry the
 * fix for that overcorrected on: reserving for the unit/label and id lines as
 * well left the digits 24 pixels, which is scale 3 for every amount there is.
 * Both failures are one arithmetic slip in the same six lines, and neither is
 * visible without a board unless it is pinned here. */
#define CARD_AVAIL (240 - 12)
#define CARD_USABLE 102
#define CARD_SMALL_H (FONT5X7_HEIGHT * FONT5X7_MIN_READABLE_SCALE)
#define CARD_AMOUNT_Y (6 + CARD_SMALL_H + FONT5X7_CARD_GAP)

static void test_confirm_card_amount_stays_readable(void) {
    /* Every one of these rendered at the 21px minimum when the unit/label and
     * id lines were reserved for too. The short ones have room to spare. */
    UL_CHECK(font5x7_card_amount_scale("21", CARD_AVAIL, CARD_USABLE, CARD_AMOUNT_Y, 1)
                 > FONT5X7_MIN_READABLE_SCALE,
             "a two-digit amount on a confirm card is bigger than the minimum");
    UL_CHECK(font5x7_card_amount_scale("100 000", CARD_AVAIL, CARD_USABLE, CARD_AMOUNT_Y, 1)
                 > FONT5X7_MIN_READABLE_SCALE,
             "and so is a six-digit one -- this is the case that regressed");
    UL_CHECK(font5x7_card_amount_scale("1 000 000", CARD_AVAIL, CARD_USABLE, CARD_AMOUNT_Y, 1)
                 > FONT5X7_MIN_READABLE_SCALE,
             "and a seven-digit one, the size the whole card was rebuilt over");
}

static void test_confirm_card_still_leaves_the_hint_room(void) {
    /* The other side of the same trade: whatever the digits take, a readable
     * hint line must still fit under them. This is the failure the reservation
     * exists for, and it must not come back while fixing the overcorrection. */
    const char *amounts[] = {"21", "2 100", "100 000", "1 000 000", "21 000 000"};
    for (unsigned i = 0; i < sizeof(amounts) / sizeof(*amounts); i++) {
        const int scale =
            font5x7_card_amount_scale(amounts[i], CARD_AVAIL, CARD_USABLE, CARD_AMOUNT_Y, 1);
        UL_CHECK(scale > 0, "the amount is drawn at all");
        const int after = CARD_AMOUNT_Y + FONT5X7_HEIGHT * scale + FONT5X7_CARD_GAP;
        UL_CHECK(after + CARD_SMALL_H <= CARD_USABLE,
                 "a readable hint still fits under the amount");
        UL_CHECK(font5x7_text_width(amounts[i], scale) <= CARD_AVAIL,
                 "and the amount itself fits the width");
    }
}

static void test_card_amount_scale_refuses_to_draw_the_unreadable(void) {
    /* When the budget cannot afford even a minimum line, the answer is 0 -- do
     * not draw. font5x7_fit_scale clamps a too-small max back UP to the
     * readable minimum, so asking it directly here would return a full-height
     * amount and push the hint off the bottom: the original bug exactly. */
    UL_CHECK(font5x7_card_amount_scale("21", CARD_AVAIL, 40, CARD_AMOUNT_Y, 1) == 0,
             "no amount at all beats an unreadable one");
    UL_CHECK(font5x7_card_amount_scale(NULL, CARD_AVAIL, CARD_USABLE, CARD_AMOUNT_Y, 1) == 0,
             "and a missing amount draws nothing");
    UL_CHECK(font5x7_card_amount_scale("", CARD_AVAIL, CARD_USABLE, CARD_AMOUNT_Y, 1) == 0,
             "an empty one too");
}

static void test_a_card_with_no_hint_gives_the_digits_the_room(void) {
    /* The browse card passes no hint, so nothing is reserved and the digits get
     * everything the width allows. It must not be taxed for a line it does not
     * draw. */
    UL_CHECK(font5x7_card_amount_scale("100 000", CARD_AVAIL, CARD_USABLE, 6, 0) >=
                 font5x7_card_amount_scale("100 000", CARD_AVAIL, CARD_USABLE, CARD_AMOUNT_Y, 1),
             "a card with no hint is never worse off than one with");
}

void test_note_display_run(void) {
    printf("-- note_display --\n");
    test_whole_sats();
    test_sub_sat_amounts_are_not_rounded();
    test_magnitudes_are_visibly_different();
    test_largest_amount_fits();
    test_short_buffers_are_safe();
    test_amount_splits_into_digits_and_unit();
    test_ordinary_labels();
    test_missing_label_is_visible();
    test_unprintable_bytes_become_visible();
    test_labels_do_not_collide();
    test_long_label_is_truncated_safely();
    test_every_byte_has_a_glyph();
    test_glyph_shapes();
    test_no_glyph_overflows_its_cell();
    test_text_width_excludes_the_trailing_gap();
    test_fit_scale_returns_the_largest_that_fits();
    test_fit_scale_never_goes_below_readable();
    test_the_amount_gets_a_big_scale_on_a_real_panel();
    test_confirm_card_amount_stays_readable();
    test_confirm_card_still_leaves_the_hint_room();
    test_card_amount_scale_refuses_to_draw_the_unreadable();
    test_a_card_with_no_hint_gives_the_digits_the_room();
}
