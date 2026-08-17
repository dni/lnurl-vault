#ifndef LNURLVAULT_BOARD_H
#define LNURLVAULT_BOARD_H

#include <stdbool.h>

#include "esp_lcd_types.h" /* esp_lcd_panel_handle_t */

/* Board hardware-abstraction seam.
 *
 * Everything that differs between physical boards -- which bus the panel
 * hangs off, which pins carry it, which way round the glass is fitted, which
 * GPIOs the buttons use -- lives behind this header, implemented once per
 * board in exactly one board_*.c. Nothing above this layer (ui/, transport/,
 * main.c) names a pin or knows a bus type.
 *
 * This exists because it was missing: the display driver was written against
 * an SPI bus that the target board does not have, and because bus choice was
 * baked into the drawing code there was no single place that could be wrong,
 * and so no single place to fix.
 *
 * The shape is borrowed from a sibling ESP32 project that carries three
 * different panels (I2C mono OLED, SPI ST7789, SPI JD9853) behind one seam:
 * https://github.com/forgesworn/heartwood-esp32/blob/main/firmware/src/board.rs
 *
 * Adding a board is one new board_*.c plus one line in ../CMakeLists.txt.
 */

/* Reported by get_info, so a client -- and a bug report -- can say which pin
 * map and which panel are actually in play. */
extern const char *const BOARD_NAME;

typedef struct {
    esp_lcd_panel_handle_t panel; /* NULL if bring-up failed */
    int width;                    /* usable pixels, in the orientation the */
    int height;                   /* board has already applied */
} board_display_t;

/* Powers and initialises the panel, applying whatever rotation, mirroring,
 * colour inversion and controller-RAM offset this particular glass needs, so
 * that callers get a plain width x height surface with (0,0) at the top left
 * and no knowledge of any of it.
 *
 * Returns .panel == NULL on failure rather than aborting. A vault whose
 * screen is dead must still boot: the notes are still on it, and a paired
 * host can still reach them. Callers must cope with a NULL panel -- but note
 * that every path which discloses a secret is gated on the screen, so those
 * paths must refuse, not proceed blind. */
board_display_t board_display_init(void);

/* Configures the board's two buttons as inputs. */
void board_buttons_init(void);

/* True while the button is physically pressed, whatever polarity and pull
 * this board actually wires. Button 1 confirms, button 2 cancels
 * (see docs/PROTOCOL.md). */
bool board_button_1_pressed(void);
bool board_button_2_pressed(void);

#endif
