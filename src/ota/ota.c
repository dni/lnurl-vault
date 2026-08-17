/* forgesworn/heartwood-esp32's own OTA implementation (its inspiration for
 * this file — see README.md's Status section) hit a real, sharp gotcha
 * worth flagging here even though it doesn't apply to this file: its
 * device's default ESP-IDF console log output shares the same USB-CDC
 * channel its framed OTA protocol uses, so a stray log line during
 * esp_ota_begin() corrupted in-flight protocol frames — they suppress
 * logging around it entirely. This project's console is on UART0/USB-
 * Serial-JTAG instead (CONFIG_ESP_CONSOLE_UART_DEFAULT, see
 * sdkconfig.t-display-s3) — a completely separate physical channel from
 * the TinyUSB CDC-ACM interface serial_cdc.c owns — so ESP_LOGE calls
 * below cannot interleave with OTA response frames the way heartwood's
 * could. Confirmed by reading sdkconfig, not assumed; re-check this if
 * CONFIG_ESP_CONSOLE_* is ever changed to route through the same USB-CDC
 * device serial_cdc.c uses. */
#include "ota.h"

#include "esp_log.h"
#include "esp_ota_ops.h"

#include "device_reboot.h"

static const char *TAG = "ota";

static esp_ota_handle_t g_handle;
static const esp_partition_t *g_partition;
static bool g_active;

bool ota_write_begin(uint32_t total_size) {
    g_partition = esp_ota_get_next_update_partition(NULL);
    if (!g_partition) {
        ESP_LOGE(TAG, "no inactive OTA partition available");
        return false;
    }
    esp_err_t err = esp_ota_begin(g_partition, total_size, &g_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return false;
    }
    g_active = true;
    return true;
}

bool ota_write_chunk(const uint8_t *data, size_t len) {
    if (!g_active) {
        return false;
    }
    esp_err_t err = esp_ota_write(g_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool ota_write_finish(void) {
    if (!g_active) {
        return false;
    }
    esp_err_t err = esp_ota_end(g_handle);
    g_active = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_ota_set_boot_partition(g_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return false;
    }
    device_reboot_delayed();
    return true;
}

void ota_write_abort(void) {
    if (g_active) {
        esp_ota_abort(g_handle);
        g_active = false;
    }
}
