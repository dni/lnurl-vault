/* Confirmed to compile against ESP-IDF 6.0.1 as part of a full firmware
 * build (see README.md's "Status" section) — this was the single most
 * version-fragile file in the project, and getting it there surfaced three
 * real bugs, not hypothetical ones: a comment-close token (asterisk then
 * slash) embedded mid-sentence that closed this very comment block early
 * and corrupted everything parsed after it; `.uuid` fields
 * assigned `BLE_UUID128_INIT(...)`'s brace-initializer value directly
 * instead of a `const ble_uuid_t *` pointer to a named `ble_uuid128_t`
 * variable's `.u` member; and a call to
 * `esp_nimble_hci_and_controller_init()`, which doesn't exist anywhere in
 * this framework version — `nimble_port_init()` alone brings up the
 * controller internally. All three were found and fixed by cross-checking
 * against ESP-IDF's own bundled `bleprph` example
 * (`examples/bluetooth/nimble/bleprph/main/`), not guessed. NimBLE glue
 * has still changed across ESP-IDF releases before, though, so on a
 * *different* installed IDF version, that same example is still the first
 * place to check if something here doesn't compile:
 *
 *   idf.py create-project-from-example espressif/esp-idf:bluetooth/nimble/bleprph
 *
 * Everything downstream of a full JSON message reaching dispatcher_handle()
 * below is the same tested logic the native tests cover.
 *
 * GATT layout: one custom service, two characteristics.
 *  - RX (write / write-no-response): command chunks in, from the browser.
 *  - TX (notify): response chunks out, to the browser.
 * Both wrap a complete JSON message in a trivial framing:
 *   [2-byte little-endian total length][chunk bytes...]
 * possibly split across several writes/notifications when the message
 * exceeds the negotiated MTU - 3. See docs/PROTOCOL.md for the full framing
 * spec and these UUIDs (also listed there for the browser side).
 *
 * WHAT RUNS WHERE, AND WHY — read before moving work between them.
 * The NimBLE host task is the only thing servicing ATT and GAP for this
 * connection, so anything it waits on, the whole link waits on. It used to
 * run dispatcher_handle() directly from the GATT write callback below, and
 * two of the commands there stop and ask the device's owner to press a
 * button — up to 30 seconds during which no ATT or GAP event is processed
 * at all. Link supervision drops the connection well inside that window, so
 * export_secret and ota_begin could not succeed over BLE however patiently
 * the owner answered. This is the same trap serial_cdc.c documents at
 * length for tud_task(), in different clothing.
 *
 * So the split is:
 *   host task  — reassembles writes, copies whole commands onto g_cmd_q,
 *                tracks connection and subscription state. Never sleeps.
 *   ble_cmd_task — dispatches (holding cmd_lock, then vault_lock — see
 *                cmd_lock.h), then sends the response. Free to sleep, and
 *                does: for the owner's answer, and for TX buffer space.
 * Nothing that can block belongs above that line. */
#include "ble_gatt.h"

#include <string.h>

#include "ble_frame.h"
#include "cmd_lock.h"
#include "dispatcher.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "vault_lock.h"

/* BLE_UUID128_INIT(...) expands to a brace initializer for a ble_uuid128_t
 * value (see the compiler error you get if you try to assign it directly
 * to a `.uuid` field, which wants a `const ble_uuid_t *` pointer, not a
 * struct value) — so each UUID needs to be its own named variable, then
 * referenced as `&name.u` (address of its embedded ble_uuid_t header)
 * everywhere a `.uuid` field is set below. */
static const ble_uuid128_t g_svc_uuid =
    BLE_UUID128_INIT(0x9c, 0x1a, 0x60, 0x4e, 0x53, 0x1b, 0x4b, 0xd6, 0x8e, 0x11, 0x3d, 0x2c, 0x1a,
                      0x0f, 0x7e, 0x40);
static const ble_uuid128_t g_chr_rx_uuid =
    BLE_UUID128_INIT(0x9c, 0x1a, 0x60, 0x4e, 0x53, 0x1b, 0x4b, 0xd6, 0x8e, 0x11, 0x3d, 0x2c, 0x1b,
                      0x0f, 0x7e, 0x40);
static const ble_uuid128_t g_chr_tx_uuid =
    BLE_UUID128_INIT(0x9c, 0x1a, 0x60, 0x4e, 0x53, 0x1b, 0x4b, 0xd6, 0x8e, 0x11, 0x3d, 0x2c, 0x1c,
                      0x0f, 0x7e, 0x40);

static const char *TAG = "ble_gatt";

/* Written by the NimBLE host task (the GAP event handler), read by
 * ble_cmd_task. Both are naturally-aligned scalars, so a torn read is not
 * possible on this architecture; volatile is what stops the compiler from
 * hoisting them out of the polling loops in send_response(). */
static volatile uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool g_tx_notify_enabled = false;
static uint16_t g_tx_val_handle;

/* Reassembly of the [2-byte LE length][payload] framing lives in
 * src/proto/ble_frame.c, not here, so it can be driven a byte at a time by
 * test/native/test_ble_frame.c. It used to be inline in rx_chr_access()
 * below, where four of its edge cases were wrong and none of them were
 * reachable from a test — see that test file. Touched only by the host
 * task. */
static ble_frame_t g_rx;

/* Complete commands, handed from the host task to ble_cmd_task. Depth 2
 * rather than 1 so a client that pipelines two commands does not have the
 * second refused outright, and no deeper because each slot is a whole
 * message: this queue is ~8KB of static RAM as it stands. */
#define CMD_QUEUE_DEPTH 2
typedef struct {
    size_t len;
    char data[BLE_FRAME_BUF_SIZE];
} ble_cmd_t;
static QueueHandle_t g_cmd_q;

/* Touched only by ble_cmd_task. */
#define TX_BUF_SIZE 4096
static uint8_t g_tx_buf[TX_BUF_SIZE]; /* [0..1]=LE length header, [2..]=JSON payload */
static size_t g_tx_len = 0;
static size_t g_tx_sent = 0;

/* How long to wait for the client to enable notifications before giving up
 * on a response. A client normally subscribes before sending anything, but
 * nothing in the protocol requires it, and the response to a command sent
 * first has nowhere to go until it does. */
#define TX_SUBSCRIBE_WAIT_MS 3000
/* How long to keep retrying a notification that the host stack will not
 * accept (its mbuf pool momentarily exhausted). Bounded for the same reason
 * serial_cdc.c's TX_GIVE_UP_US is: an unbounded retry turns a transient
 * stall into a permanently wedged task, and every response behind it. */
#define TX_GIVE_UP_MS 8000

/* Sends the whole framed response, in ATT-sized pieces.
 *
 * Runs only on ble_cmd_task, never on the NimBLE host task — it sleeps, and
 * the host task must never sleep. That is also what makes the retry below
 * possible at all: the mbufs it is waiting on are freed by the host task, so
 * retrying from inside a host callback could only ever spin against itself. */
static void send_response(void) {
    int64_t deadline_us = esp_timer_get_time() + (int64_t)TX_SUBSCRIBE_WAIT_MS * 1000;
    while (!g_tx_notify_enabled && g_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
           esp_timer_get_time() < deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!g_tx_notify_enabled || g_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "no subscriber for a %u-byte response; dropping it", (unsigned)g_tx_len);
        return;
    }

    /* One notification must fit in one ATT packet. ble_att_mtu() reports
     * what was actually negotiated for this connection; the 23-byte default
     * applies until it is. Three bytes go to the ATT opcode and handle. */
    uint16_t mtu = ble_att_mtu(g_conn_handle);
    size_t max_chunk = (mtu > 3 ? mtu : 23) - 3;

    int64_t give_up_at_us = esp_timer_get_time() + (int64_t)TX_GIVE_UP_MS * 1000;
    while (g_tx_sent < g_tx_len) {
        if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGW(TAG, "link dropped with %u bytes of a response unsent",
                     (unsigned)(g_tx_len - g_tx_sent));
            return;
        }
        size_t remaining = g_tx_len - g_tx_sent;
        size_t chunk_len = remaining < max_chunk ? remaining : max_chunk;

        struct os_mbuf *om = ble_hs_mbuf_from_flat(g_tx_buf + g_tx_sent, chunk_len);
        int rc = om ? ble_gatts_notify_custom(g_conn_handle, g_tx_val_handle, om)
                    : BLE_HS_ENOMEM; /* ble_gatts_notify_custom frees om either way */
        if (rc == 0) {
            g_tx_sent += chunk_len;
            continue;
        }
        if (esp_timer_get_time() > give_up_at_us) {
            ESP_LOGW(TAG, "notify stuck (rc=%d), giving up after %u/%u bytes", rc,
                     (unsigned)g_tx_sent, (unsigned)g_tx_len);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5)); /* let the host task free some mbufs */
    }
}

/* The task that exists so the NimBLE host task does not have to wait for
 * anything. See this file's header comment; in short, export_secret and
 * ota_begin both stop and ask the device's owner a question, and running
 * that wait on the host task meant no ATT or GAP event was serviced for up
 * to 30 seconds — long enough that link supervision dropped the connection
 * before the owner could answer, so those two commands could never succeed
 * over BLE at all. */
static void ble_cmd_task(void *arg) {
    (void)arg;
    /* static: a whole ble_cmd_t is larger than this task's stack. Safe
     * because exactly one item is in hand at a time. */
    static ble_cmd_t item;
    for (;;) {
        if (xQueueReceive(g_cmd_q, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* cmd_lock before vault_lock, always — see cmd_lock.h. The confirm
         * callbacks in main.c release vault_lock around the human wait and
         * reacquire it after; cmd_lock is deliberately held throughout. */
        cmd_lock_acquire();
        vault_lock_acquire();
        dispatcher_handle(item.data, (char *)g_tx_buf + 2, TX_BUF_SIZE - 2);
        vault_lock_release();
        cmd_lock_release();

        size_t resp_len = strlen((const char *)g_tx_buf + 2);
        g_tx_buf[0] = (uint8_t)(resp_len & 0xFF);
        g_tx_buf[1] = (uint8_t)((resp_len >> 8) & 0xFF);
        g_tx_len = resp_len + 2;
        g_tx_sent = 0;
        send_response();
    }
}

/* Runs on the NimBLE host task, so it only copies — see ble_cmd_task(). */
static void on_message(const char *msg, size_t len, void *ctx) {
    (void)ctx;
    if (len >= sizeof(((ble_cmd_t *)0)->data)) {
        return; /* unreachable: ble_frame caps a payload below this */
    }
    /* static: see ble_cmd_task()'s note. The host task does not re-enter
     * itself, so one shared staging buffer is safe here. */
    static ble_cmd_t item;
    item.len = len;
    memcpy(item.data, msg, len + 1); /* + NUL */
    if (xQueueSend(g_cmd_q, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, dropping a %u-byte command", (unsigned)len);
    }
}

static int rx_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    /* One ATT write cannot exceed the negotiated MTU minus 3, and NimBLE's
     * own ceiling on that (BLE_ATT_MTU_MAX) keeps it under this buffer — so
     * the EMSGSIZE branch below is defensive, not an expected path. If it
     * ever does fire, bytes have been lost, and reassembly must be abandoned
     * rather than resumed across the hole: splicing the two sides of a gap
     * together makes one command out of two halves that were never adjacent. */
    uint8_t chunk[512];
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, chunk, sizeof(chunk), &copied) != 0) {
        ESP_LOGW(TAG, "write larger than %u bytes; dropping and resyncing",
                 (unsigned)sizeof(chunk));
        ble_frame_reset(&g_rx);
        return 0;
    }

    ble_frame_feed(&g_rx, chunk, copied, on_message, NULL);
    return 0;
}

static int tx_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return 0; /* TX is notify-only; reads aren't expected but shouldn't fault */
}

static const struct ble_gatt_chr_def g_chrs[] = {
    {
        .uuid = &g_chr_rx_uuid.u,
        .access_cb = rx_chr_access,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid = &g_chr_tx_uuid.u,
        .access_cb = tx_chr_access,
        .val_handle = &g_tx_val_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    {0},
};

static const struct ble_gatt_svc_def g_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_svc_uuid.u,
        .characteristics = g_chrs,
    },
    {0},
};

/* Handles connect/disconnect/subscribe events. In particular, notifications
 * only start flowing once a BLE_GAP_EVENT_SUBSCRIBE for the TX
 * characteristic arrives (the browser side enabling notifications) — until
 * then g_tx_notify_enabled stays false and ble_cmd_task waits (briefly) for
 * it before sending a response. */
static int gap_event_handler(struct ble_gap_event *event, void *arg);

static void start_advertising(void) {
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)ble_svc_gap_device_name();
    fields.name_len = strlen(ble_svc_gap_device_name());
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, gap_event_handler,
                       NULL);
}

static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                g_conn_handle = event->connect.conn_handle;
            } else {
                start_advertising(); /* connection attempt failed; resume advertising */
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            g_tx_notify_enabled = false;
            /* A half-received message belongs to the connection that was
             * carrying it. Without this, the next client's first write lands
             * on the previous client's leftovers and the two are spliced
             * into one command. */
            ble_frame_reset(&g_rx);
            start_advertising();
            return 0;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == g_tx_val_handle) {
                g_tx_notify_enabled = event->subscribe.cur_notify;
                g_conn_handle = event->subscribe.conn_handle;
                /* Deliberately does not send: a response waiting on this
                 * subscribe is ble_cmd_task's to deliver, and it is already
                 * watching this flag (see send_response()). Sending from
                 * here would race it over the same TX buffer. */
            }
            return 0;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            start_advertising();
            return 0;
        default:
            return 0;
    }
}

static void on_sync(void) {
    ble_svc_gap_device_name_set("lnurl-vault");
    start_advertising();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "nimble host reset, reason=%d", reason);
    g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    g_tx_notify_enabled = false;
}

static void nimble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_gatt_start(void) {
    /* nimble_port_init() brings up the controller itself (it calls
     * esp_nimble_hci_init() internally — see nimble_port.c) — confirmed
     * against the real bleprph example bundled with this ESP-IDF version,
     * which does exactly this and nothing more here. An earlier version of
     * this function called a separate esp_nimble_hci_and_controller_init(),
     * which doesn't exist in this ESP-IDF version (nor, it turns out,
     * anywhere in this framework at all) and failed to compile. */
    ble_frame_init(&g_rx);

    g_cmd_q = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(ble_cmd_t));
    if (!g_cmd_q) {
        ESP_LOGE(TAG, "could not allocate the command queue");
        return;
    }
    /* 6KB: dispatcher_handle() materialises a note_meta_t (~408 bytes) plus
     * a JSON writer on this stack while streaming list_notes, and the OTA
     * path adds a ~1KB chunk decode buffer on top. */
    xTaskCreate(ble_cmd_task, "ble_cmd", 6144, NULL, 5, NULL);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_gatts_count_cfg(g_svcs);
    ble_gatts_add_svcs(g_svcs);

    nimble_port_freertos_init(nimble_host_task);
}
