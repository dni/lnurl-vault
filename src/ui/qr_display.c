/* QR encoding needs a Reed-Solomon encoder plus per-version capacity/
 * alignment/format-info tables straight out of ISO/IEC 18004 — large,
 * precise, table-heavy code where a single transcription slip produces a
 * code that silently fails to scan, with no way to catch that in this
 * environment (no physical device, no camera, no QR decoder to round-trip
 * against). Hand-writing it from memory the way sha256.c was would have
 * been a real correctness risk with no verification path, so this
 * integrates against a real, widely-used library instead:
 *
 *   ricmoo/QRCode, https://github.com/ricmoo/QRCode (MIT, single-file, no
 *   heap allocation — written for exactly this microcontroller-plus-small-
 *   display use case).
 *
 * Vendor qrcode.h and qrcode.c from that repo directly into src/ui/ (flat,
 * no subdirectory). Two mechanisms that would auto-fetch it were both
 * empirically tried and confirmed NOT to work for this framework/library
 * combination, not just assumed to fail:
 *   - platformio.ini `lib_deps = ricmoo/QRCode`: the library is genuinely
 *     in PlatformIO's index (`pio pkg search qrcode` finds it), but
 *     PlatformIO's Library Dependency Finder never engages at all for
 *     `framework = espidf` — a real `pio run` here logs "LDF: ... Found 0
 *     compatible libraries" / "No dependencies" regardless.
 *   - A git dependency in src/idf_component.yml (the mechanism that does
 *     work for esp_tinyusb, see that file): ESP-IDF's component manager
 *     happily clones the repo, but then refuses to build it — "Directory
 *     '.../managed_components/qrcode' does not contain a component" —
 *     because ricmoo/QRCode's repo root has no CMakeLists.txt of its own;
 *     it's a plain library meant for direct inclusion, not shaped as an
 *     ESP-IDF component.
 * Manual vendoring is what's actually left, and what's been confirmed to
 * compile (mod the bug below).
 *
 * The API this file assumes (qrcode_initText, qrcode_getModule, the
 * QRCode struct's .size field, ECC_LOW) was confirmed directly against
 * v0.0.1's qrcode.h — see below for one real, confirmed incompatibility
 * and its fix, found by inspecting that header and iterating against a
 * real build in this environment before settling on this fix (two other
 * approaches were tried first and empirically failed — see the git
 * history of this comment, or just trust that this one actually compiles).
 *
 * REQUIRED PATCH TO THE VENDORED HEADER: qrcode.h unconditionally shims
 * its own bool/true/false in C mode —
 *     #ifndef __cplusplus
 *     typedef unsigned char bool;
 *     static const bool false = 0;
 *     static const bool true = 1;
 *     #endif
 * — which this project's ESP-IDF version (6.0.1, GCC 15) breaks in a way
 * no amount of #undef/#include reshuffling on our side can work around:
 * this toolchain's CMake unconditionally forces `-std=gnu23` for chip
 * targets with no override hook (confirmed against build.cmake — not a
 * Kconfig option, a hardcoded `list(APPEND c_compile_options
 * "-std=gnu23")` for any non-"linux" IDF_TARGET), and in C23, bool/true/
 * false are real language keywords, not <stdbool.h> macros — so `typedef
 * unsigned char bool;` fails outright ("'bool' cannot be defined via
 * 'typedef'"), and nothing short of not compiling under C23 would dodge
 * it, which this project doesn't control. Since C99 and later already
 * provide bool/true/false one way or another (macros pre-C23, keywords
 * from C23 on) and ESP-IDF is never C89, this shim is simply unnecessary
 * for our purposes regardless of standard version — so the fix is to
 * delete it, as an automated step every time the library is (re)vendored,
 * not a one-off hand-edit:
 *
 *   sed -i '/typedef unsigned char bool;/d; \
 *           /static const bool false = 0;/d; \
 *           /static const bool true = 1;/d' src/ui/qrcode.h
 *
 * (See README.md's Build & flash section and .github/workflows/ci.yml's
 * "Vendor QR library" step — both run this same sed command.) This leaves
 * an empty, harmless `#ifndef __cplusplus` / `#endif` wrapper behind. */
#include "qr_display.h"

#include "display.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "qrcode.h"

#define QUIET_ZONE_MODULES 4
#define MIN_QR_VERSION 4
#define MAX_QR_VERSION 20 /* version 20 byte-mode/ECC-L capacity is far above any URL this device builds */

#define QR_WHITE 0xFFFF
#define QR_BLACK 0x0000

/* Generously sized for MAX_QR_VERSION rather than relying on the exact
 * calling convention of the library's own buffer-size helper (function vs.
 * macro has varied across similar libraries) — see this file's header
 * comment. */
static uint8_t g_qr_buf[1200];

bool qr_display_show(const char *text) {
    esp_lcd_panel_handle_t panel = display_panel_handle();
    if (!display_ready() || !panel || !text || !text[0]) {
        return false;
    }

    QRCode qrcode;
    bool encoded = false;
    for (uint8_t v = MIN_QR_VERSION; v <= MAX_QR_VERSION; v++) {
        if (qrcode_initText(&qrcode, g_qr_buf, v, ECC_LOW, text) == 0) {
            encoded = true;
            break;
        }
    }
    if (!encoded) {
        return false;
    }

    const int screen_w = display_width();
    const int screen_h = display_height();

    /* The drawn square is the code plus its quiet zone on all four sides. It
     * has to fit the SHORTER screen axis: sizing from the height alone
     * produced a square wider than a landscape screen, so the centring
     * arithmetic below went negative and the code was drawn partly off the
     * panel. */
    const int modules = qrcode.size + 2 * QUIET_ZONE_MODULES;
    const int shorter = screen_w < screen_h ? screen_w : screen_h;
    const int scale = shorter / modules;
    if (scale < 1) {
        return false; /* screen too small for this version at 1px per module */
    }

    const int qr_px = modules * scale;
    const int x0 = (screen_w - qr_px) / 2;
    const int y0 = (screen_h - qr_px) / 2;

    /* Render the whole square into one buffer and send it as a single
     * transfer. Drawing module-by-module would be tens of thousands of
     * queued transactions, and reusing one scratch row for rows that differ
     * would race the DMA that esp_lcd_panel_draw_bitmap() queues against it.
     * At the largest size this fits (170px square) the buffer is ~58KB. */
    uint16_t *buf = heap_caps_malloc((size_t)qr_px * (size_t)qr_px * sizeof(uint16_t),
                                      MALLOC_CAP_DMA);
    if (!buf) {
        return false;
    }

    for (int py = 0; py < qr_px; py++) {
        /* Quiet zone is part of the square, so module coordinates are offset
         * by it. Previously module (0,0) was drawn at the square's own
         * corner, which pushed the entire margin onto the right and bottom
         * and left as little as 3px above the code where the spec wants four
         * modules. Scanners vary in how much they tolerate; the failure mode
         * is a code that simply will not read, with nothing on screen to say
         * why. */
        const int my = py / scale - QUIET_ZONE_MODULES;
        for (int px = 0; px < qr_px; px++) {
            const int mx = px / scale - QUIET_ZONE_MODULES;
            bool dark = mx >= 0 && my >= 0 && mx < qrcode.size && my < qrcode.size &&
                        qrcode_getModule(&qrcode, (uint8_t)mx, (uint8_t)my);
            buf[py * qr_px + px] = dark ? QR_BLACK : QR_WHITE;
        }
    }

    /* Clear to white first so no earlier state colour shows around the code:
     * a QR sitting on a coloured margin still scans, but it looks like a
     * glitch on a device whose whole job here is to be trusted at a glance. */
    display_fill_rect(0, 0, screen_w, screen_h, QR_WHITE);
    esp_lcd_panel_draw_bitmap(panel, x0, y0, x0 + qr_px, y0 + qr_px, buf);

    /* The blit is queued, not completed, so the buffer cannot be freed here
     * without risking the DMA reading freed memory. Hold it until the next
     * call, by which time the transfer is long done -- the QR stays on screen
     * until the owner dismisses it, which is a human-scale delay. */
    static uint16_t *prev = NULL;
    if (prev) {
        heap_caps_free(prev);
    }
    prev = buf;
    return true;
}
