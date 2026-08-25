#ifndef LNURLVAULT_DISPLAY_H
#define LNURLVAULT_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "font5x7.h"

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

/* Puts the screen out, and brings it back.
 *
 * The vault sits on a desk all day with the same resting card in the same
 * pixels; an IPS panel left like that acquires a faint permanent copy of it.
 * Sleeping blanks the framebuffer to black AND kills the backlight -- both,
 * not either: the backlight alone leaves the liquid crystal held in the same
 * state it was ghosting into, and the blank alone leaves a lit black
 * rectangle that looks like a dead device.
 *
 * WHEN to sleep is not decided here. That is src/proto/screen_sleep.h, driven
 * by src/ui/ui_task.c, which is also the only thing that should call these.
 *
 * Order matters when waking, and it is the caller's to get right: REPAINT
 * FIRST, THEN display_wake(). The panel keeps whatever was last drawn on it,
 * so lighting it before the new card is composed shows the old one -- the
 * same reason both board files turn their backlight on last during
 * bring-up. Drawing while asleep is not an error; it simply happens in the
 * dark, which is exactly what is wanted.
 *
 * Both are idempotent, and both do nothing at all when the panel never came
 * up: there is no screen to blank, and a display_ready() of false already
 * means every disclosure refuses. */
void display_sleep(void);
void display_wake(void);
bool display_asleep(void);

/* The two pieces of card furniture, in pixels: the header band along the top
 * and the progress bar along the bottom. Exposed only so the boot screen can
 * animate INTO exactly the geometry every card afterwards uses -- an opening
 * shutter that settles a pixel off the band it becomes is worse than no
 * animation. Nothing else should need these; a card gets them by being drawn
 * through display_note_detail(). */
int display_band_height(void);
int display_bar_height(void);

void display_set_state(display_state_t state);

/* Exposed so anything drawing onto a state's background, or checking what was
 * drawn there, gets the same answer rather than a second copy of the mapping.
 * None of these is a constant -- see palette.h.
 *
 * `color` is the ground: a warm near-black on the screens you read up close,
 * the state's own colour on the ones you read across a room. `accent` is the
 * state's colour either way, which is what the header band and the progress
 * bar are drawn in. `ink` is the text, and `ink_dim` is the second weight for
 * everything that is context rather than content -- unit, label, id, gesture.
 * Without that second weight an amount and the eight hex characters beside it
 * carried exactly equal force. */
uint16_t display_state_color(display_state_t state);
uint16_t display_state_accent(display_state_t state);
uint16_t display_state_ink(display_state_t state);
uint16_t display_state_ink_dim(display_state_t state);

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
#define DISPLAY_MAX_TEXT_SCALE FONT5X7_MAX_SCALE
void display_text(int x, int y, const char *text, int scale, uint16_t fg, uint16_t bg);

/* Draws a note's detail full-screen on `state`'s background: which note, and
 * for how much. Until this existed the screen was a flat colour, which makes
 * the physical gate a formality rather than a control (issue #9).
 *
 * The amount arrives split (see note_display.h) and its digits get the whole
 * width at the largest scale that fits, with the unit on the line below: the
 * first version drew the lot at one scale and a person on real hardware could
 * not read it.
 *
 * `action` is the verb, drawn first -- without it, disclosing one note and
 * wiping every note presented identically. `hint` is the gesture, drawn last
 * against the bar; the approval is a two-second HOLD (approval.h) and nothing
 * on screen used to say so, so people tapped and concluded it was dead.
 *
 * Layout comes from the panel at runtime and leaves the lower band for
 * display_progress(). Any argument may be NULL and that line is skipped; the
 * amount is fitted to what the lines below leave, so adding one shrinks the
 * digits rather than pushing a line off the bottom. */
void display_note_detail(display_state_t state, const char *action, const char *amount_num,
                          const char *amount_unit, const char *label, const char *id,
                          const char *hint);

/* A large title and up to two smaller lines, centred as a block on `state`'s
 * colour: an outcome, the boot screen, or what the vault holds at rest. All
 * of those were a flat colour, which means nothing to anyone who does not
 * already know the scheme -- and a blank idle screen got pressed at, on a
 * device where a press starts browsing bearer secrets.
 *
 * The title takes the largest scale that fits the width and the lines below.
 * Any argument may be NULL and that line is skipped. */
void display_message(display_state_t state, const char *title, const char *line1,
                     const char *line2);

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
