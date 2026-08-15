/* NOTE: unverified by compilation, and additionally depends on a THIRD-
 * PARTY LIBRARY NOT INCLUDED IN THIS REPO (see README.md's Build section
 * and "Known limitations"). QR encoding needs a Reed-Solomon encoder plus
 * per-version capacity/alignment/format-info tables straight out of
 * ISO/IEC 18004 — large, precise, table-heavy code where a single
 * transcription slip produces a code that silently fails to scan, with no
 * way to catch that in this environment (no physical device, no camera, no
 * QR decoder to round-trip against). Hand-writing it from memory the way
 * sha256.c was would have been a real correctness risk with no verification
 * path, so this integrates against a real, widely-used library instead:
 *
 *   https://github.com/ricmoo/QRCode  (MIT, single-file, no heap allocation
 *   — written for exactly this microcontroller-plus-small-display use case)
 *
 * Vendor qrcode.h and qrcode.c from that repo directly into src/ui/ (flat,
 * no subdirectory — CMakeLists.txt's existing `SRC_DIRS ... "ui"` picks up
 * qrcode.c automatically once it's there, no build file changes needed).
 * The API this file assumes (qrcode_initText, qrcode_getModule, the
 * QRCode struct's .size field, ECC_LOW) matches that library's README as of
 * this writing; if a build error points here, diff against the actual
 * vendored qrcode.h you pulled in — that header, not this comment, is the
 * source of truth for its own API. */
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
