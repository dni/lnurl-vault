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
 * 5. A bounded write that gives up after sending a response prefix has
 *    damaged the newline framing as well as lost one reply. Sending the next
 *    JSON immediately glues it to that prefix, so the host reports an
 *    "unparseable" line and loses a second reply that was otherwise intact.
 *    line_tx.c now remembers a partial abandonment and sends a standalone
 *    newline before the next response. If even that delimiter cannot be
 *    queued, the new response is dropped whole rather than corrupting the
 *    stream further.
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
#include "line_proto.h"
#include "line_tx.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "vault_lock.h"

static const char *TAG = "serial_cdc";

#define LINE_BUF_SIZE 2048
#define RESP_BUF_SIZE 4096
#define TX_QUEUE_DEPTH 4
#define RX_QUEUE_DEPTH 4

static char g_resp_buf[RESP_BUF_SIZE];
static line_proto_t g_line_rx;
static line_tx_t g_line_tx;

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

/* What this link has thrown away, reported by get_info -- see
 * dispatcher.h's transport_drops_fn.
 *
 * No lock, and none needed: each stored counter has exactly one writer task
 * (rx from tud_task, queued tx drops from serial_rx_task, and write-time tx /
 * tx_stalled drops from serial_tx_task), and a 32-bit aligned load cannot tear
 * on this core. A reader can therefore be a moment out of date but never
 * wrong, which is the whole of what a diagnostic counter owes anybody. */
static transport_drops_t g_drops;
static uint32_t g_tx_before_byte_drops;

void serial_cdc_drops(transport_drops_t *out) {
    *out = g_drops;
    out->tx += g_tx_before_byte_drops;
}

/* What the bus itself has done, reported by get_info as `usb` -- see
 * dispatcher.h's usb_link_fn. These are TinyUSB's weak default callbacks,
 * overridden here; TinyUSB invokes every one of them from its own task, so
 * each counter has one writer and the same no-lock reasoning as g_drops
 * holds. esp_tinyusb defines the mount pair itself only when its MSC class
 * is compiled in, which this build never enables -- and the linker would say
 * so if that changed, rather than one silently winning. */
static usb_link_t g_usb;

void serial_cdc_usb_link(usb_link_t *out) {
    *out = g_usb;
}

/* SET_CONFIGURATION with a non-zero value: the host has finished enumerating
 * this device. Once per boot in a healthy session; more than that means the
 * link was torn down and rebuilt at USB level. */
void tud_mount_cb(void) {
    g_usb.configured++;
}

void tud_umount_cb(void) {
    g_usb.unconfigured++;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    g_usb.suspends++;
}

void tud_resume_cb(void) {
    g_usb.resumes++;
}

/* One completed IN transfer on the CDC data endpoint. In slave mode that is
 * however much of the TX ring was pending when the transfer was armed, up to
 * the ring's 512 bytes; it was one 64-byte packet in the DMA mode this build
 * has moved away from (see sdkconfig.defaults.esp32s3). */
void tud_cdc_tx_complete_cb(uint8_t itf) {
    (void)itf;
    g_usb.tx_xfers++;
}

/* The host opening and closing the port, as the bus shows it: DTR up and
 * down. Counted for get_info's `usb`, and acted on.
 *
 * A host that closes the port will never read what is queued for it. Left
 * in TinyUSB's TX ring, those bytes come out as the first thing the *next*
 * open receives: a stale, torn tail glued in front of the answer to that
 * client's first command, which is exactly the shape of one field sample
 * (a get_info reply missing its first 256 bytes). So on close the ring is
 * cleared, and any reply still being written is abandoned: g_link_generation
 * is what serial_write() compares against. On open the ring is cleared
 * again, for anything queued in between while nobody was listening.
 * MicroPython's CDC driver clears its TX FIFO on DTR low for the same
 * reason (micropython/micropython 8d3597ca5).
 *
 * Runs on the TinyUSB task. tud_cdc_n_write_clear() takes the ring's write
 * mutex, the same one serial_tx_task's writes take, so the two cannot
 * interleave inside the ring. Edge-detected: hosts raise DTR and RTS in
 * separate requests, each of which fires this callback. */
static volatile uint32_t g_link_generation;
static bool g_dtr;

static void handle_line_state(int itf, cdcacm_event_t *event) {
    bool dtr = event->line_state_changed_data.dtr;
    if (dtr == g_dtr) {
        return;
    }
    g_dtr = dtr;
    if (dtr) {
        g_usb.port_opens++;
    } else {
        g_usb.port_closes++;
    }
    /* Both edges, not just the closing one. This clears the TX ring either
     * way, so a reply already part-written into it loses its queued head --
     * and if the generation does not move, host_still_there() stays true and
     * serial_write() goes on appending the tail. The client then receives a
     * reply with an arbitrary run deleted from the middle and the rest
     * perfectly formed, which is unparseable but looks nothing like packet
     * loss. A bench capture showed exactly that on a DTR *rise*
     * (`port_opens` 1): 18 bytes gone from a 469-byte get_info, taking
     * `"serial","ble"],"r` out of the middle of the capabilities object. 18
     * is not a multiple of 64, which is what ruled out the lost-USB-packet
     * explanation this file previously assumed.
     *
     * Bumping on both edges abandons that reply cleanly instead. A reply that
     * never arrives is a timeout the client can see and retry -- and with a
     * `tag` (see dispatcher.c) it can retry safely; a reply with a hole in it
     * is silent corruption that reaches the client as garbage. */
    g_link_generation++;
    tud_cdc_n_write_clear((uint8_t)itf);
}

/* Generous on purpose: test_serial.py's own docstring documents this
 * device having a real, unexplained ~2s+ baseline response latency even
 * when nothing is actually stuck, so a short cap here gives up on
 * transfers that would otherwise have gone through. This is meant to catch
 * only genuine, permanent stalls (write_queue() never freeing space again),
 * not to police normal-but-slow delivery — see this file's header comment
 * for why an unbounded wait isn't safe either. */
#define TX_GIVE_UP_US (8 * 1000 * 1000)

typedef struct {
    int itf;
    int64_t give_up_at_us;
    uint32_t generation; /* g_link_generation when this response began */
} serial_write_ctx_t;

/* False once the host has closed the port under this response -- see
 * handle_line_state(). Queueing the rest would only stock the ring with a
 * stale tail for the next client to trip over. */
static bool host_still_there(const serial_write_ctx_t *serial) {
    return g_link_generation == serial->generation;
}

static size_t serial_write(void *ctx, const uint8_t *data, size_t len) {
    serial_write_ctx_t *serial = (serial_write_ctx_t *)ctx;
    if (!host_still_there(serial)) {
        return 0;
    }
    return tinyusb_cdcacm_write_queue(serial->itf, data, len);
}

static void serial_flush(void *ctx) {
    serial_write_ctx_t *serial = (serial_write_ctx_t *)ctx;
    /* Non-blocking: just kicks transmission of what's staged so far. A
     * blocking flush from TinyUSB's own callback deadlocks, and doing one on
     * every chunk from this task was also measured to make latency worse. */
    tinyusb_cdcacm_write_flush(serial->itf, 0);
}

static bool serial_wait_for_space(void *ctx) {
    serial_write_ctx_t *serial = (serial_write_ctx_t *)ctx;
    if (!host_still_there(serial) || esp_timer_get_time() > serial->give_up_at_us) {
        return false;
    }
    /* No room queued this pass; give tud_task() a moment to drain its FIFO
     * before retrying. */
    vTaskDelay(pdMS_TO_TICKS(2));
    return true;
}

/* Runs on its own task, never nested inside TinyUSB's rx callback — see
 * this file's header comment for why that distinction makes the bounded
 * wait/retry loop below safe when it wasn't in handle_rx(). */
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
        serial_write_ctx_t serial = {
            .itf = item.itf,
            .give_up_at_us = esp_timer_get_time() + TX_GIVE_UP_US,
            .generation = g_link_generation,
        };
        line_tx_result_t result =
            line_tx_send(&g_line_tx, item.data, item.len, serial_write,
                         serial_flush, serial_wait_for_space, &serial);
        if (result != LINE_TX_OK && !host_still_there(&serial)) {
            /* Not a drop the link is charged with: the host closed the port
             * under this reply, and it is counted where that belongs, in
             * `usb.port_closes`. line_tx has armed its framing repair if
             * any of it had already left, which is all the next client
             * needs. */
            ESP_LOGW(TAG, "host closed the port under a %u-byte response; abandoned",
                     (unsigned)item.len);
        } else if (result == LINE_TX_DROPPED_PARTIAL) {
            g_drops.tx_stalled++;
            ESP_LOGW(TAG, "tx stalled after a partial %u-byte response; framing repair armed",
                     (unsigned)item.len);
        } else if (result == LINE_TX_DROPPED) {
            g_tx_before_byte_drops++;
            ESP_LOGW(TAG, "tx stalled before a %u-byte response; dropping it whole",
                     (unsigned)item.len);
        }
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

        static tx_item_t tx;
        tx.itf = item.itf;
        tx.len = resp_len;
        memcpy(tx.data, g_resp_buf, resp_len);
        if (xQueueSend(g_tx_queue, &tx, 0) != pdTRUE) {
            g_drops.tx++;
            ESP_LOGW(TAG, "tx queue full, dropping a %u-byte response", (unsigned)tx.len);
        }
    }
}

static void queue_line(const char *line, void *ctx) {
    int itf = (int)(intptr_t)ctx;
    static rx_item_t item;
    item.itf = itf;
    item.len = strlen(line);
    memcpy(item.data, line, item.len + 1); /* + NUL */
    if (xQueueSend(g_rx_queue, &item, 0) != pdTRUE) {
        g_drops.rx++;
        ESP_LOGW(TAG, "rx queue full, dropping a %u-byte command",
                 (unsigned)item.len);
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
        line_proto_feed(&g_line_rx, chunk, rx_size, queue_line, (void *)(intptr_t)itf);
    }
}

void serial_cdc_start(void) {
    g_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(tx_item_t));
    xTaskCreate(serial_tx_task, "serial_tx", 4096, NULL, 5, NULL);
    g_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_item_t));
    xTaskCreate(serial_rx_task, "serial_rx", 4096, NULL, 5, NULL);
    line_proto_init(&g_line_rx);
    line_tx_init(&g_line_tx);

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
        .callback_line_state_changed = &handle_line_state,
        .callback_line_coding_changed = NULL,
    };
    err = tusb_cdc_acm_init(&acm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tusb_cdc_acm_init failed: %s", esp_err_to_name(err));
    }
}

#endif /* LNURLVAULT_BOARD_T_DISPLAY_S3 */
