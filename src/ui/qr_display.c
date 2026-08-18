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
 * provide bool/true/false — but pre-C23 they come from <stdbool.h> and
 * are not automatic, which is the part that matters here. So the shim is
 * REPLACED by that include rather than simply deleted: deleting it alone
 * left the header declaring `bool qrcode_getModule(...)` with nothing
 * defining bool, which compiles only where bool is already a keyword,
 * i.e. C23 and up. That is true of this firmware build and of nothing
 * else, so the header could not be included from an ordinary C11
 * translation unit at all. Including <stdbool.h> is correct under every
 * standard from C99 on, and it sits inside the library's own
 * `#ifndef __cplusplus` guard, where the shim was.
 *
 * This is an automated step, not a one-off hand-edit: it lives in
 * tools/vendor_qrcode.sh, which is what README.md's Build & flash section
 * and both workflows actually run. The script uses awk rather than the
 * `sed -i` this comment used to suggest — BSD sed takes an argument to
 * -i and GNU sed does not, so no single sed invocation works on both
 * macOS and CI; that failed for real once already. */
#include "qr_display.h"

#include "display.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include <string.h>

#include "qr_capacity.h"
#include "qrcode.h"

#define QR_WHITE 0xFFFF
#define QR_BLACK 0x0000

/* Sized for QR_MAX_VERSION; checked against the library at run time before
 * use, because getting this wrong overflows a buffer holding a bearer secret
 * rather than merely drawing badly. */
static uint8_t g_qr_buf[1400];

bool qr_display_show(const char *text) {
    esp_lcd_panel_handle_t panel = display_panel_handle();
    if (!display_ready() || !panel || !text || !text[0]) {
        return false;
    }

    QRCode qrcode;
    const size_t len = strlen(text);
    const uint8_t version = qr_version_for_length(len);
    if (version == 0) {
        return false; /* longer than any version we are prepared to render */
    }
    if (qrcode_getBufferSize(version) > sizeof(g_qr_buf)) {
        return false; /* refuse rather than overflow */
    }
    if (qrcode_initText(&qrcode, g_qr_buf, version, ECC_LOW, text) != 0) {
        return false;
    }

    const int screen_w = display_width();
    const int screen_h = display_height();

    /* The drawn square is the code plus its quiet zone on all four sides. It
     * has to fit the SHORTER screen axis: sizing from the height alone
     * produced a square wider than a landscape screen, so the centring
     * arithmetic below went negative and the code was drawn partly off the
     * panel. */
    const int modules = qr_square_modules(qrcode.size);
    const int ideal_scale = qr_scale_for(qrcode.size, screen_w, screen_h);
    if (ideal_scale < 1) {
        return false; /* screen too small for this version at 1px per module */
    }

    /* One buffer for the whole square, sent as a single transfer. Drawing
     * module-by-module would be tens of thousands of queued transactions, and
     * reusing one scratch row for rows that differ would race the DMA that
     * esp_lcd_panel_draw_bitmap() queues against it.
     *
     * The previous code's buffer is released BEFORE this one is claimed, not
     * after. Holding both at once needed ~65KB of DMA-capable RAM, which
     * fails on a classic ESP32 with NimBLE up -- observed on hardware as the
     * third and later codes in the self-test ladder refusing to render while
     * the first two worked. Freeing first halves the peak to a single buffer.
     *
     * Safe to free here rather than on transfer-completion because the only
     * thing that triggers another render is a person pressing a button, which
     * is many orders of magnitude slower than the blit. */
    static uint16_t *held = NULL;
    if (held) {
        heap_caps_free(held);
        held = NULL;
    }

    /* Degrade rather than show nothing: if the ideal scale will not fit in
     * DMA-capable memory, step it down. A slightly smaller code that renders
     * beats a blank screen on a device whose whole job here is to show one. */
    uint16_t *buf = NULL;
    int scale = ideal_scale;
    int qr_px = 0;
    for (; scale >= 1; scale--) {
        qr_px = modules * scale;
        buf = heap_caps_malloc((size_t)qr_px * (size_t)qr_px * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (buf) {
            break;
        }
    }
    if (!buf) {
        return false;
    }

    int x0 = 0, y0 = 0;
    qr_origin(qrcode.size, scale, screen_w, screen_h, &x0, &y0);

    for (int py = 0; py < qr_px; py++) {
        /* Quiet zone is part of the square, so module coordinates are offset
         * by it. Previously module (0,0) was drawn at the square's own
         * corner, which pushed the entire margin onto the right and bottom
         * and left as little as 3px above the code where the spec wants four
         * modules. */
        const int my = qr_module_at(py, scale);
        for (int px = 0; px < qr_px; px++) {
            const int mx = qr_module_at(px, scale);
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

    held = buf;
    return true;
}
