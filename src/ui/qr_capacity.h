#ifndef LNURLVAULT_QR_CAPACITY_H
#define LNURLVAULT_QR_CAPACITY_H

#include <stddef.h>
#include <stdint.h>

/* QR version selection, deliberately kept free of any ESP-IDF or display
 * dependency so test/native can reach it.
 *
 * It lives in its own file because the bug it exists to prevent shipped
 * precisely because it was unreachable from the test suite: the vendored
 * encoder does not report a payload that will not fit (qrcode_initBytes()
 * returns -1 only for an invalid mode), so version selection is ours to get
 * right, and a wrong answer renders a corrupt code that looks perfectly
 * valid. That is worth a test, and a test needs it out here. */

/* Highest version this project is prepared to render. */
#define QR_MAX_VERSION 20

/* Smallest QR version at ECC level L whose byte-mode capacity holds `len`
 * bytes, or 0 if no supported version does.
 *
 * Byte mode on purpose, even though the encoder will use denser alphanumeric
 * mode when a payload allows it: overestimating picks a slightly larger
 * version than strictly needed, while underestimating renders garbage.
 * lnurlw:// URLs are lowercase, so byte mode regardless. */
uint8_t qr_version_for_length(size_t len);

/* Byte-mode capacity at ECC-L for `version`, or 0 if out of range. Exposed so
 * a test can assert the table against the standard rather than against
 * itself. */
uint16_t qr_capacity_for_version(uint8_t version);


/* --- geometry ----------------------------------------------------------
 *
 * Where the code lands on a panel, kept out here for the same reason version
 * selection is: it is arithmetic, it has been wrong before, and inside
 * qr_display.c no test could reach it. Two failures came from this maths --
 * a scale derived so the square came out wider than a landscape panel, which
 * sent the centring negative and drew the code partly off the glass; and a
 * quiet zone counted into the square's size but not into its module offsets,
 * which pushed the whole margin onto the right and bottom and left as little
 * as 3px above a code where the spec wants four modules. */

/* ISO/IEC 18004 requires four clear modules on every side. Not decoration:
 * a decoder uses the quiet zone to find the code's edge, and a short one is a
 * code that scans on some phones and not others -- the worst kind of broken
 * for something handed to a stranger. */
#define QR_QUIET_ZONE_MODULES 4

/* Modules across the whole drawn square, code plus quiet zone on both sides. */
int qr_square_modules(int qr_size);

/* Largest whole-pixel scale at which the square fits both panel dimensions,
 * or 0 when it will not fit even at one pixel per module. Whole pixels only:
 * a fractional scale means module edges land mid-pixel, and a decoder reading
 * a smeared boundary is exactly the failure this cannot afford. */
int qr_scale_for(int qr_size, int screen_w, int screen_h);

/* Top-left corner of the centred square. Never negative for a scale that
 * qr_scale_for() returned -- that is the invariant the off-panel bug broke. */
void qr_origin(int qr_size, int scale, int screen_w, int screen_h, int *x0, int *y0);

/* Module coordinate for a pixel offset within the drawn square, or a negative
 * value when that pixel falls in the quiet zone rather than on the code. */
int qr_module_at(int pixel_in_square, int scale);

#endif
