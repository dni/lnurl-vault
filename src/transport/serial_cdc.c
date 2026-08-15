/* Confirmed to compile against ESP-IDF 6.0.1 as part of a full firmware
 * build, with espressif/esp_tinyusb ^1.4 as resolved by the component
 * manager at that time (see README.md's "Status" section and
 * src/idf_component.yml) — including the exact tinyusb_config_cdcacm_t /
 * tusb_cdc_acm_init shape assumed below. esp_tinyusb's CDC-ACM API has
 * changed across its own release history (0.x vs 1.x) somewhat
 * independently of ESP-IDF versions, though, so on a different resolved
 * version, that's still the first thing to check if a build error points
 * here. Actual USB-CDC behavior (does a browser's navigator.serial
 * actually see and talk to this) has never been checked against real
 * hardware. Everything past a complete line reaching dispatcher_handle()
 * below is the same tested logic the native tests cover. */
#include "serial_cdc.h"

#include <string.h>

#include "dispatcher.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "vault_lock.h"

static const char *TAG = "serial_cdc";

#define LINE_BUF_SIZE 2048
#define RESP_BUF_SIZE 4096

static char g_line_buf[LINE_BUF_SIZE];
static size_t g_line_len = 0;
static char g_resp_buf[RESP_BUF_SIZE];

static void handle_rx(int itf, cdcacm_event_t *event) {
    (void)event;
    uint8_t chunk[64];
    size_t rx_size = 0;
    esp_err_t err = tinyusb_cdcacm_read(itf, chunk, sizeof(chunk), &rx_size);
    if (err != ESP_OK) {
        return;
    }

    for (size_t i = 0; i < rx_size; i++) {
        char c = (char)chunk[i];
        if (c == '\n' || c == '\r') {
            if (g_line_len > 0) {
                g_line_buf[g_line_len] = '\0';
                vault_lock_acquire();
                dispatcher_handle(g_line_buf, g_resp_buf, sizeof(g_resp_buf));
                vault_lock_release();
                size_t resp_len = strlen(g_resp_buf);
                tinyusb_cdcacm_write_queue(itf, (const uint8_t *)g_resp_buf, resp_len);
                uint8_t nl = '\n';
                tinyusb_cdcacm_write_queue(itf, &nl, 1);
                tinyusb_cdcacm_write_flush(itf, 0);
                g_line_len = 0;
            }
            continue;
        }
        if (g_line_len + 1 < LINE_BUF_SIZE) {
            g_line_buf[g_line_len++] = c;
        }
        /* else: silently drop overlong line content; g_line_len resets on
         * the next newline and the dispatcher will see a truncated (likely
         * malformed -> bad_request) line rather than the device wedging. */
    }
}

void serial_cdc_start(void) {
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,
        .string_descriptor = NULL,
        .external_phy = false,
        .configuration_descriptor = NULL,
    };
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return;
    }

    tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 256,
        .callback_rx = &handle_rx,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    err = tusb_cdc_acm_init(&acm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tusb_cdc_acm_init failed: %s", esp_err_to_name(err));
    }
}
