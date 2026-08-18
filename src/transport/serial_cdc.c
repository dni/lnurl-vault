/* Confirmed to compile against ESP-IDF 6.0.1 as part of a full firmware
 * build, with espressif/esp_tinyusb ^1.4 as resolved by the component
 * manager at that time (see README.md's "Status" section and
 * src/idf_component.yml) — including the exact tinyusb_config_cdcacm_t /
 * tusb_cdc_acm_init shape assumed below. esp_tinyusb's CDC-ACM API has
 * changed across its own release history (0.x vs 1.x) somewhat
 * independently of ESP-IDF versions, though, so on a different resolved
 * version, that's still the first thing to check if a build error points
 * here. Everything past a complete line reaching dispatcher_handle() below
 * is the same tested logic the native tests cover.
 *
 * WHY THIS FILE LOOKS LIKE THIS — read before changing it. Full
 * history of the hardware runs that led here is in README.md's Status
 * section; this is just the load-bearing conclusions:
 *
 * 1. handle_rx() is esp_tinyusb's callback_rx, invoked by TinyUSB core from
 *    within tud_task()'s own event-processing call chain (confirmed by
 *    reading managed_components/espressif__esp_tinyusb/tusb_cdc_acm.c and
 *    .../espressif__tinyusb/src/class/cdc/cdc_device.c directly) — i.e. it
 *    runs ON the one TinyUSB task that services the USB peripheral. A
 *    *blocking* tinyusb_cdcacm_write_flush(itf, timeout) called from inside
 *    it self-deadlocks: the flush waits on a transfer-complete event that
 *    only tud_task() can deliver, and tud_task() can't get back to its own
 *    loop until this nested callback returns. Confirmed on real hardware.
 *    Do not call a blocking flush from handle_rx().
 * 2. tinyusb_cdcacm_write_queue() returns the number of bytes it actually
 *    staged, silently clamped to whatever's free in the 512-byte TX ring
 *    buffer (CONFIG_TINYUSB_CDC_TX_BUFSIZE) — ignoring that return value
 *    means a response that didn't fully fit gets silently truncated or
 *    (if the buffer was already full) dropped outright. Always check it
 *    and retry, which is why the write lives in its own task
 *    (serial_tx_task) rather than inline in handle_rx(): retrying needs to
 *    wait for space to free up, and waiting inline would hit problem #1.
 * 3. That retry must be bounded. An earlier unbounded "loop until every
 *    byte is confirmed queued" version permanently wedged serial_tx_task,
 *    and with it every response queued behind the stuck one, the moment
 *    write_queue() stopped making progress — confirmed on real hardware.
 *    give_up_at_us below is that bound.
 * 4. handle_rx() itself used to call dispatcher_handle() (JSON parse/build,
 *    SHA-256, vault iteration — real, non-trivial work, not a fixed small
 *    cost) directly, inline, still nested inside tud_task() per point 1.
 *    That's the read-side version of the exact problem points 1-3 already
 *    fixed on the write side: while tud_task() is captive running
 *    dispatcher_handle() for message N, it can't service the USB
 *    peripheral for anything else — not the next incoming message, not
 *    housekeeping for the one in flight. From the host's side that reads
 *    as "the device is sometimes slow to even notice a command," with the
 *    response itself feeling instant once it finally arrives (reported
 *    directly against real hardware) — because dispatcher_handle() for any
 *    one command really is fast in isolation; the delay was tud_task()
 *    not getting back to its own loop promptly, not dispatcher_handle()
 *    being slow. Fixed the same way as the write side: handle_rx() now
 *    only assembles a complete line and hands it to serial_rx_task over
 *    g_rx_queue; dispatcher_handle() (and the vault_lock around it) moved
 *    there, off tud_task() entirely.
 *
 * Even with all of the above, hardware testing (test/hardware/
 * test_serial.py) had shown real, intermittent flakiness — occasional
 * dropped or torn responses, worse over a long-running boot — that
 * predated point 4's fix and was NOT explained by anything points 1-3
 * addressed (heap growth and BLE/CPU-core contention were both separately
 * ruled out against real hardware; see README.md's Status section). Point
 * 4 is a real, confirmed-by-reading-the-code violation of this file's own
 * "never block tud_task()" rule, and a plausible match for that report,
 * but is NOT yet confirmed on hardware to be the actual cause — treat it
 * as the strongest still-untested lead, not a confirmed fix. */
#ifdef LNURLVAULT_BOARD_T_DISPLAY_S3

#include "serial_cdc.h"

#include <string.h>

#include "cmd_lock.h"
#include "dispatcher.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "vault_lock.h"

static const char *TAG = "serial_cdc";

#define LINE_BUF_SIZE 2048
#define RESP_BUF_SIZE 4096
#define TX_QUEUE_DEPTH 4
#define RX_QUEUE_DEPTH 4

static char g_line_buf[LINE_BUF_SIZE];
static size_t g_line_len = 0;
static char g_resp_buf[RESP_BUF_SIZE];

typedef struct {
    int itf;
    size_t len;
    uint8_t data[RESP_BUF_SIZE];
} tx_item_t;

static QueueHandle_t g_tx_queue;

typedef struct {
    int itf;
    size_t len; /* not including the NUL below */
    char data[LINE_BUF_SIZE];
} rx_item_t;

static QueueHandle_t g_rx_queue;

/* Generous on purpose: test_serial.py's own docstring documents this
 * device having a real, unexplained ~2s+ baseline response latency even
 * when nothing is actually stuck, so a short cap here gives up on
 * transfers that would otherwise have gone through. This is meant to catch
 * only genuine, permanent stalls (write_queue() never freeing space again),
 * not to police normal-but-slow delivery — see this file's header comment
 * for why an unbounded wait isn't safe either. */
#define TX_GIVE_UP_US (8 * 1000 * 1000)

/* Runs on its own task, never nested inside TinyUSB's rx callback — see
 * this file's header comment for why that distinction is what makes the
 * blocking flush below safe here when it wasn't in handle_rx(). */
static void serial_tx_task(void *arg) {
    (void)arg;
    /* static: tx_item_t is ~4.1KB (RESP_BUF_SIZE-sized data[]) — as a plain
     * stack local it overflowed this task's 4096-byte stack outright (a
     * single local bigger than the whole stack budget), which reset the
     * device the moment a response was queued. This task processes one
     * item at a time, so a static buffer is safe here. */
    static tx_item_t item;
    for (;;) {
        if (xQueueReceive(g_tx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        size_t sent = 0;
        int64_t give_up_at_us = esp_timer_get_time() + TX_GIVE_UP_US;
        while (sent < item.len) {
            size_t queued =
                tinyusb_cdcacm_write_queue(item.itf, item.data + sent, item.len - sent);
            sent += queued;
            if (queued > 0) {
                /* Non-blocking: just kicks transmission of what's staged so
                 * far. A blocking flush here on every iteration was tried
                 * and made things worse (see header comment) — only wait,
                 * below, when actually stalled. */
                tinyusb_cdcacm_write_flush(item.itf, 0);
            } else if (esp_timer_get_time() > give_up_at_us) {
                ESP_LOGW(TAG, "tx stalled, giving up after %u/%u bytes",
                         (unsigned)sent, (unsigned)item.len);
                break;
            } else {
                /* No room queued this pass; give tud_task() a moment to
                 * drain the ring buffer before retrying. */
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
        tinyusb_cdcacm_write_flush(item.itf, 0);
    }
}

/* Runs on its own task, never nested inside TinyUSB's rx callback — see
 * this file's header comment (point 4) for why dispatcher_handle() moved
 * here instead of running inline in handle_rx(). */
static void serial_rx_task(void *arg) {
    (void)arg;
    /* static: see serial_tx_task()'s comment above — same reasoning
     * (rx_item_t is ~2KB, this task processes one item at a time). */
    static rx_item_t item;
    for (;;) {
        if (xQueueReceive(g_rx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* cmd_lock before vault_lock, always — see cmd_lock.h. main.c's
         * confirm callbacks release vault_lock around the up-to-30s wait
         * for the owner to press a button, so ui_task can still run; this
         * task's other half of that arrangement is holding cmd_lock across
         * the whole command, which is what keeps the dispatcher's own OTA
         * session state safe from the other transport meanwhile. */
        cmd_lock_acquire();
        dispatcher_set_source(DISPATCH_SOURCE_SERIAL);
        vault_lock_acquire();
        dispatcher_handle(item.data, g_resp_buf, sizeof(g_resp_buf) - 1);
        vault_lock_release();
        cmd_lock_release();
        size_t resp_len = strlen(g_resp_buf);
        g_resp_buf[resp_len++] = '\n';

        static tx_item_t tx;
        tx.itf = item.itf;
        tx.len = resp_len;
        memcpy(tx.data, g_resp_buf, resp_len);
        if (xQueueSend(g_tx_queue, &tx, 0) != pdTRUE) {
            ESP_LOGW(TAG, "tx queue full, dropping a %u-byte response", (unsigned)tx.len);
        }
    }
}

/* Only ever assembles bytes into a line and hands complete lines off to
 * serial_rx_task via g_rx_queue — see this file's header comment (point 4)
 * for why dispatcher_handle() must not run here, nested inside tud_task(). */
static void handle_rx(int itf, cdcacm_event_t *event) {
    (void)event;
    uint8_t chunk[64];
    /* Drain the whole FIFO: tud_cdc_rx_cb fires once per packet, so a single
     * 64-byte read leaves any surplus in the 512-byte FIFO until the next
     * packet arrives, and the backlog grows across a boot. Loop until empty. */
    for (;;) {
        size_t rx_size = 0;
        if (tinyusb_cdcacm_read(itf, chunk, sizeof(chunk), &rx_size) != ESP_OK || rx_size == 0) {
            return;
        }
        for (size_t i = 0; i < rx_size; i++) {
            char c = (char)chunk[i];
            if (c == '\n' || c == '\r') {
                if (g_line_len > 0) {
                    g_line_buf[g_line_len] = '\0';

                    /* static: this task (TinyUSB's own) never re-enters
                     * handle_rx() concurrently with itself, so a static buffer
                     * here is safe, same reasoning as tx_item_t above. */
                    static rx_item_t item;
                    item.itf = itf;
                    item.len = g_line_len;
                    memcpy(item.data, g_line_buf, g_line_len + 1); /* + NUL */
                    if (xQueueSend(g_rx_queue, &item, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "rx queue full, dropping a %u-byte command",
                                 (unsigned)item.len);
                    }
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
}

void serial_cdc_start(void) {
    g_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(tx_item_t));
    xTaskCreate(serial_tx_task, "serial_tx", 4096, NULL, 5, NULL);
    g_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_item_t));
    xTaskCreate(serial_rx_task, "serial_rx", 4096, NULL, 5, NULL);

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

#endif /* LNURLVAULT_BOARD_T_DISPLAY_S3 */
