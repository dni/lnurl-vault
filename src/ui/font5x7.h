#ifndef LNURLVAULT_FONT5X7_H
#define LNURLVAULT_FONT5X7_H

#include <stdint.h>

/* A 5x7 bitmap font over printable ASCII, so the device can say which note it
 * is about to disclose and for how much.
 *
 * Until this existed the approval screen was a full-screen colour and nothing
 * else: the owner was asked to physically approve handing out a bearer secret
 * without being told which note or for what amount, which makes the physical
 * gate a formality rather than a control. Issue #9.
 *
 * No ESP-IDF dependency, so the glyph table is exercised by
 * test/native/test_font5x7.c and can be printed as ASCII art by
 * tools/show_font.py without a board.
 */

#define FONT5X7_WIDTH 5
#define FONT5X7_HEIGHT 7
#define FONT5X7_FIRST_CHAR 32  /* space */
#define FONT5X7_LAST_CHAR 126  /* ~ */

/* One column of blank between glyphs, so a caller advances by this per
 * character. */
#define FONT5X7_ADVANCE (FONT5X7_WIDTH + 1)

/* The smallest scale worth drawing at on the panels this project uses.
 *
 * Borrowed judgement, not a measurement of mine: heartwood-esp32 rejected two
 * font sizes as unreadable on real hardware before settling on a minimum, and
 * recorded it (docs/memory/feedback_oled_font.md). A 5x7 cell at scale 1 is
 * seven pixels tall on a 240x135 panel held at arm's length, which is the
 * mistake they already made. Scale 2 is the floor here, and the amount --
 * the one field a mistake actually costs money -- is drawn larger still. */
#define FONT5X7_MIN_READABLE_SCALE 2

/* Five column bytes for `c`: bit 0 is the top row, bit 6 the bottom.
 *
 * Never returns NULL and never reads out of range: anything outside the
 * printable range renders as '?'. Note labels arrive over the wire, so this
 * is called on attacker-influenced bytes. */
const uint8_t *font5x7_glyph(char c);

#endif
