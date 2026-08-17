#ifndef LNURLVAULT_DISPLAY_H
#define LNURLVAULT_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_lcd_panel_ops.h" /* esp_lcd_panel_handle_t */

typedef enum {
    DISPLAY_STATE_IDLE,
    DISPLAY_STATE_BROWSE,
    DISPLAY_STATE_CONFIRM_PENDING,
    DISPLAY_STATE_APPROVED,
    DISPLAY_STATE_DECLINED,
    /* A prompt nobody answered. Its own state, not DECLINED, so a stale
     * request can never be left looking live and can never be mistaken for
     * the owner having said no. */
    DISPLAY_STATE_EXPIRED,
} display_state_t;

/* Brings up whatever panel src/board/ describes and clears it to the idle
 * colour. Never fails loudly: check display_ready() afterwards. */
void display_init(void);

/* False when the panel could not be brought up. Anything that discloses a
 * secret MUST check this and refuse rather than proceed -- the physical
 * confirmation is the security control, and a confirmation nobody can see is
 * not one. */
bool display_ready(void);

/* Usable surface, in the orientation the board has already applied. Runtime
 * rather than compile-time constants, so drawing code stays board-agnostic
 * and a second board does not silently inherit the first one's geometry. */
int display_width(void);
int display_height(void);

void display_set_state(display_state_t state);

/* Fills an axis-aligned rectangle. Out-of-bounds rectangles are dropped
 * rather than clipped: a caller computing a negative origin has a bug, and
 * silently drawing something slightly wrong on a device that shows bearer
 * secrets is worse than drawing nothing. */
void display_fill_rect(int x, int y, int w, int h, uint16_t color);

/* Draws `text` with its top-left at (x, y), each font pixel scaled to a
 * scale x scale block. Clips at the panel edge rather than wrapping or
 * refusing: a label arrives over the wire, so an over-long one must simply
 * run out of screen. Scale is clamped to DISPLAY_MAX_TEXT_SCALE.
 *
 * See src/ui/font5x7.h -- printable ASCII only, and anything else draws as
 * '?' rather than as nothing, so two different labels cannot look the same. */
#define DISPLAY_MAX_TEXT_SCALE 6
void display_text(int x, int y, const char *text, int scale, uint16_t fg, uint16_t bg);

/* Draws a note's detail full-screen, on `state`'s background: says which
 * note, and for how much.
 *
 * Until this existed the screen was a flat colour and nothing else, so the
 * owner was asked to physically approve handing over a bearer secret without
 * being told either -- which makes the physical gate a formality rather than
 * a control (issue #9). The amount is drawn largest because it is the field
 * where a mistake costs money.
 *
 * The amount arrives split (see note_display.h): the digits are drawn at the
 * largest scale the panel width allows, with the unit small beside them on the
 * same line. That is not decoration -- " sats" is five characters of a line
 * that has to fit in 240 pixels, and giving them back to the digits is the
 * difference between an amount being readable and merely being present. The
 * first version of this drew the whole string at one scale and a person on
 * real hardware could not read it.
 *
 * Layout is derived from the panel at runtime and leaves the lower band free
 * for display_progress(). Any argument may be NULL, and that line is skipped.
 */
void display_note_detail(display_state_t state, const char *amount_num,
                          const char *amount_unit, const char *label, const char *id);

/* Draws the approval hold as a filling bar, 0..1000 parts per thousand (see
 * approval.h). Idempotent and cheap enough to call every poll tick: it
 * repaints only the bar, not the screen behind it, so the amber
 * CONFIRM_PENDING background set by display_set_state stays put.
 *
 * The bar is the feedback the old single-tap gesture had none of. A hold with
 * nothing on screen is indistinguishable from a device that is not listening,
 * and the owner's response to that is to press harder and more often -- at
 * the exact screen where that is least wanted. */
void display_progress(uint16_t permille);

/* Flashes the screen white `count` times (150ms on/150ms off each), then
 * restores whatever state display_set_state last showed.
 *
 * This was the font-free way to indicate "note #N of the CONFIRMED ones is
 * selected" while browsing, from before there was any on-screen text.
 * display_note_detail() replaced it there -- a blinked-out count told you
 * which position you were on but not which note or for how much, and
 * unveiling the wrong note is exactly the risk. Kept because a
 * non-text-dependent signal is still worth having on a panel whose text
 * rendering is broken. Clamped to 20 flashes. count < 1 is a no-op. */
void display_flash_count(int count);

/* The initialised panel handle, for qr_display.c to draw onto rather than
 * opening a second, conflicting panel instance. NULL until display_init()
 * has run successfully. */
esp_lcd_panel_handle_t display_panel_handle(void);

#endif
