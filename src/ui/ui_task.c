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
/* Declares this translation unit as ui_task for cmd_lock.h's guard: that
 * header refuses to be included here at all, because "ui_task must never
 * acquire cmd_lock" is the invariant standing between this firmware and a
 * deadlock that needs a power cycle. Anyone calling cmd_lock_acquire() from
 * this file has to include the header to do it, and that is the moment this
 * catches them -- at build time, rather than in the field with a paired host
 * waiting on a confirm nobody can give. */
#define LNURLVAULT_TU_IS_UI_TASK 1

#include "ui_task.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

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
    char action[24];
    char amount[NOTE_AMOUNT_BUF];
    char unit[8];
    char label[24];
    char id[VAULT_ID_BUF + 8];
} remote_confirm_request_t;

/* The gesture, said out loud on the card. approval.h makes approving a
 * two-second hold of button 1; nothing on screen used to mention either the
 * hold or the button, which left tapping -- and concluding the device was
 * dead -- as the obvious thing to try. */
#define CONFIRM_HINT "HOLD BTN1 2s"

/* While a button already down when the prompt appeared has not been seen
 * released -- nothing it does counts until then (approval.h), and the owner
 * sees only a bar that will not fill. */
#define RELEASE_HINT "LET GO FIRST"

/* After the response, not in front of it: the card is for the person holding
 * the device, and making the wallet wait for it helped nobody. */
#define OUTCOME_HOLD_MS 1800

static QueueHandle_t g_request_q;

#define BROWSE_IDLE_TIMEOUT_MS 15000

/* The unveiled QR is the secret in the clear, so clear it after a while rather
 * than wait for a button press. Longer than browse (scanning is deliberate). */
#define QR_SHOWN_TIMEOUT_MS 60000

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
static int find_confirmed(int from, int step, char out_id[VAULT_ID_BUF]) {
    vault_lock_acquire();
    size_t total = vault_count();
    int result = -1;
    if (out_id) {
        out_id[0] = '\0';
    }
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
                /* Capture the id under the same lock as the index; unveil()
                 * discloses by id, not position. */
                if (out_id) {
                    memcpy(out_id, meta.id, VAULT_ID_BUF);
                }
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
        display_message(DISPLAY_STATE_BROWSE, "NO NOTES", NULL, NULL);
        return;
    }
    note_meta_t meta;
    bool got = false;
    vault_lock_acquire();
    got = vault_get_meta_at((size_t)browse_index, &meta);
    vault_lock_release();
    if (!got) {
        display_message(DISPLAY_STATE_BROWSE, "NO NOTES", NULL, NULL);
        return;
    }

    char amount[NOTE_AMOUNT_BUF];
    char unit[8];
    char label[24];
    char id[VAULT_ID_BUF + 16];
    note_format_amount_parts(meta.amount_msat, amount, sizeof(amount), unit, sizeof(unit));
    note_format_label(meta.label, label, sizeof(label));
    snprintf(id, sizeof(id), "%s  %d", meta.id, position);
    /* No verb and no gesture hint: browsing is not a prompt, and the chord
     * that unveils from here is not the confirm hold. */
    display_note_detail(DISPLAY_STATE_BROWSE, NULL, amount, unit, label, id, NULL);
}

/* PENDING notes have no settled value and cannot be browsed or exported, so
 * they are not what the resting screen counts. */
static size_t confirmed_count(void) {
    size_t confirmed = 0;
    vault_lock_acquire();
    const size_t total = vault_count();
    for (size_t i = 0; i < total; i++) {
        note_meta_t meta;
        if (vault_get_meta_at(i, &meta) && meta.state == NOTE_STATE_CONFIRMED) {
            confirmed++;
        }
    }
    vault_lock_release();
    return confirmed;
}

/* The screen the device sits on all day. It was a flat dark rectangle, which
 * says nothing about whether it is alive, paired or holding anything, and got
 * pressed at to find out.
 *
 * How many notes, NOT what they are worth: a vault announcing its balance to
 * the room is a different device from one that makes you ask. */
static void draw_idle(size_t confirmed) {
    char title[24];
    if (confirmed == 0) {
        snprintf(title, sizeof(title), "NO NOTES");
    } else if (confirmed == 1) {
        snprintf(title, sizeof(title), "1 NOTE");
    } else {
        snprintf(title, sizeof(title), "%u NOTES", (unsigned)confirmed);
    }
    /* Both second lines are 12 characters or fewer: that is what fits across
     * the 240px panel at the readable minimum. */
    display_message(DISPLAY_STATE_IDLE, title, confirmed > 0 ? "TAP TO VIEW" : "PAIR TO ADD",
                    NULL);
}

static size_t show_idle(void) {
    const size_t confirmed = confirmed_count();
    draw_idle(confirmed);
    return confirmed;
}

/* A flat colour could not go stale; a number can. Notes arrive and are spent
 * over the wire with nobody near the device. Repaints only when the count
 * actually moves, so an idle device is not blitting forever. */
#define IDLE_RECOUNT_MS 1000

static void wipe(char *buf, size_t len) {
    volatile char *p = (volatile char *)buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = 0;
    }
}

/* Exports the selected note's secret and shows it as a QR. Resolves by the id
 * captured at selection, NOT browse position: a concurrent remote delete
 * compacts the array (vault.c's remove_at), so the index can point at a
 * different note by unveil time. Looking up by id discloses exactly the
 * selected note or nothing. Secret and URL are wiped once no longer needed. */
static bool unveil(const char *browse_id) {
    if (!browse_id || !browse_id[0]) {
        return false;
    }

    note_meta_t meta;
    char k1[VAULT_SECRET_HEX_BUF];
    bool got_secret = false;

    vault_lock_acquire();
    if (vault_get_meta(browse_id, &meta) && meta.state == NOTE_STATE_CONFIRMED) {
        got_secret = vault_export_secret(browse_id, k1) == VAULT_OK;
    }
    vault_lock_release();

    if (!got_secret) {
        return false;
    }

    char url[256];
    bool built = note_url_build_as(LNURLVAULT_QR_FORMAT, NULL, meta.host, k1, meta.amount_msat,
                                    url, sizeof(url));
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

/* The gesture line changes while the prompt is up (RELEASE_HINT), so both
 * drawings go through here rather than diverging by a field.
 *
 * No note id: eight hex identify a note to the wallet, not to the person
 * holding the device, and the row buys the gesture instead. It stays on the
 * browse card, where picking a specific note is the point. A request with no
 * note behind it (OTA, wipe) still gets the verb and the hint. */
static void draw_confirm_card(const remote_confirm_request_t *req, const char *hint) {
    display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, req->action,
                        req->has_detail ? req->amount : NULL,
                        req->has_detail ? req->unit : NULL,
                        req->has_detail ? req->label : NULL, NULL, hint);
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
    /* display.h: "Anything that discloses a secret MUST check this and refuse
     * rather than proceed -- the physical confirmation is the security
     * control, and a confirmation nobody can see is not one." Checked here
     * rather than per caller, because the same is true of a wipe and of an
     * OTA: approving what you cannot see is not approval. A blank screen is
     * also exactly when someone is most likely to be pressing buttons, trying
     * to work out why it is blank.
     *
     * display_ready() is settled at boot -- it reports whether the panel and
     * its row buffers came up, and those are allocated once in display_init()
     * and never freed -- so this cannot start refusing a device that is
     * working. */
    if (!display_ready()) {
        return CONFIRM_UNAVAILABLE;
    }
    approval_t ap;
    int64_t now = esp_timer_get_time();
    approval_begin(&ap, now, timeout_ms);
    /* approval_begin() assumes both buttons may be down and only a poll
     * clears that, so drawing first would tell every owner to let go. */
    approval_state_t state = approval_poll(&ap, buttons_raw_1(), buttons_raw_2(), now);
    bool waiting = approval_waiting_for_release(&ap);
    draw_confirm_card(req, waiting ? RELEASE_HINT : CONFIRM_HINT);
    display_progress(0);

    uint16_t drawn = 0;
    while (state == APPROVAL_PENDING) {
        now = esp_timer_get_time();
        state = approval_poll(&ap, buttons_raw_1(), buttons_raw_2(), now);

        /* Letting go swaps the hint, which is also the device visibly
         * answering them. */
        const bool now_waiting = approval_waiting_for_release(&ap);
        if (state == APPROVAL_PENDING && now_waiting != waiting) {
            waiting = now_waiting;
            draw_confirm_card(req, waiting ? RELEASE_HINT : CONFIRM_HINT);
            display_progress(drawn);
        }

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
    const char *title;
    const char *tail;
    switch (state) {
        case APPROVAL_GRANTED:
            result = CONFIRM_YES;
            card = DISPLAY_STATE_APPROVED;
            title = "APPROVED";
            tail = NULL;
            break;
        case APPROVAL_DENIED:
            result = CONFIRM_NO;
            card = DISPLAY_STATE_DECLINED;
            title = "DECLINED";
            tail = "NOTHING DONE";
            break;
        default:
            /* Its own card, deliberately: a prompt nobody answered must not
             * be left looking live, and must not be dressed as the owner
             * having said no. */
            result = CONFIRM_TIMEOUT;
            card = DISPLAY_STATE_EXPIRED;
            title = "NO ANSWER";
            tail = "NOTHING DONE";
            break;
    }
    /* In words, naming what it was about: a prompt that simply vanished never
     * said whether the device had timed out or refused. */
    display_message(card, title, req->action[0] ? req->action : NULL, tail);
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
    char browse_id[VAULT_ID_BUF] = {0}; /* identity of the selected note, for unveil() */
    int64_t browse_last_activity_us = 0;

    size_t idle_shown = show_idle();
    int64_t idle_checked_us = esp_timer_get_time();

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
            /* Answer first, then let the owner read the outcome. */
            vTaskDelay(pdMS_TO_TICKS(OUTCOME_HOLD_MS));
            mode = UI_IDLE;
            browse_index = -1;
            browse_id[0] = '\0';
            idle_shown = show_idle();
            idle_checked_us = esp_timer_get_time();
            continue;
        }

        button_event_t ev = buttons_poll();

        switch (mode) {
            case UI_IDLE:
                /* Keep the count honest. */
                if ((esp_timer_get_time() - idle_checked_us) >
                    (int64_t)IDLE_RECOUNT_MS * 1000) {
                    idle_checked_us = esp_timer_get_time();
                    const size_t now_confirmed = confirmed_count();
                    if (now_confirmed != idle_shown) {
                        idle_shown = now_confirmed;
                        draw_idle(idle_shown);
                    }
                }
                if (ev == BTN_EVENT_1_TAP || ev == BTN_EVENT_2_TAP) {
                    browse_index = find_confirmed(-1, 1, browse_id);
                    if (browse_index >= 0) {
                        mode = UI_BROWSE;
                        browse_last_activity_us = esp_timer_get_time();
                        show_browse_note(browse_index, confirmed_position(browse_index));
                    }
                }
                break;

            case UI_BROWSE:
                if (ev == BTN_EVENT_BOTH_CHORD) {
                    if (unveil(browse_id)) {
                        mode = UI_QR_SHOWN;
                    } else {
                        /* Note gone, export refused, or a URL too long for a
                         * QR this panel can draw -- a red flash alone left the
                         * owner to guess which. */
                        display_message(DISPLAY_STATE_DECLINED, "FAILED", "NOT SHOWN", NULL);
                        vTaskDelay(pdMS_TO_TICKS(900));
                        show_browse_note(browse_index, confirmed_position(browse_index));
                    }
                    browse_last_activity_us = esp_timer_get_time();
                } else if (ev == BTN_EVENT_1_TAP) {
                    browse_index = find_confirmed(browse_index, 1, browse_id);
                    browse_last_activity_us = esp_timer_get_time();
                    show_browse_note(browse_index, confirmed_position(browse_index));
                } else if (ev == BTN_EVENT_2_TAP) {
                    browse_index = find_confirmed(browse_index, -1, browse_id);
                    browse_last_activity_us = esp_timer_get_time();
                    show_browse_note(browse_index, confirmed_position(browse_index));
                } else if ((esp_timer_get_time() - browse_last_activity_us) >
                           (int64_t)BROWSE_IDLE_TIMEOUT_MS * 1000) {
                    mode = UI_IDLE;
                    browse_id[0] = '\0';
                    idle_shown = show_idle();
                    idle_checked_us = esp_timer_get_time();
                }
                break;

            case UI_QR_SHOWN:
                if (ev == BTN_EVENT_1_TAP || ev == BTN_EVENT_2_TAP || ev == BTN_EVENT_BOTH_CHORD) {
                    mode = UI_BROWSE;
                    browse_last_activity_us = esp_timer_get_time();
                    show_browse_note(browse_index, confirmed_position(browse_index));
                } else if ((esp_timer_get_time() - browse_last_activity_us) >
                           (int64_t)QR_SHOWN_TIMEOUT_MS * 1000) {
                    /* Clear the secret from screen rather than wait for a press. */
                    mode = UI_IDLE;
                    browse_index = -1;
                    browse_id[0] = '\0';
                    idle_shown = show_idle();
                    idle_checked_us = esp_timer_get_time();
                }
                break;
        }

        wdt_feed();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void ui_task_init(void) {
    /* Split from ui_task_start() so the queue exists before any transport
     * starts: a gated command arriving during boot is then queued for the
     * task, not sent to a NULL handle. Idempotent. */
    if (!g_request_q) {
        g_request_q = xQueueCreate(4, sizeof(remote_confirm_request_t));
    }
}

void ui_task_start(void) {
    ui_task_init(); /* idempotent, in case start is called without init */
    xTaskCreate(ui_task_fn, "ui_task", 4096, NULL, 5, NULL);
}

/* Kept as the plain name, delegating, so a caller that only has a timeout
 * still compiles.
 *
 * This is not tidiness. Renaming it outright made two independently-correct
 * branches break each other on merge: another change added a confirm entry
 * point calling request_confirm(), git merged both texts without complaint
 * because they touch different lines, and the result was a call to a function
 * that no longer existed. Each branch built fine alone; whichever landed
 * second broke main. Caught by building the two together rather than by
 * either one's own CI, which is a thing worth doing and not a thing to rely
 * on. Keeping the old name costs one line and removes the hazard. */
static confirm_result_t request_confirm_detailed(uint32_t timeout_ms, const note_meta_t *note,
                                                  const char *action);

static confirm_result_t request_confirm(uint32_t timeout_ms, const char *action) {
    return request_confirm_detailed(timeout_ms, NULL, action);
}

static confirm_result_t request_confirm_detailed(uint32_t timeout_ms, const note_meta_t *note,
                                                  const char *action) {
    /* Refuse rather than crash if a command reaches here before ui_task_init(),
     * or if the per-request queue can't be allocated. */
    if (!g_request_q) {
        return CONFIRM_UNAVAILABLE;
    }
    QueueHandle_t resp_q = xQueueCreate(1, sizeof(confirm_result_t));
    if (!resp_q) {
        return CONFIRM_UNAVAILABLE;
    }
    remote_confirm_request_t req = {.timeout_ms = timeout_ms, .response_q = resp_q};
    if (action && action[0]) {
        /* The gated destructive commands arrive as their wire names --
         * "mark_spent", "discard" -- because that is what dispatcher.c has to
         * hand. Uppercased with underscores opened out, rather than mapped
         * through a table here: a table would be one more thing to keep in
         * step with dispatcher.c, and would silently show the wrong verb for
         * any command added without remembering to update it. */
        size_t n = 0;
        for (; action[n] && n + 1 < sizeof(req.action); n++) {
            char c = action[n];
            if (c == '_') {
                c = ' ';
            } else if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            req.action[n] = c;
        }
        req.action[n] = '\0';
    }
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
    /* Kept to 12 characters, which is what fits across the narrower of the two
     * panels at the readable minimum scale (240px less margins, 6px advance,
     * scale 3). Longer verbs get clipped, not shrunk -- see font5x7.h. */
    return request_confirm_detailed(timeout_ms, note, "SHOW SECRET");
}

confirm_result_t ui_task_request_ota_confirm(uint32_t timeout_ms) {
    return request_confirm(timeout_ms, "NEW FIRMWARE");
}

confirm_result_t ui_task_request_action_confirm(const char *action, const note_meta_t *note,
                                                 uint32_t timeout_ms) {
    /* The action name IS shown now. It was plumbed this far and then dropped
     * on the floor, which meant every gated destructive command -- mark_spent,
     * discard, delete, rename -- put up the same card export_secret does. */
    return request_confirm_detailed(timeout_ms, note, action);
}

confirm_result_t ui_task_request_wipe_confirm(uint32_t timeout_ms) {
    /* No note to name: a wipe is about all of them, which is exactly why the
     * verb has to be on screen. This card and a single note's disclosure used
     * to be the same flat amber. */
    return request_confirm(timeout_ms, "WIPE ALL");
}
