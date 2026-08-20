#ifndef LNURLVAULT_HOSTGFX_H
#define LNURLVAULT_HOSTGFX_H

#include <stdint.h>

/* A panel you can run the firmware's real drawing code against, on a laptop.
 *
 * Every display bug this project has actually shipped was a layout bug, and
 * every one of them was found by a person squinting at a board -- the amount
 * drawn too small to read, the whole screen mirrored, the hold hint computed
 * and then silently dropped one pixel past the bottom. None of those are
 * reachable from a unit test that only checks arithmetic, because the
 * arithmetic was individually defensible each time; what was wrong was what
 * ended up on the glass.
 *
 * So this stands in for ESP-IDF underneath src/ui/display.c rather than
 * standing in for display.c: the shim headers beside this one satisfy its
 * four ESP dependencies (esp_lcd, heap_caps, FreeRTOS, board), and
 * esp_lcd_panel_draw_bitmap writes into a framebuffer instead of a DMA queue.
 * The pixels a test inspects are produced by the same function that produces
 * the pixels on the device, at the same geometry, from the same font.
 *
 * Two uses: assertions (test_card_render.c -- "the line I asked for is
 * actually on screen"), and PNGs (preview.c -- looking at a screen without
 * flashing a board, which is what the confirm card should have had all along).
 */

/* What hostgfx_reset() fills the framebuffer with: a colour no screen in this
 * firmware ever draws, so "the firmware never painted here" is distinguishable
 * from "the firmware painted black here". Black is the ink colour on every
 * note card, so a framebuffer cleared to black would make an unpainted panel
 * and a fully-inked one look the same to a test, and would hide a gap in a
 * preview instead of shouting about it. */
#define HOSTGFX_UNPAINTED 0xF81F /* magenta */

#define HOSTGFX_MAX_W 400
#define HOSTGFX_MAX_H 400

/* Sets what board_display_init() will report, and blanks the framebuffer.
 * Call before display_init(). Geometry larger than the maxima above is
 * clamped -- both real panels are well inside them. */
void hostgfx_reset(int w, int h);

int hostgfx_width(void);
int hostgfx_height(void);

/* RGB565 at (x, y); 0 for anything off-panel. */
uint16_t hostgfx_pixel(int x, int y);

/* How many pixels have been drawn outside the panel since hostgfx_reset().
 * A real panel swallows those silently, which is exactly how a line comes to
 * be drawn and yet invisible; on the host it is countable, so a test can
 * simply insist it stays zero. */
long hostgfx_offscreen_pixels(void);

/* Total pixels currently equal to `ink`. Note cards draw text in one colour
 * on a flat background, so this is a proxy for "how much text is on screen" --
 * and the question a dropped line answers wrongly. */
long hostgfx_ink_pixels(uint16_t ink);

/* Topmost / bottommost row containing an `ink` pixel, or -1 if there is none.
 * The bottom one is what a line falling off the card changes. */
int hostgfx_first_ink_row(uint16_t ink);
int hostgfx_last_ink_row(uint16_t ink);

/* Rightmost column containing an `ink` pixel, or -1. Text on this device is
 * clipped at the panel edge rather than wrapped, so "how close did that line
 * get to running out of screen" is a question worth being able to ask. */
int hostgfx_last_ink_col(uint16_t ink);

/* Remembers the framebuffer, so a later hostgfx_first_changed_row() reports
 * the topmost row some subsequent drawing touched. That is how a test locates
 * a band -- the progress bar, say -- without knowing any of display.c's
 * internal geometry: draw, snapshot, draw the other thing, ask what moved. */
void hostgfx_snapshot(void);
int hostgfx_first_changed_row(void);

/* Writes the framebuffer to `path` as a PNG, each pixel drawn as a zoom x
 * zoom block (the panels are small; at 1:1 they are hard to look at). Returns
 * 0 on success, non-zero on any I/O or allocation failure.
 *
 * Deliberately dependency-free -- a stored-deflate zlib stream, which is a
 * legal one -- because a preview tool that needs libpng installed is a
 * preview tool nobody runs. */
int hostgfx_write_png(const char *path, int zoom);

#endif
