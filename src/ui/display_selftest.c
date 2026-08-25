/* Boot-time panel diagnostics, each behind its own build flag, neither in a
 * shipping build.
 *
 * The display is the one component with no telemetry: nothing it does comes
 * back over the wire, so a wrong colour order, a wrong rotation, a wrong RAM
 * offset and a panel that is not being driven at all all present identically
 * -- as somebody describing what they see. Guessing between them from the
 * source costs a flash cycle per guess. These put the answer on the glass.
 *
 *   LNURLVAULT_DISPLAY_SELFTEST  colour, geometry, rotation, mirroring
 *   LNURLVAULT_QR_SELFTEST       QR density ladder
 *
 * The geometry test earned its keep immediately: the T-Display's mirror
 * settings were established with it in three flash cycles, and it revealed
 * that with swap_xy enabled esp_lcd's mirror axes are transposed relative to
 * the surface we draw into -- which reads as a bug in the wrong place if all
 * you have is the source.
 */
#if defined(LNURLVAULT_DISPLAY_SELFTEST) || defined(LNURLVAULT_QR_SELFTEST)

#include "display_selftest.h"

#include <string.h>

#include "board.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qr_display.h"
#include "qr_capacity.h"
#include "qrcode.h"

#ifdef LNURLVAULT_BOARD_T_DISPLAY
#include "driver/uart.h"
#include <stdarg.h>
#include <stdio.h>

/* Writes a diagnostic line down the same UART the protocol uses.
 *
 * This board sets CONFIG_ESP_CONSOLE_NONE (UART0 carries the command
 * protocol, so console logging would corrupt it), which leaves the QR path
 * with no telemetry at all -- and three separate wrong hypotheses were talked
 * through before anyone thought to just ask the device. Safe here only
 * because the self-test runs while nothing else is transmitting. */
static void diag(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        uart_write_bytes(UART_NUM_0, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
        uart_write_bytes(UART_NUM_0, "\n", 1);
    }
}
#else
#define diag(...) ((void)0)
#endif

static const char *TAG = "selftest";

#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_RED 0xF800
#define C_GREEN 0x07E0
#define C_BLUE 0x001F
#define C_YELLOW 0xFFE0

/* Blocks until either button is pressed and released, or timeout_ms passes.
 * Polled directly rather than through button_fsm: this runs before ui_task
 * owns the buttons, and it wants a plain "did somebody press it", not a
 * classified gesture. */
static void wait_for_press(uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (waited < timeout_ms && !board_button_1_pressed() && !board_button_2_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(25));
        waited += 25;
    }
    while (board_button_1_pressed() || board_button_2_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    vTaskDelay(pdMS_TO_TICKS(150)); /* let the contacts settle */
}

#ifdef LNURLVAULT_DISPLAY_SELFTEST
static void hold(const char *what, uint16_t color, int ms) {
    ESP_LOGI(TAG, "%s", what);
    display_fill_rect(0, 0, display_width(), display_height(), color);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void display_selftest_run(void) {
    if (!display_ready()) {
        ESP_LOGE(TAG, "no panel; nothing to test");
        return;
    }
    const int w = display_width();
    const int h = display_height();
    ESP_LOGI(TAG, "surface reports %dx%d", w, h);

    hold("1/6 RED", C_RED, 2500);
    hold("2/6 GREEN", C_GREEN, 2500);
    hold("3/6 BLUE", C_BLUE, 2500);
    hold("4/6 WHITE", C_WHITE, 2000);

    /* A frame inset 10px from every edge. The first version drew a one-pixel
     * border and proved nothing -- a single line is invisible at arm's length.
     * An inset frame makes an uneven margin obvious instead: if the gap on an
     * axis is wrong, one side is fatter than its opposite, or vanishes
     * off-screen entirely. */
    ESP_LOGI(TAG, "5/6 white frame, even 10px margin on all four sides");
    {
        const int m = 10;
        display_fill_rect(0, 0, w, h, C_BLACK);
        display_fill_rect(m, m, w - 2 * m, 4, C_WHITE);
        display_fill_rect(m, h - m - 4, w - 2 * m, 4, C_WHITE);
        display_fill_rect(m, m, 4, h - 2 * m, C_WHITE);
        display_fill_rect(w - m - 4, m, 4, h - 2 * m, C_WHITE);
    }
    vTaskDelay(pdMS_TO_TICKS(8000));

    /* Asymmetric corners. Rotation and mirroring both move an image around;
     * only an asymmetric image tells them apart. Clockwise from top left:
     * RED, GREEN, BLUE, YELLOW. */
    ESP_LOGI(TAG, "6/6 corners: expect red TL, green TR, blue BR, yellow BL");
    const int s = h / 3;
    display_fill_rect(0, 0, w, h, C_BLACK);
    display_fill_rect(0, 0, s, s, C_RED);
    display_fill_rect(w - s, 0, s, s, C_GREEN);
    display_fill_rect(w - s, h - s, s, s, C_BLUE);
    display_fill_rect(0, h - s, s, s, C_YELLOW);
    vTaskDelay(pdMS_TO_TICKS(12000));

    ESP_LOGI(TAG, "geometry self-test done");
}
#endif /* LNURLVAULT_DISPLAY_SELFTEST */

#ifdef LNURLVAULT_QR_SELFTEST
/* A QR that will not scan has two very different causes -- a renderer drawing
 * the wrong modules, or a correct render whose modules are too small for a
 * camera to resolve -- and they look identical on the glass. Encoding
 * progressively longer strings separates them, and locates the exact payload
 * length at which a given panel stops being usable for the offline handoff.
 *
 * Each code waits for a button rather than a timer: scanning a marginal code
 * takes as long as it takes, and on a fixed delay the interesting ones scroll
 * past before they can be tried. */
void qr_selftest_run(void) {
    if (!display_ready()) {
        ESP_LOGE(TAG, "no panel; nothing to test");
        return;
    }
    /* Dump the encoder's module grid as text before drawing it. Scanning is
     * a lossy oracle -- it folds encoder, renderer, optics and the phone's
     * decoder into one bit -- and it had us adjusting module size when the
     * evidence stopped fitting that story. This prints what the encoder
     * actually produced so it can be diffed against a reference
     * implementation on the host, separating "we drew it wrong" from "the
     * camera cannot read it". */
    static const char *const ladder[] = {
        "HELLO",
        "lnurlw://m.ln?k1=deadbeef",
        "lnurlw://m.ln?k1=00000000000000000000000000000000000000000000000000000000000000ff",
        "lnurlw://m.ln?k1=00000000000000000000000000000000000000000000000000000000000000ff&amount=21000",
        "lnurlw://mint.example.com?k1=00000000000000000000000000000000000000000000000000000000000000ff&amount=21000",
    };
    const unsigned n = (unsigned)(sizeof(ladder) / sizeof(ladder[0]));

    diag("SELFTEST screen=%dx%d", display_width(), display_height());
    for (unsigned i = 0; i < n; i++) {
        const size_t len = strlen(ladder[i]);
        const uint8_t ver = qr_version_for_length(len);
        const int modules = ver ? (4 * ver + 17) + 8 : 0;
        const int shorter = display_width() < display_height() ? display_width() : display_height();
        const int scale = modules ? shorter / modules : 0;
        diag("QR %u/%u len=%u ver=%u modules=%d scale=%d px=%d buf=%d", i + 1, n, (unsigned)len,
             ver, modules, scale, modules * scale, modules * scale * modules * scale * 2);

        bool shown = qr_display_show(ladder[i], NULL);
        diag("QR %u/%u shown=%s", i + 1, n, shown ? "YES" : "NO");
        if (!shown) {
            display_fill_rect(0, 0, display_width(), display_height(), C_RED);
        }
        wait_for_press(120000);
        diag("QR %u/%u advanced", i + 1, n);
    }
    ESP_LOGI(TAG, "QR ladder done");
}
#endif /* LNURLVAULT_QR_SELFTEST */

#endif /* either selftest */
