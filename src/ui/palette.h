#ifndef LNURLVAULT_PALETTE_H
#define LNURLVAULT_PALETTE_H

#include <stdint.h>

/* RGB565, as the panels store it.
 *
 * The first scheme made the state colour the WALLPAPER: a full field of
 * #00FC00, #F80000 or #F8D400 edge to edge, with black or white text on top.
 * It was chosen to be unmistakable at arm's length and it is -- but a
 * saturated field has no edges, so nothing on it can be framed, separated or
 * emphasised. Everything read as one wash, and the colour was spent on the
 * background before any of it could be spent on meaning.
 *
 * So the colour was promoted out of the background and into the STRUCTURE.
 * Two kinds of screen now:
 *
 *   CARDS  -- browse, confirm, rest. Things you read, up close, holding the
 *             device. A warm near-black ground, warm off-white text, and the
 *             state colour as a header band and the progress bar. The band
 *             carries the verb, which is the thing that must never be
 *             mistaken, and it carries it in colour AND in words.
 *
 *   FIELDS -- approved, declined, no answer. Things you read at a glance,
 *             from across a room, that carry no detail to study. These keep
 *             the full field of colour, because that is what it is good at,
 *             with the same warm dark as ink.
 *
 * The colours themselves came off the primaries. #00FC00 against black text
 * is not bold, it is fluorescent, and pure primaries at full saturation are
 * exactly what looks cheap on a small panel. These are the same six signals
 * pulled towards something a person would choose: warm amber rather than
 * sodium yellow, coral rather than fire engine, mint rather than laser. They
 * stay far apart in hue, which is the property that made the old set worth
 * keeping. */

/* --- the ground everything sits on --------------------------------------- */

/* #14120F. Warm, not blue-black: a neutral #000 next to amber and coral
 * reads cold and makes them look garish. Doubles as the ink on colour
 * fields, so a card's ground and an outcome's text are the same value --
 * which is what makes the two kinds of screen feel like one device. */
#define PALETTE_GROUND 0x1081

/* #EAE6DC. Off-white with the same warmth as the ground. Pure #FFF on a dark
 * panel glares and bleeds at these glyph sizes. */
#define PALETTE_INK 0xEF3B

/* #8F8A7E. Everything that is context rather than content -- the unit, the
 * label, the id, the gesture. Without a second weight the amount and the
 * eight hex characters beside it carried equal force, and they do not. */
#define PALETTE_INK_DIM 0x8C4F

/* True black, and true white. Not part of the scheme above: the first is what
 * a sleeping screen is cleared to (see display_sleep -- the point there is an
 * unlit panel, not a warm one), the second is the QR's own ground, where a
 * phone's decoder wants maximum contrast and no opinion about warmth. */
#define PALETTE_INK_DARK 0x0000
#define PALETTE_INK_LIGHT 0xFFFF
#define PALETTE_PAPER 0xFFFF

/* Unfilled progress track: the ground lifted just enough to be visible as a
 * channel the bar runs in, rather than a bar appearing out of nothing. */
#define PALETTE_TRACK 0x2945 /* #2C2820 */

/* --- the six signals ------------------------------------------------------ */

#define PALETTE_ACCENT_IDLE 0x4554     /* #46A9A0 teal -- at rest, holding */
#define PALETTE_ACCENT_BROWSE 0x9B5E   /* #9B6BF2 violet -- looking through notes */
#define PALETTE_ACCENT_PENDING 0xF524  /* #F5A623 amber -- being asked */
#define PALETTE_ACCENT_APPROVED 0x3EB1 /* #3DD68C mint -- you said yes */
#define PALETTE_ACCENT_DECLINED 0xF289 /* #F0524B coral -- you said no */
/* Neither the amber of a live prompt nor the coral of a refusal: a prompt
 * nobody answered is neither, and must not be dressed as either. */
#define PALETTE_ACCENT_EXPIRED 0x9CB1 /* #9A958C */

#endif
