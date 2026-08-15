#ifndef LNURLVAULT_DISPLAY_H
#define LNURLVAULT_DISPLAY_H

#include "esp_lcd_panel_ops.h" /* esp_lcd_panel_handle_t */

#define LCD_WIDTH 320
#define LCD_HEIGHT 170

typedef enum {
    DISPLAY_STATE_IDLE,
    DISPLAY_STATE_BROWSE,
    DISPLAY_STATE_CONFIRM_PENDING,
    DISPLAY_STATE_APPROVED,
    DISPLAY_STATE_DECLINED,
} display_state_t;

void display_init(void);
void display_set_state(display_state_t state);

/* Flashes the screen white `count` times (150ms on/150ms off each), then
 * restores whatever state display_set_state last showed. A lightweight,
 * font-free way to indicate "note #N (of however many CONFIRMED notes
 * exist) is currently selected" while browsing — see README.md's "Known
 * limitations" on why there's no on-screen text yet. Clamped to 20 flashes
 * regardless of count, so a large note collection doesn't turn selection
 * into a multi-second light show. count < 1 is a no-op. */
void display_flash_count(int count);

/* The already-initialized panel handle, for qr_display.c to draw directly
 * onto rather than opening a second, conflicting panel instance. Valid only
 * after display_init(). */
esp_lcd_panel_handle_t display_panel_handle(void);

#endif
