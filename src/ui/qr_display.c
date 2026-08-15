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
#include "esp_lcd_panel_ops.h"
#include "qrcode.h"

#define QUIET_ZONE_MODULES 4
#define MIN_QR_VERSION 4
#define MAX_QR_VERSION 20 /* version 20 byte-mode/ECC-L capacity is far above any URL this device builds */

/* Generously sized for MAX_QR_VERSION rather than relying on the exact
 * calling convention of the library's own buffer-size helper (function vs.
 * macro has varied across similar libraries) — see this file's header
 * comment. */
static uint8_t g_qr_buf[1200];

bool qr_display_show(const char *text) {
    esp_lcd_panel_handle_t panel = display_panel_handle();
    if (!panel || !text || !text[0]) {
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

    int modules = qrcode.size + 2 * QUIET_ZONE_MODULES;
    int scale = LCD_HEIGHT / modules;
    if (scale < 1) {
        scale = 1;
    }
    int qr_px = modules * scale;
    int x0 = (LCD_WIDTH - qr_px) / 2;
    int y0 = (LCD_HEIGHT - qr_px) / 2;

    static uint16_t white_row[LCD_WIDTH];
    for (int i = 0; i < LCD_WIDTH; i++) {
        white_row[i] = 0xFFFF;
    }
    for (int y = 0; y < LCD_HEIGHT; y++) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_WIDTH, y + 1, white_row);
    }

    static uint16_t black_row[LCD_WIDTH];
    for (int i = 0; i < LCD_WIDTH; i++) {
        black_row[i] = 0x0000;
    }
    for (int my = 0; my < qrcode.size; my++) {
        for (int mx = 0; mx < qrcode.size; mx++) {
            if (!qrcode_getModule(&qrcode, mx, my)) {
                continue; /* white module: background already drawn */
            }
            int px = x0 + mx * scale;
            for (int dy = 0; dy < scale; dy++) {
                int py = y0 + my * scale + dy;
                if (py < 0 || py >= LCD_HEIGHT) {
                    continue;
                }
                esp_lcd_panel_draw_bitmap(panel, px, py, px + scale, py + 1, black_row);
            }
        }
    }
    return true;
}
