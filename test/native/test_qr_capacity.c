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

/* ---- geometry ---------------------------------------------------------
 *
 * Two shipped bugs came from this arithmetic, and neither could be reached
 * from a test while it lived inside qr_display.c. */

/* ISO/IEC 18004 wants four clear modules on every side. A decoder uses the
 * quiet zone to find the code's edge, so a short one scans on some phones and
 * not others -- the worst kind of broken for something handed to a stranger. */
static void test_the_quiet_zone_is_on_all_four_sides(void) {
    const int size = 33; /* version 4 */
    UL_CHECK(qr_square_modules(size) == size + 8,
             "the square is the code plus four modules on each side");

    /* At the top-left corner, the first four modules of the square are quiet
     * zone, and the fifth is the code. */
    const int scale = 3;
    UL_CHECK(qr_module_at(0, scale) < 0, "the very first pixel is quiet zone");
    UL_CHECK(qr_module_at(QR_QUIET_ZONE_MODULES * scale - 1, scale) < 0,
             "and so is the last pixel before the code starts");
    UL_CHECK(qr_module_at(QR_QUIET_ZONE_MODULES * scale, scale) == 0,
             "module 0 begins exactly after the quiet zone");

    /* And symmetrically at the far edge: the last four modules are quiet zone
     * too, which is the half the original bug dropped. */
    const int px = qr_square_modules(size) * scale;
    UL_CHECK(qr_module_at(px - 1, scale) >= size,
             "the final pixel is past the code, i.e. quiet zone");
    UL_CHECK(qr_module_at(px - QR_QUIET_ZONE_MODULES * scale, scale) == size,
             "the trailing quiet zone is a full four modules");
}

/* The code must never be drawn partly off the panel. A scale taken from one
 * dimension alone produced a square wider than a landscape screen, and the
 * centring then went negative. */
static void test_the_code_always_fits_on_the_panel(void) {
    const int panels[][2] = {{240, 135}, {135, 240}, {320, 170}, {170, 320}, {128, 128}};
    bool always_fits = true, never_negative = true, always_largest = true;

    for (size_t p = 0; p < sizeof(panels) / sizeof(panels[0]); p++) {
        const int w = panels[p][0], h = panels[p][1];
        for (int size = 21; size <= 97; size += 4) { /* versions 1..20 */
            const int scale = qr_scale_for(size, w, h);
            if (scale <= 0) {
                continue; /* legitimately too big for this panel */
            }
            const int px = qr_square_modules(size) * scale;
            if (px > w || px > h) {
                always_fits = false;
            }
            int x0 = 0, y0 = 0;
            qr_origin(size, scale, w, h, &x0, &y0);
            if (x0 < 0 || y0 < 0 || x0 + px > w || y0 + px > h) {
                never_negative = false;
            }
            /* One scale larger must genuinely not fit, or we wasted screen. */
            const int bigger = qr_square_modules(size) * (scale + 1);
            if (bigger <= w && bigger <= h) {
                always_largest = false;
            }
        }
    }
    UL_CHECK(always_fits, "the square fits both panel dimensions, on every panel tried");
    UL_CHECK(never_negative, "and is fully on screen, never a negative origin");
    UL_CHECK(always_largest, "and no larger whole-pixel scale would also have fitted");
}

/* A panel too small for a given version must say so rather than return a
 * fractional or zero scale that later divides by zero. */
static void test_a_panel_too_small_is_refused(void) {
    UL_CHECK(qr_scale_for(97, 64, 64) == 0, "a big version on a tiny panel is refused");
    UL_CHECK(qr_scale_for(0, 240, 135) == 0, "a zero-size code is refused");
    UL_CHECK(qr_scale_for(33, 0, 0) == 0, "a zero-size panel is refused");
    UL_CHECK(qr_module_at(10, 0) < 0, "a zero scale cannot divide");
}

/* Every pixel of the square maps to a module or to quiet zone, and the code's
 * modules are each covered by exactly scale x scale pixels. */
static void test_every_module_gets_its_pixels(void) {
    const int size = 25, scale = 4;
    const int px = qr_square_modules(size) * scale;
    int covered[64] = {0};
    bool in_range = true;

    for (int i = 0; i < px; i++) {
        const int m = qr_module_at(i, scale);
        if (m >= 0) {
            if (m >= size) {
                continue; /* trailing quiet zone */
            }
            covered[m]++;
        } else if (m < -QR_QUIET_ZONE_MODULES) {
            in_range = false;
        }
    }
    UL_CHECK(in_range, "no pixel maps outside the quiet zone");

    bool exact = true;
    for (int m = 0; m < size; m++) {
        if (covered[m] != scale) {
            exact = false;
        }
    }
    UL_CHECK(exact, "every module of the code is exactly `scale` pixels wide");
}

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
    test_the_quiet_zone_is_on_all_four_sides();
    test_the_code_always_fits_on_the_panel();
    test_a_panel_too_small_is_refused();
    test_every_module_gets_its_pixels();
}
