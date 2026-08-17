/* Confirmed to compile against ESP-IDF 6.0.1 as part of a full firmware
 * build (see README.md's "Status" section). Builds on buttons.c/display.c
 * and qr_display.c (which additionally depends on a vendored third-party
 * library — see its own header comment); the actual gesture logic driving
 * all of this (tap vs. chord, debounce) is src/proto/button_fsm.c, which
 * is unit-tested independent of the ESP32 build — see
 * test/native/test_button_fsm.c. vault.c access from this task is
 * serialized against the transport task via vault_lock.h — see that
 * header's comment for why. None of this has been checked against real
 * button/display hardware. */
#include "ui_task.h"

#include <stddef.h>

#include "buttons.h"
#include "button_fsm.h"
#include "display.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "note_url.h"
#include "qr_display.h"
#include "vault.h"
#include "vault_lock.h"

typedef struct {
    uint32_t timeout_ms;
    QueueHandle_t response_q;
} remote_confirm_request_t;

static QueueHandle_t g_request_q;

#define BROWSE_IDLE_TIMEOUT_MS 15000

typedef enum {
    UI_IDLE,
    UI_BROWSE,
    UI_QR_SHOWN,
} ui_mode_t;

/* Finds the vault index (0-based, into the full note list) of the next/
 * previous CONFIRMED note relative to `from` (pass -1 to start from the
 * beginning), wrapping around. Returns -1 if there are no CONFIRMED notes.
 * Only CONFIRMED notes are browsable — a PENDING note has no settled value
 * yet, and export_secret would reject it anyway. */
static int find_confirmed(int from, int step) {
    vault_lock_acquire();
    size_t total = vault_count();
    int result = -1;
    if (total > 0) {
        int idx = from;
        for (size_t tries = 0; tries < total; tries++) {
            idx += step;
            if (idx < 0) {
                idx = (int)total - 1;
            } else if (idx >= (int)total) {
                idx = 0;
            }
            note_meta_t meta;
            if (vault_get_meta_at((size_t)idx, &meta) && meta.state == NOTE_STATE_CONFIRMED) {
                result = idx;
                break;
            }
        }
    }
    vault_lock_release();
    return result;
}

/* 1-based position of `idx` among CONFIRMED notes only, purely for
 * display_flash_count()'s human-facing counter — not a vault id. */
static int confirmed_position(int idx) {
    if (idx < 0) {
        return 0;
    }
    vault_lock_acquire();
    int pos = 0;
    for (int i = 0; i <= idx; i++) {
        note_meta_t meta;
        if (vault_get_meta_at((size_t)i, &meta) && meta.state == NOTE_STATE_CONFIRMED) {
            pos++;
        }
    }
    vault_lock_release();
    return pos;
}

static void wipe(char *buf, size_t len) {
    volatile char *p = (volatile char *)buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = 0;
    }
}

/* Exports the selected note's secret (re-validating it's still CONFIRMED
 * under the lock — browse_index is a snapshot that a concurrent remote
 * discard/delete could have invalidated since it was last set), builds its
 * lnurlw:// URL, and shows it as a QR code. The secret and the URL (which
 * embeds it) are wiped from their stack buffers as soon as each is no
 * longer needed. */
static bool unveil(int browse_index) {
    if (browse_index < 0) {
        return false;
    }

    note_meta_t meta;
    char k1[VAULT_SECRET_HEX_BUF];
    bool got_secret = false;

    vault_lock_acquire();
    if (vault_get_meta_at((size_t)browse_index, &meta) && meta.state == NOTE_STATE_CONFIRMED) {
        got_secret = vault_export_secret(meta.id, k1) == VAULT_OK;
    }
    vault_lock_release();

    if (!got_secret) {
        return false;
    }

    char url[256];
    bool built = note_url_build(meta.host, k1, meta.amount_msat, url, sizeof(url));
    wipe(k1, sizeof(k1));
    if (!built) {
        return false;
    }

    bool shown = qr_display_show(url);
    wipe(url, sizeof(url));
    return shown;
}

static confirm_result_t service_remote_confirm(uint32_t timeout_ms) {
    display_set_state(DISPLAY_STATE_CONFIRM_PENDING);
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    confirm_result_t result = CONFIRM_TIMEOUT;

    while (esp_timer_get_time() < deadline_us) {
        button_event_t ev = buttons_poll();
        if (ev == BTN_EVENT_1_TAP) {
            result = CONFIRM_YES;
            break;
        }
        if (ev == BTN_EVENT_2_TAP) {
            result = CONFIRM_NO;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    display_set_state(result == CONFIRM_YES ? DISPLAY_STATE_APPROVED : DISPLAY_STATE_DECLINED);
    vTaskDelay(pdMS_TO_TICKS(800));
    return result;
}

static void ui_task_fn(void *arg) {
    (void)arg;
    ui_mode_t mode = UI_IDLE;
    int browse_index = -1;
    int64_t browse_last_activity_us = 0;

    display_set_state(DISPLAY_STATE_IDLE);

    for (;;) {
        /* A pending remote confirm request always takes priority over
         * local browsing, and owns buttons/display exclusively until it
         * resolves. This wait itself never holds vault_lock for
         * export_secret (main.c's confirm_export_on_device() releases it
         * first — see vault_lock.h's header comment); ota_begin's confirm
         * still does, a known, separately-tracked gap (see
         * ota_approve_on_device()'s comment in main.c). */
        remote_confirm_request_t req;
        if (xQueueReceive(g_request_q, &req, 0) == pdTRUE) {
            confirm_result_t result = service_remote_confirm(req.timeout_ms);
            xQueueSend(req.response_q, &result, portMAX_DELAY);
            mode = UI_IDLE;
            browse_index = -1;
            display_set_state(DISPLAY_STATE_IDLE);
            continue;
        }

        button_event_t ev = buttons_poll();

        switch (mode) {
            case UI_IDLE:
                if (ev == BTN_EVENT_1_TAP || ev == BTN_EVENT_2_TAP) {
                    browse_index = find_confirmed(-1, 1);
                    if (browse_index >= 0) {
                        mode = UI_BROWSE;
                        browse_last_activity_us = esp_timer_get_time();
                        display_set_state(DISPLAY_STATE_BROWSE);
                        display_flash_count(confirmed_position(browse_index));
                    }
                }
                break;

            case UI_BROWSE:
                if (ev == BTN_EVENT_BOTH_CHORD) {
                    if (unveil(browse_index)) {
                        mode = UI_QR_SHOWN;
                    } else {
                        display_set_state(DISPLAY_STATE_DECLINED);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        display_set_state(DISPLAY_STATE_BROWSE);
                    }
                    browse_last_activity_us = esp_timer_get_time();
                } else if (ev == BTN_EVENT_1_TAP) {
                    browse_index = find_confirmed(browse_index, 1);
                    browse_last_activity_us = esp_timer_get_time();
                    display_flash_count(confirmed_position(browse_index));
                } else if (ev == BTN_EVENT_2_TAP) {
                    browse_index = find_confirmed(browse_index, -1);
                    browse_last_activity_us = esp_timer_get_time();
                    display_flash_count(confirmed_position(browse_index));
                } else if ((esp_timer_get_time() - browse_last_activity_us) >
                           (int64_t)BROWSE_IDLE_TIMEOUT_MS * 1000) {
                    mode = UI_IDLE;
                    display_set_state(DISPLAY_STATE_IDLE);
                }
                break;

            case UI_QR_SHOWN:
                if (ev == BTN_EVENT_1_TAP || ev == BTN_EVENT_2_TAP || ev == BTN_EVENT_BOTH_CHORD) {
                    mode = UI_BROWSE;
                    browse_last_activity_us = esp_timer_get_time();
                    display_set_state(DISPLAY_STATE_BROWSE);
                }
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void ui_task_start(void) {
    g_request_q = xQueueCreate(4, sizeof(remote_confirm_request_t));
    xTaskCreate(ui_task_fn, "ui_task", 4096, NULL, 5, NULL);
}

static confirm_result_t request_confirm(uint32_t timeout_ms) {
    QueueHandle_t resp_q = xQueueCreate(1, sizeof(confirm_result_t));
    remote_confirm_request_t req = {.timeout_ms = timeout_ms, .response_q = resp_q};
    xQueueSend(g_request_q, &req, portMAX_DELAY);
    confirm_result_t result = CONFIRM_TIMEOUT;
    xQueueReceive(resp_q, &result, portMAX_DELAY);
    vQueueDelete(resp_q);
    return result;
}

confirm_result_t ui_task_request_remote_confirm(const note_meta_t *note, uint32_t timeout_ms) {
    (void)note; /* not shown on-screen yet — see display.c's header comment */
    return request_confirm(timeout_ms);
}

confirm_result_t ui_task_request_ota_confirm(uint32_t timeout_ms) {
    return request_confirm(timeout_ms);
}
