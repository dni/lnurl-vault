/* NOTE: unverified by compilation — this is the single most version-fragile
 * file in the project (see README.md, "Status: unverified by compilation").
 * NimBLE glue (callback signatures, header paths, ble_gatts_*/nimble_port_*
 * helper names) has changed across ESP-IDF releases more than anything else
 * here. Treat this file as a strong starting skeleton to debug against your
 * installed IDF version's own NimBLE example, not as verified-working code:
 *
 *   idf.py create-project-from-example espressif/esp-idf:bluetooth/nimble/bleprph
 *
 * diff that example's app_main/GAP/GATT bring-up against this file's if
 * something doesn't compile. Everything downstream of a full JSON message
 * reaching dispatcher_handle() below is the same tested logic the native
 * tests cover — the risk here is entirely in the BLE plumbing above it.
 *
 * GATT layout: one custom service, two characteristics.
 *  - RX (write / write-no-response): command chunks in, from the browser.
 *  - TX (notify): response chunks out, to the browser.
 * Both wrap a complete JSON message in a trivial framing:
 *   [2-byte little-endian total length][chunk bytes...]
 * possibly split across several writes/notifications when the message
 * exceeds the negotiated MTU - 3. See docs/PROTOCOL.md for the full framing
 * spec and these UUIDs (also listed there for the browser side). */
#include "ble_gatt.h"

#include <string.h>

#include "dispatcher.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "vault_lock.h"

#define LNURLVAULT_SVC_UUID128                                                                       \
    BLE_UUID128_INIT(0x9c, 0x1a, 0x60, 0x4e, 0x53, 0x1b, 0x4b, 0xd6, 0x8e, 0x11, 0x3d, 0x2c, 0x1a, \
                      0x0f, 0x7e, 0x40)
#define LNURLVAULT_CHR_RX_UUID128                                                                     \
    BLE_UUID128_INIT(0x9c, 0x1a, 0x60, 0x4e, 0x53, 0x1b, 0x4b, 0xd6, 0x8e, 0x11, 0x3d, 0x2c, 0x1b, \
                      0x0f, 0x7e, 0x40)
#define LNURLVAULT_CHR_TX_UUID128                                                                     \
    BLE_UUID128_INIT(0x9c, 0x1a, 0x60, 0x4e, 0x53, 0x1b, 0x4b, 0xd6, 0x8e, 0x11, 0x3d, 0x2c, 0x1c, \
                      0x0f, 0x7e, 0x40)

static const char *TAG = "ble_gatt";

static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_tx_val_handle;
static bool g_tx_notify_enabled = false;

#define RX_BUF_SIZE 4096
static uint8_t g_rx_buf[RX_BUF_SIZE];
static size_t g_rx_have = 0;
static size_t g_rx_want = 0;

#define TX_BUF_SIZE 4096
static uint8_t g_tx_buf[TX_BUF_SIZE]; /* [0..1]=LE length header, [2..]=JSON payload */
static size_t g_tx_len = 0;
static size_t g_tx_sent = 0;

static void send_next_tx_chunk(void) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || g_tx_sent >= g_tx_len) {
        return;
    }
    size_t remaining = g_tx_len - g_tx_sent;
    size_t chunk_len = remaining < 180 ? remaining : 180; /* conservative, pre-MTU-negotiation size */
    struct os_mbuf *om = ble_hs_mbuf_from_flat(g_tx_buf + g_tx_sent, chunk_len);
    if (!om) {
        return;
    }
    int rc = ble_gatts_notify_custom(g_conn_handle, g_tx_val_handle, om);
    if (rc == 0) {
        g_tx_sent += chunk_len;
        if (g_tx_sent < g_tx_len) {
            send_next_tx_chunk();
        }
    } else {
        ESP_LOGW(TAG, "notify failed: %d", rc);
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

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t chunk[512];
    if (len > sizeof(chunk)) {
        g_rx_have = 0; /* drop an oversized single write, resync on next */
        return 0;
    }
    ble_hs_mbuf_to_flat(ctxt->om, chunk, len, NULL);

    size_t offset = 0;
    if (g_rx_have == 0) {
        if (len < 2) {
            return 0;
        }
        g_rx_want = (size_t)chunk[0] | ((size_t)chunk[1] << 8);
        if (g_rx_want > RX_BUF_SIZE - 1) {
            g_rx_want = 0;
            return 0; /* refuse an oversized message rather than overflow */
        }
        offset = 2;
    }

    size_t take = len - offset;
    if (g_rx_have + take > RX_BUF_SIZE - 1) {
        take = RX_BUF_SIZE - 1 - g_rx_have;
    }
    memcpy(g_rx_buf + g_rx_have, chunk + offset, take);
    g_rx_have += take;

    if (g_rx_want > 0 && g_rx_have >= g_rx_want) {
        g_rx_buf[g_rx_have < RX_BUF_SIZE ? g_rx_have : RX_BUF_SIZE - 1] = '\0';

        vault_lock_acquire();
        dispatcher_handle((const char *)g_rx_buf, (char *)g_tx_buf + 2, TX_BUF_SIZE - 2);
        vault_lock_release();
        size_t resp_len = strlen((const char *)g_tx_buf + 2);
        g_tx_buf[0] = (uint8_t)(resp_len & 0xFF);
        g_tx_buf[1] = (uint8_t)((resp_len >> 8) & 0xFF);
        g_tx_len = resp_len + 2;
        g_tx_sent = 0;

        g_rx_have = 0;
        g_rx_want = 0;

        if (g_tx_notify_enabled) {
            send_next_tx_chunk();
        }
    }
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
        .uuid = LNURLVAULT_CHR_RX_UUID128,
        .access_cb = rx_chr_access,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid = LNURLVAULT_CHR_TX_UUID128,
        .access_cb = tx_chr_access,
        .val_handle = &g_tx_val_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    {0},
};

static const struct ble_gatt_svc_def g_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = LNURLVAULT_SVC_UUID128,
        .characteristics = g_chrs,
    },
    {0},
};

/* Handles connect/disconnect/subscribe events. In particular, notifications
 * only start flowing once a BLE_GAP_EVENT_SUBSCRIBE for the TX
 * characteristic arrives (the browser side enabling notifications) — until
 * then g_tx_notify_enabled stays false and rx_chr_access() above buffers
 * (but doesn't send) any response. */
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
            start_advertising();
            return 0;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == g_tx_val_handle) {
                g_tx_notify_enabled = event->subscribe.cur_notify;
                g_conn_handle = event->subscribe.conn_handle;
                send_next_tx_chunk(); /* flush any response buffered before this subscribe */
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
    esp_err_t err = esp_nimble_hci_and_controller_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller init failed: %s", esp_err_to_name(err));
        return;
    }

    nimble_port_init();

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_gatts_count_cfg(g_svcs);
    ble_gatts_add_svcs(g_svcs);

    nimble_port_freertos_init(nimble_host_task);
}
