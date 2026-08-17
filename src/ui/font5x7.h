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
 * MEASURED, not guessed, and it has already moved once. This started at 2 --
 * a 14-pixel-tall cell on a 240x135 panel -- reasoning from heartwood-esp32
 * having rejected two sizes as unreadable before settling on a minimum
 * (docs/memory/feedback_oled_font.md). Put in front of a person on real
 * hardware, the verdict was immediate: "the text on the screen is too small
 * for me to read".
 *
 * So it is 3, and nothing is drawn below it. Text that will not fit at 3 is
 * shortened rather than shrunk, because a line nobody can read conveys
 * nothing whether or not it is complete. The amount gets whatever larger
 * scale the panel width allows -- see display_note_detail(). */
#define FONT5X7_MIN_READABLE_SCALE 3

/* Five column bytes for `c`: bit 0 is the top row, bit 6 the bottom.
 *
 * Never returns NULL and never reads out of range: anything outside the
 * printable range renders as '?'. Note labels arrive over the wire, so this
 * is called on attacker-influenced bytes. */
const uint8_t *font5x7_glyph(char c);

#endif
