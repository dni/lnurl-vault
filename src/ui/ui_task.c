/* Confirmed to compile against ESP-IDF 6.0.1 as part of a full firmware
 * build (see README.md's "Status" section). Builds on buttons.c/display.c
 * and qr_display.c (which additionally depends on a vendored third-party
 * library — see its own header comment); the actual gesture logic driving
 * all of this lives in src/proto, unit-tested independent of the ESP32
 * build: button_fsm.c for browsing gestures (tap vs. chord, debounce) and
 * approval.c for the hold-to-approve gate in front of every disclosure —
 * see test/native/test_button_fsm.c and test/native/test_approval.c. vault.c access from this task is
 * serialized against the transport task via vault_lock.h — see that
 * header's comment for why. None of this has been checked against real
 * button/display hardware. */
#include "ui_task.h"

#include <stddef.h>
#include <stdio.h>

#include "approval.h"
#include "note_display.h"
#include "buttons.h"
#include "button_fsm.h"
#include "display.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "note_url.h"
#include "qr_display.h"
#include "vault.h"
#include "vault_lock.h"

static const char *TAG = "ui_task";

/* The strings the approval screen shows, formatted by the requester and
 * copied into the queue rather than passed by pointer: the note_meta_t behind
 * them belongs to the calling transport's stack, and this request outlives
 * that call by up to the whole confirm window. */
typedef struct {
    uint32_t timeout_ms;
    QueueHandle_t response_q;
    bool has_detail;
    char amount[NOTE_AMOUNT_BUF];
    char unit[8];
    char label[24];
    char id[VAULT_ID_BUF + 8];
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

/* Shows the selected note while browsing, so unveiling the wrong one takes a
 * deliberate misreading rather than a miscount. This replaced a blinked-out
 * position count, which told you where you were in the list but not which
 * note or for how much -- and the chord that follows discloses a bearer
 * secret on screen. Issue #9.
 *
 * `position` is the 1-based place among CONFIRMED notes, shown next to the id
 * so the count the old flash conveyed is not lost. */
static void show_browse_note(int browse_index, int position) {
    if (browse_index < 0) {
        display_set_state(DISPLAY_STATE_BROWSE);
        return;
    }
    note_meta_t meta;
    bool got = false;
    vault_lock_acquire();
    got = vault_get_meta_at((size_t)browse_index, &meta);
    vault_lock_release();
    if (!got) {
        display_set_state(DISPLAY_STATE_BROWSE);
        return;
    }

    char amount[NOTE_AMOUNT_BUF];
    char unit[8];
    char label[24];
    char id[VAULT_ID_BUF + 16];
    note_format_amount_parts(meta.amount_msat, amount, sizeof(amount), unit, sizeof(unit));
    note_format_label(meta.label, label, sizeof(label));
    snprintf(id, sizeof(id), "%s  %d", meta.id, position);
    display_note_detail(DISPLAY_STATE_BROWSE, amount, unit, label, id);
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

/* Why the watchdog watches THIS task and not the transports.
 *
 * ui_task is the one task that must always be able to make progress: it is
 * on the far side of every confirmation wait, and the deadlock in issue #4
 * showed up precisely as ui_task blocked on a lock it could not get, with a
 * transport waiting on a queue only ui_task could drain. Nothing detected
 * it; the vault simply stopped answering, with secrets still in RAM.
 *
 * The transport tasks are deliberately NOT subscribed. Their normal resting
 * state is blocked on a queue with no timeout, which is health, not a wedge
 * -- and they may legitimately be stuck for a long time on purpose: a
 * 30-second approval wait, plus up to another 30 waiting on cmd_lock behind
 * the other transport's approval. That is 60s of correct behaviour, which
 * is the timeout itself. A watchdog that panics a device holding secrets
 * because someone took their time reading the screen would be worse than
 * the fault it is looking for.
 *
 * So: 60s against a 30s approval, on the task whose loop is 30ms and which
 * has no legitimate reason to ever pause -- borrowed from heartwood-esp32's
 * ratio, applied to the task that ratio actually holds for. */
static void wdt_feed(void) {
    esp_task_wdt_reset();
}

/* The gate in front of every disclosure: a two-second hold on button 1, with
 * the hold drawn on screen as it fills. It used to be a single tap, which is
 * one pocket-brush away from handing out a bearer secret and gave no sign it
 * had registered.
 *
 * The decision logic is src/proto/approval.c -- portable, and unit-tested a
 * tick at a time, because that is where the bounce and edge-case bugs live
 * and they are near-impossible to find on hardware. This function is only
 * the loop that feeds it levels and paints the result. */
static confirm_result_t service_remote_confirm(const remote_confirm_request_t *req) {
    const uint32_t timeout_ms = req->timeout_ms;
    if (req->has_detail) {
        display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, req->amount, req->unit, req->label,
                            req->id);
    } else {
        /* No detail to show (an OTA image, or a wipe) -- the flat state colour
         * is all there is, same as before. */
        display_set_state(DISPLAY_STATE_CONFIRM_PENDING);
    }
    display_progress(0);

    approval_t ap;
    int64_t now = esp_timer_get_time();
    approval_begin(&ap, now, timeout_ms);

    approval_state_t state = APPROVAL_PENDING;
    uint16_t drawn = 0;
    while (state == APPROVAL_PENDING) {
        now = esp_timer_get_time();
        state = approval_poll(&ap, buttons_raw_1(), buttons_raw_2(), now);

        /* Repaint only on a visible step. The bar is a real DMA blit and this
         * loop runs 50 times a second; 40 steps is smooth to the eye and a
         * fortieth of the traffic. */
        uint16_t p = approval_progress_permille(&ap, now);
        if (p / 25 != drawn / 25) {
            display_progress(p);
            drawn = p;
        }

        wdt_feed(); /* a 30s hold is patience, not a wedge */
        if (state == APPROVAL_PENDING) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* Whichever button answered is still physically down. Without this, its
     * release arrives in button_fsm as a fresh tap and the screen after this
     * one acts on a press the owner made for this one -- which, on a device
     * where a tap starts browsing secrets, means an approval also scrolls to
     * one. */
    buttons_consume_press();

    confirm_result_t result;
    display_state_t card;
    switch (state) {
        case APPROVAL_GRANTED:
            result = CONFIRM_YES;
            card = DISPLAY_STATE_APPROVED;
            break;
        case APPROVAL_DENIED:
            result = CONFIRM_NO;
            card = DISPLAY_STATE_DECLINED;
            break;
        default:
            /* Its own card, deliberately: a prompt nobody answered must not
             * be left looking live, and must not be dressed as the owner
             * having said no. */
            result = CONFIRM_TIMEOUT;
            card = DISPLAY_STATE_EXPIRED;
            break;
    }
    display_set_state(card);
    vTaskDelay(pdMS_TO_TICKS(800));
    return result;
}

static void ui_task_fn(void *arg) {
    (void)arg;
    /* Not fatal if it fails -- an unwatched vault still works, and refusing
     * to browse notes because a watchdog would not subscribe would be a
     * worse trade than going unwatched. */
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "task watchdog not watching ui_task: %s", esp_err_to_name(wdt_err));
    }

    ui_mode_t mode = UI_IDLE;
    int browse_index = -1;
    int64_t browse_last_activity_us = 0;

    display_set_state(DISPLAY_STATE_IDLE);

    for (;;) {
        /* A pending remote confirm request always takes priority over
         * local browsing, and owns buttons/display exclusively until it
         * resolves. Reaching this point at all depends on no transport
         * holding vault_lock while it waits: this task takes vault_lock
         * itself (find_confirmed(), confirmed_position(), unveil()), so a
         * caller that held it across the wait would block this task before
         * it ever got here, and wait forever for a queue nobody can drain.
         * Both confirm paths in main.c release it first — see
         * vault_lock.h and cmd_lock.h. */
        remote_confirm_request_t req;
        if (xQueueReceive(g_request_q, &req, 0) == pdTRUE) {
            confirm_result_t result = service_remote_confirm(&req);
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
                        show_browse_note(browse_index, confirmed_position(browse_index));
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
                        show_browse_note(browse_index, confirmed_position(browse_index));
                    }
                    browse_last_activity_us = esp_timer_get_time();
                } else if (ev == BTN_EVENT_1_TAP) {
                    browse_index = find_confirmed(browse_index, 1);
                    browse_last_activity_us = esp_timer_get_time();
                    show_browse_note(browse_index, confirmed_position(browse_index));
                } else if (ev == BTN_EVENT_2_TAP) {
                    browse_index = find_confirmed(browse_index, -1);
                    browse_last_activity_us = esp_timer_get_time();
                    show_browse_note(browse_index, confirmed_position(browse_index));
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
                    show_browse_note(browse_index, confirmed_position(browse_index));
                }
                break;
        }

        wdt_feed();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void ui_task_start(void) {
    g_request_q = xQueueCreate(4, sizeof(remote_confirm_request_t));
    xTaskCreate(ui_task_fn, "ui_task", 4096, NULL, 5, NULL);
}

static confirm_result_t request_confirm_detailed(uint32_t timeout_ms, const note_meta_t *note) {
    QueueHandle_t resp_q = xQueueCreate(1, sizeof(confirm_result_t));
    remote_confirm_request_t req = {.timeout_ms = timeout_ms, .response_q = resp_q};
    if (note) {
        req.has_detail = true;
        note_format_amount_parts(note->amount_msat, req.amount, sizeof(req.amount), req.unit,
                                  sizeof(req.unit));
        note_format_label(note->label, req.label, sizeof(req.label));
        snprintf(req.id, sizeof(req.id), "id %s", note->id);
    }
    xQueueSend(g_request_q, &req, portMAX_DELAY);
    confirm_result_t result = CONFIRM_TIMEOUT;
    xQueueReceive(resp_q, &result, portMAX_DELAY);
    vQueueDelete(resp_q);
    return result;
}

confirm_result_t ui_task_request_remote_confirm(const note_meta_t *note, uint32_t timeout_ms) {
    /* The note IS shown now -- issue #9. Approving a disclosure without being
     * told which note or for how much made the physical gate a formality. */
    return request_confirm_detailed(timeout_ms, note);
}

confirm_result_t ui_task_request_ota_confirm(uint32_t timeout_ms) {
    return request_confirm_detailed(timeout_ms, NULL);
}

confirm_result_t ui_task_request_wipe_confirm(uint32_t timeout_ms) {
    return request_confirm(timeout_ms);
}
