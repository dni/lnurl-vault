#ifndef LNURLVAULT_BOARD_H
#define LNURLVAULT_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_lcd_panel_ops.h" /* esp_lcd_panel_swap_xy / _mirror */
#include "esp_lcd_types.h"    /* esp_lcd_panel_handle_t */

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

/* Applies a panel's orientation, and refuses at compile time to configure one
 * that is REFLECTED rather than merely rotated.
 *
 * Both esp_lcd_panel_swap_xy() and each esp_lcd_panel_mirror() flag are
 * reflections -- swap_xy is a transpose, which is a reflection about the
 * diagonal. Composing them, the image comes out un-mirrored only when an EVEN
 * number of reflections is applied in total. Odd, and everything drawn is
 * handed the wrong way round.
 *
 * That is not a theoretical concern. The classic T-Display shipped with
 * swap_xy plus BOTH mirror flags -- three reflections, odd -- and every pixel
 * it drew was mirrored from the day the board was added. It survived because
 * the orientation was established by walking a red square to a corner, and a
 * corner marker cannot tell a rotation from a reflection: two different
 * settings put the marker in the same corner and only one of them is right.
 * Nothing drawn afterwards could reveal it either. A flat colour has no
 * handedness, and neither does a QR code as far as a phone is concerned --
 * decoders correct orientation themselves, so the mirrored codes still
 * scanned. It took putting readable text on the screen, and a person saying
 * it looked wrong.
 *
 * The parity rule catches all of that without a display, a camera or a
 * person. It says nothing about WHICH rotation is right -- that still needs an
 * asymmetric figure on real glass, see docs/HARDWARE-TEST-CHECKLIST.md -- only
 * that the result is a rotation at all, which is the half that was silently
 * wrong for weeks. */
#define BOARD_APPLY_ORIENTATION(panel, swap_xy, mirror_x, mirror_y)                              \
    do {                                                                                          \
        _Static_assert((((swap_xy) ? 1 : 0) + ((mirror_x) ? 1 : 0) + ((mirror_y) ? 1 : 0)) % 2 == 0, \
                       "panel orientation is a reflection, not a rotation: swap_xy and each "     \
                       "mirror flag are reflections, so an odd number of them mirrors everything " \
                       "drawn. Flip one flag and re-check which rotation is right on real glass."); \
        esp_lcd_panel_swap_xy((panel), (swap_xy));                                                \
        esp_lcd_panel_mirror((panel), (mirror_x), (mirror_y));                                    \
    } while (0)

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

/* Starts whichever host transport this board is wired for, and begins
 * serving the command protocol on it: native USB-CDC where the chip has a
 * USB-OTG peripheral, a UART behind an external USB bridge where it does not.
 * Both present to a browser as an ordinary serial port, so docs/PROTOCOL.md's
 * newline-delimited JSON is identical either way. */
void board_serial_start(void);

/* What this board can ask its owner with.
 *
 * Every gated command here assumes two buttons. True of both current boards,
 * false of the next ones worth having: T-Dongle-S3 has one, T-Watch-S3 has
 * none and a touchscreen. Those need a different gesture, and something above
 * this layer has to know which. Reported via get_info's `capabilities`.
 *
 * `buttons` counts buttons wired for confirm/cancel, not buttons present --
 * that is what the gesture layer has to work with. */
typedef struct {
    uint8_t buttons; /* 0, 1 or 2 */
    bool touch;
} board_input_caps_t;

board_input_caps_t board_input_caps(void);

/* Configures the board's buttons as inputs. */
void board_buttons_init(void);

/* True while the button is physically pressed, whatever polarity and pull
 * this board actually wires. Button 1 confirms, button 2 cancels
 * (see docs/PROTOCOL.md). */
bool board_button_1_pressed(void);
bool board_button_2_pressed(void);

#endif
