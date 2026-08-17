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

/* Flashes the screen white `count` times (150ms on/150ms off each), then
 * restores whatever state display_set_state last showed. A font-free way to
 * indicate "note #N of the CONFIRMED ones is selected" while browsing -- see
 * README.md's "Known limitations" on why there is no on-screen text yet.
 * Clamped to 20 flashes. count < 1 is a no-op. */
void display_flash_count(int count);

/* The initialised panel handle, for qr_display.c to draw onto rather than
 * opening a second, conflicting panel instance. NULL until display_init()
 * has run successfully. */
esp_lcd_panel_handle_t display_panel_handle(void);

#endif
