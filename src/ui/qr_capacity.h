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

#endif
