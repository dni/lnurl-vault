/* Host transport for boards whose chip has no USB-OTG peripheral and reach
 * the host through an external USB-UART bridge instead (the classic ESP32
 * T-Display uses a CH9102 on UART0).
 *
 * To a browser this is indistinguishable from native USB-CDC: navigator.serial
 * enumerates a CH9102/CP210x bridge exactly as it does a CDC-ACM device, so
 * docs/PROTOCOL.md's newline-delimited JSON is carried identically.
 *
 * IMPORTANT: UART0 is also where ESP-IDF's console logs by default, and log
 * lines interleaved with responses corrupt the stream in a way that looks like
 * random protocol failures. sdkconfig.defaults.esp32 sets CONFIG_ESP_CONSOLE_NONE
 * for exactly this reason -- see that file, and note the same trap exists in
 * different clothing on the S3 build.
 *
 * Unlike the USB-CDC transport this runs its own task rather than a driver
 * callback, so a slow command blocks only this task. */
#ifdef LNURLVAULT_BOARD_T_DISPLAY

#include "serial_uart.h"

#include <string.h>

#include "dispatcher.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "line_proto.h"
#include "vault_lock.h"

static const char *TAG = "serial_uart";

#define UART_PORT UART_NUM_0
#define UART_TX_PIN 1
#define UART_RX_PIN 3
#define UART_BAUD 115200
#define UART_RX_RING 2048
#define RESP_BUF_SIZE 4096

static char g_resp[RESP_BUF_SIZE];
static line_proto_t g_lp;

static void on_line(const char *line, void *ctx) {
    (void)ctx;
    vault_lock_acquire();
    dispatcher_handle(line, g_resp, sizeof(g_resp));
    vault_lock_release();

    size_t n = strlen(g_resp);
    uart_write_bytes(UART_PORT, g_resp, n);
    uart_write_bytes(UART_PORT, "\n", 1);
}

static void uart_task(void *arg) {
    (void)arg;
    uint8_t chunk[128];
    for (;;) {
        int n = uart_read_bytes(UART_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        if (n > 0) {
            line_proto_feed(&g_lp, chunk, (size_t)n, on_line, NULL);
        }
    }
}

void serial_uart_start(void) {
    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(UART_PORT, UART_RX_RING, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return;
    }
    uart_param_config(UART_PORT, &cfg);
    uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    line_proto_init(&g_lp);
    /* 6KB: dispatcher_handle materialises a note_meta_t (~408 bytes) plus a
     * JSON writer on this stack while streaming list_notes. */
    xTaskCreate(uart_task, "serial_uart", 6144, NULL, 5, NULL);
}

#endif /* LNURLVAULT_BOARD_T_DISPLAY */
