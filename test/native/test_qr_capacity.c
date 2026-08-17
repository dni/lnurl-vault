/* Guards the QR version selection that src/ui/qr_display.c depends on.
 *
 * This exists because the bug it prevents reached real hardware: the vendored
 * encoder returns success for a payload that overflows the version it was
 * given, so nothing downstream notices, and the rendered code looks valid
 * while decoding to nothing. Version selection is therefore load-bearing, and
 * until qr_capacity.c was split out it was unreachable from here. */
#include <stdbool.h>
#include <string.h>

#include "qr_capacity.h"
#include "unity_lite.h"

void test_qr_capacity_run(void) {
    /* Spot-checked against ISO/IEC 18004's byte-mode ECC-L capacities rather
     * than against the implementation, so a transcription slip in the table
     * fails here instead of silently shrinking what we can encode. */
    UL_CHECK(qr_capacity_for_version(1) == 17, "v1 holds 17 bytes at ECC-L");
    UL_CHECK(qr_capacity_for_version(4) == 78, "v4 holds 78 bytes at ECC-L");
    UL_CHECK(qr_capacity_for_version(5) == 106, "v5 holds 106 bytes at ECC-L");
    UL_CHECK(qr_capacity_for_version(10) == 271, "v10 holds 271 bytes at ECC-L");
    UL_CHECK(qr_capacity_for_version(0) == 0, "version 0 is not a version");
    UL_CHECK(qr_capacity_for_version(QR_MAX_VERSION + 1) == 0, "past the table is rejected");

    /* Capacity must rise monotonically, or "smallest that fits" is not
     * smallest. */
    bool monotonic = true;
    for (uint8_t v = 2; v <= QR_MAX_VERSION; v++) {
        if (qr_capacity_for_version(v) <= qr_capacity_for_version(v - 1)) {
            monotonic = false;
        }
    }
    UL_CHECK(monotonic, "capacity increases with every version");

    /* The chosen version must always actually fit the payload, and the one
     * below it must not -- that is the whole contract. */
    bool always_fits = true, always_smallest = true;
    for (size_t len = 1; len <= 858; len++) {
        uint8_t v = qr_version_for_length(len);
        if (v == 0 || qr_capacity_for_version(v) < len) {
            always_fits = false;
        }
        if (v > 1 && qr_capacity_for_version((uint8_t)(v - 1)) >= len) {
            always_smallest = false;
        }
    }
    UL_CHECK(always_fits, "every length from 1 to 858 gets a version that holds it");
    UL_CHECK(always_smallest, "and never a larger version than necessary");

    UL_CHECK(qr_version_for_length(0) == 1, "an empty payload still needs a real version");
    UL_CHECK(qr_version_for_length(858) == QR_MAX_VERSION, "the largest supported payload fits v20");
    UL_CHECK(qr_version_for_length(859) == 0, "one byte past the table is refused, not clamped");

    /* The payload that actually broke on hardware: a realistic note URL. The
     * old code pinned this to MIN_QR_VERSION 4, whose 78 bytes cannot hold it. */
    const char *real = "lnurlw://mint.example.com?k1="
                       "00000000000000000000000000000000000000000000000000000000000000ff"
                       "&amount=21000";
    const uint8_t v = qr_version_for_length(strlen(real));
    UL_CHECK(strlen(real) == 106, "the realistic note URL is 106 bytes");
    UL_CHECK(v == 5, "a realistic note URL needs version 5");
    UL_CHECK(qr_capacity_for_version(4) < strlen(real),
             "and version 4 -- the old hardcoded choice -- provably cannot hold it");
}
