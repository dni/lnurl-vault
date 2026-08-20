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

/* The largest scale anything is drawn at. A ceiling rather than a preference:
 * it bounds the per-glyph arithmetic and, on the drawing side, the buffers
 * that arithmetic sizes. Lives here rather than in display.h so the fitting
 * logic below stays free of ESP-IDF and can be tested without a board. */
#define FONT5X7_MAX_SCALE 6

/* Five column bytes for `c`: bit 0 is the top row, bit 6 the bottom.
 *
 * Never returns NULL and never reads out of range: anything outside the
 * printable range renders as '?'. Note labels arrive over the wire, so this
 * is called on attacker-influenced bytes. */
const uint8_t *font5x7_glyph(char c);

/* Pixel width of `text` at `scale`. The last glyph occupies its cell but not
 * the blank column after it, so this is not simply len * FONT5X7_ADVANCE --
 * getting that wrong by one column per line is the kind of arithmetic that
 * silently drops the last character off a screen edge. */
int font5x7_text_width(const char *text, int scale);

/* The largest scale in [FONT5X7_MIN_READABLE_SCALE, max_scale] at which `text`
 * fits `avail_w` pixels -- and the minimum when nothing fits, because
 * shortening beats shrinking: a line nobody can read conveys nothing whether or
 * not it is complete.
 *
 * Portable, and tested, because this is where today's two display failures
 * actually lived. The first version derived a scale from the panel HEIGHT and
 * produced 21-pixel digits a person could not read; the second reserved room
 * for a unit on the same line and ate 90 of 228 pixels, holding a seven-digit
 * amount down to the same unreadable size. Neither was a drawing bug. Both were
 * this arithmetic, and neither could be checked without a board until it moved
 * here. */
int font5x7_fit_scale(const char *text, int avail_w, int max_scale);

/* The vertical gap between card lines. ONE constant, because it is used both
 * when reserving space for a line and when advancing past one, and those two
 * disagreeing by a single pixel is not a rounding detail: four lines at the
 * readable minimum then land at y=103 against a usable height of 102, and the
 * last line is computed, reserved for, and silently dropped. */
#define FONT5X7_CARD_GAP 3

/* The scale for a note card's amount line: the largest that fits `avail_w`
 * once the lines BELOW it have been reserved for. 0 when not even a readable
 * line fits, meaning the caller must draw no amount at all rather than an
 * unreadable one.
 *
 * `y` is where the amount would start (already past the action line, if any).
 * Only the hint is reserved for. The unit/label and id lines keep their own
 * draw-if-it-fits guards at the call site: reserving for them as well taxes
 * the amount by two more rows, which on the 240x135 panel pins every confirm
 * card's digits to the readable minimum - the exact 21-pixel height this
 * header and display.h both record as unreadable on real hardware. A label is
 * worth less than legible digits.
 *
 * Here, and tested, for the same reason fit_scale is: this arithmetic is where
 * the display failures actually live, and none of it can be checked on a
 * board. */
int font5x7_card_amount_scale(const char *amount_num, int avail_w, int usable_h,
                              int y, int has_hint);

#endif
