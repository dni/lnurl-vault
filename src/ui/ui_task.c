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
#include "board.h"
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
#include "screen_sleep.h"
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
 * dead -- as the obvious thing to try.
 *
 * Naming the button was not enough. The two buttons carry no labels a person
 * can see, so "BTN1" only helps someone who already knows which one it is,
 * and the natural reach is for the left -- which on the classic board is
 * cancel. That cost two bench runs of section 17 with this hint on screen and
 * correct throughout: three approvals timed out, and a fourth came back
 * user_declined from a press on the wrong button.
 *
 * So say the side instead, where the board knows it (board_confirm_side).
 * "RIGHT" needs no lookup and no prior knowledge, and it is the one fact the
 * owner is actually missing while holding the thing. A board that has not had
 * its side established on a bench keeps the old wording rather than guess:
 * pointing someone at the wrong button is worse than making them work it out.
 */
#define CONFIRM_HINT_UNKNOWN_SIDE "HOLD BTN1 2s"
#define CONFIRM_HINT_WITH_GUIDE "HOLD 2s"

/* display.c cannot ask board.h -- the native tests compile it without any
 * board file -- so the side is pushed down once, at startup, the same way
 * display_set_state pushes the palette. */
static void publish_confirm_side(void) {
    switch (board_confirm_side()) {
        case BOARD_CONFIRM_SIDE_LEFT:
            display_set_confirm_side(DISPLAY_CONFIRM_SIDE_LEFT);
            break;
        case BOARD_CONFIRM_SIDE_RIGHT:
            display_set_confirm_side(DISPLAY_CONFIRM_SIDE_RIGHT);
            break;
        case BOARD_CONFIRM_SIDE_UNKNOWN:
        default:
            display_set_confirm_side(DISPLAY_CONFIRM_SIDE_UNKNOWN);
            break;
    }
}

static const char *confirm_hint(void) {
    switch (board_confirm_side()) {
        case BOARD_CONFIRM_SIDE_LEFT:
        case BOARD_CONFIRM_SIDE_RIGHT:
            /* The card draws both buttons with the right one filled, so the
             * words do not have to name a side as well. */
            return CONFIRM_HINT_WITH_GUIDE;
        case BOARD_CONFIRM_SIDE_UNKNOWN:
        default:
            return CONFIRM_HINT_UNKNOWN_SIDE;
    }
}

/* While a button already down when the prompt appeared has not been seen
 * released -- nothing it does counts until then (approval.h), and the owner
 * sees only a bar that will not fill. */
#define RELEASE_HINT "LET GO FIRST"

/* After the response, not in front of it: the card is for the person holding
 * the device, and making the wallet wait for it helped nobody. */
#define OUTCOME_HOLD_MS 1800

static QueueHandle_t g_request_q;

/* When the screen goes dark. Policy in src/proto/screen_sleep.c, the light
 * itself in display.c; this file is the only thing that joins them. */
static screen_sleep_t g_screen;

/* A new screen has just been drawn, or a button has just been touched.
 *
 * Called AFTER drawing, never before: waking lights whatever is already in
 * the panel, so lighting first shows the card from a minute ago for a frame.
 * Everything that puts something new on screen ends with this, which is why
 * no caller has to remember whether the screen was dark -- and why adding a
 * screen that forgets shows up immediately as one that will not light. */
static void screen_awake(void) {
    if (screen_sleep_touch(&g_screen, esp_timer_get_time())) {
        display_wake();
    }
}

#define BROWSE_IDLE_TIMEOUT_MS 15000

/* The unveiled QR is the secret in the clear, so clear it after a while rather
 * than wait for a button press. Longer than browse (scanning is deliberate). */
#define QR_SHOWN_TIMEOUT_MS 60000

/* How long the screen stays lit with nothing new on it.
 *
 * A vault lives plugged in. Left alone it holds the same resting card in the
 * same pixels for as long as it has power, and an IPS panel treated like that
 * ends up with a faint permanent copy of it -- while burning the backlight
 * for a screen nobody is looking at. Long enough to walk over and read
 * something; short enough that a device on a desk spends its day dark.
 *
 * Comfortably longer than the longest confirm window (30s), though nothing
 * depends on that: the tick that blanks the screen is not reached while a
 * prompt is up. See screen_sleep.h. */
#define SCREEN_SLEEP_MS 60000

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

/* Defined below, beside the other counting helpers; needed here for the
 * browse card's "3 of 12" band. Takes vault_lock itself, so it must not be
 * called with it already held. */
static size_t confirmed_count(void);

/* Shows the selected note while browsing, so unveiling the wrong one takes a
 * deliberate misreading rather than a miscount. This replaced a blinked-out
 * position count, which told you where you were in the list but not which
 * note or for how much -- and the chord that follows discloses a bearer
 * secret on screen. Issue #9.
 *
 * `position` is the 1-based place among CONFIRMED notes, shown next to the id
 * so the count the old flash conveyed is not lost. */
static void draw_browse_note(int browse_index, int position) {
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
    char badge[24];
    note_format_amount_parts(meta.amount_msat, amount, sizeof(amount), unit, sizeof(unit));
    note_format_label(meta.label, label, sizeof(label));
    /* Where you are goes in the band, where the verb goes on a prompt. It used
     * to be tacked onto the end of the id -- "f822a462  3" -- which put the
     * one number that says how far through the list you are in the same
     * weight, the same size and the same line as eight characters of hex that
     * mean nothing to a person. The id keeps its own line and the band says
     * which of how many. */
    snprintf(badge, sizeof(badge), "NOTE %d/%u", position, (unsigned)confirmed_count());
    /* Still no gesture hint: browsing is not a prompt, and the chord that
     * unveils from here is not the confirm hold. */
    display_note_detail(DISPLAY_STATE_BROWSE, badge, amount, unit, label, meta.id, NULL);
}

static void show_browse_note(int browse_index, int position) {
    draw_browse_note(browse_index, position);
    screen_awake();
}

/* PENDING notes have no settled value and cannot be browsed or exported, so
 * they are not what the resting screen counts. */
static size_t count_confirmed_locked(void) {
    size_t confirmed = 0;
    const size_t total = vault_count();
    for (size_t i = 0; i < total; i++) {
        note_meta_t meta;
        if (vault_get_meta_at(i, &meta) && meta.state == NOTE_STATE_CONFIRMED) {
            confirmed++;
        }
    }
    return confirmed;
}

static size_t confirmed_count(void) {
    vault_lock_acquire();
    const size_t confirmed = count_confirmed_locked();
    vault_lock_release();
    return confirmed;
}

/* False if the vault is busy. The idle count is advisory and this runs once a
 * second, so skipping a pass costs nothing -- whereas blocking would put
 * ui_task behind a lock with no timeout for a number nobody is waiting on. */
static bool try_confirmed_count(size_t *out) {
    if (!vault_lock_try_acquire()) {
        return false;
    }
    *out = count_confirmed_locked();
    vault_lock_release();
    return true;
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

/* draw_idle() keeps the number honest; this SHOWS it to someone. The
 * difference is the whole reason they are two functions: notes arriving and
 * being spent over the wire, with nobody near the device, repaint through
 * draw_idle() and must not light a dark room. */
static size_t show_idle(void) {
    const size_t confirmed = confirmed_count();
    draw_idle(confirmed);
    screen_awake();
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

/* Room for the longest form a note takes. The bech32 LNURL is the big one --
 * about 177 characters for an ordinary mint, against the claim link's 138 --
 * and note_url_build_as() refuses rather than truncates, so undersizing this
 * shows up as a format that simply will not display. */
#define UNVEIL_URL_BUF 320

/* What the strip under the code says. The name of the form on screen, and
 * that there is another one behind button 1 -- without which the cycling is
 * a feature nobody discovers. Button 2 still dismisses, as any tap used to;
 * it is not on the strip because the two together do not fit at a size worth
 * reading, and leaving is the thing people already know how to do. */
#define QR_CAPTION_BUF 32

/* Exports the selected note's secret and shows it as a QR, in `format`.
 *
 * Resolves by the id captured at selection, NOT browse position: a concurrent
 * remote delete compacts the array (vault.c's remove_at), so the index can
 * point at a different note by unveil time. Looking up by id discloses exactly
 * the selected note or nothing. Secret and URL are wiped once no longer
 * needed. */
static bool unveil(const char *browse_id, note_url_format_t format) {
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

    char url[UNVEIL_URL_BUF];
    bool built = note_url_build_as(format, NULL, meta.host, k1, meta.amount_msat, url,
                                    sizeof(url));
    wipe(k1, sizeof(k1));
    if (!built) {
        return false;
    }

    char caption[QR_CAPTION_BUF];
    snprintf(caption, sizeof(caption), "%s   BTN1 NEXT", note_url_format_name(format));

    bool shown = qr_display_show(url, caption);
    wipe(url, sizeof(url));
    if (shown) {
        screen_awake();
    }
    return shown;
}

/* Shows the note in the next form that will actually render.
 *
 * Tries each in turn rather than only the next one: a mint host long enough
 * to push the bech32 form past what a QR version here can hold would
 * otherwise make button 1 look dead on that note, on that form, and nowhere
 * else -- which is the kind of fault nobody reproduces. Returns false only if
 * NONE of them renders, which is the same condition the chord already treats
 * as a failed unveil.
 *
 * `*format` is left on whatever ended up on screen. */
static bool unveil_next_format(const char *browse_id, note_url_format_t *format) {
    for (int step = 1; step <= NOTE_URL_FORMAT_COUNT; step++) {
        const note_url_format_t next =
            (note_url_format_t)(((int)*format + step) % NOTE_URL_FORMAT_COUNT);
        if (unveil(browse_id, next)) {
            *format = next;
            return true;
        }
    }
    return false;
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
    screen_awake();
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
    draw_confirm_card(req, waiting ? RELEASE_HINT : confirm_hint());
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
            draw_confirm_card(req, waiting ? RELEASE_HINT : confirm_hint());
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
    screen_awake();
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

    /* A panel that never came up gets a timeout of 0, which means never
     * sleep: there is nothing to blank, and a device that believes its screen
     * is dark spends the first press of every gesture turning on a light that
     * does not exist. */
    screen_sleep_init(&g_screen, esp_timer_get_time(),
                      display_ready() ? SCREEN_SLEEP_MS : 0);

    ui_mode_t mode = UI_IDLE;
    int browse_index = -1;
    char browse_id[VAULT_ID_BUF] = {0}; /* identity of the selected note, for unveil() */
    int64_t browse_last_activity_us = 0;
    /* Which encoding the unveil screen is showing. Reset to the build default
     * on every fresh chord rather than carried between notes: the last note
     * you handed over says nothing about which wallet the next person has. */
    note_url_format_t qr_format = LNURLVAULT_QR_FORMAT;

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
            /* Answer first, then let the owner read the outcome -- unless
             * there was no screen to draw it on, in which case holding for a
             * card nobody can see just makes a degraded vault slower. */
            if (result != CONFIRM_UNAVAILABLE) {
                vTaskDelay(pdMS_TO_TICKS(OUTCOME_HOLD_MS));
            }
            mode = UI_IDLE;
            browse_index = -1;
            browse_id[0] = '\0';
            idle_shown = show_idle();
            idle_checked_us = esp_timer_get_time();
            continue;
        }

        button_event_t ev = buttons_poll();

        /* Dark, the first touch buys the light and nothing else.
         *
         * On the raw level rather than on the tap a release produces, so the
         * screen comes up under the thumb instead of after it -- and the
         * press is then consumed, so waking a vault never also starts it
         * browsing bearer notes. Whichever way the press arrives, it is spent
         * here: `ev` is dropped rather than handed to the mode below. */
        if (screen_sleep_is_asleep(&g_screen)) {
            if (ev != BTN_EVENT_NONE || buttons_raw_1() || buttons_raw_2()) {
                buttons_consume_press();
                /* Repaints in the dark and lights the panel afterwards, in
                 * that order -- see screen_awake(). Recounting on the way is
                 * the point: the count on the glass is from before the
                 * screen went out, and notes move over the wire. */
                idle_shown = show_idle();
                idle_checked_us = esp_timer_get_time();
            }
            wdt_feed();
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        switch (mode) {
            case UI_IDLE:
                /* Keep the count honest. */
                if ((esp_timer_get_time() - idle_checked_us) >
                    (int64_t)IDLE_RECOUNT_MS * 1000) {
                    size_t now_confirmed = 0;
                    if (try_confirmed_count(&now_confirmed)) {
                        idle_checked_us = esp_timer_get_time();
                        if (now_confirmed != idle_shown) {
                            idle_shown = now_confirmed;
                            draw_idle(idle_shown);
                        }
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
                    qr_format = LNURLVAULT_QR_FORMAT;
                    if (unveil(browse_id, qr_format)) {
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
                /* Button 1 cycles the encoding; button 2 leaves. It used to be
                 * that any tap left, which was fine when there was one form to
                 * show and nothing to choose between.
                 *
                 * Three forms because no single one works everywhere: LUD-25
                 * names lnurlw:// and bech32 LNURL, and neither of those opens
                 * on a stock phone camera, which is what the claim link is
                 * for. Which one a given wallet accepts is a question the
                 * person holding the device can now answer by pressing a
                 * button, instead of by rebuilding the firmware.
                 *
                 * Note what is deliberately NOT here: cycling does not touch
                 * browse_last_activity_us. The secret leaves the screen at a
                 * fixed deadline from the chord that unveiled it, however many
                 * times it is redrawn, so this cannot become a way to hold a
                 * bearer secret up indefinitely by tapping. Another chord is
                 * one gesture away if the window runs out. */
                if (ev == BTN_EVENT_1_TAP) {
                    if (!unveil_next_format(browse_id, &qr_format)) {
                        display_message(DISPLAY_STATE_DECLINED, "FAILED", "NOT SHOWN", NULL);
                        vTaskDelay(pdMS_TO_TICKS(900));
                        mode = UI_BROWSE;
                        browse_last_activity_us = esp_timer_get_time();
                        show_browse_note(browse_index, confirmed_position(browse_index));
                    }
                } else if (ev == BTN_EVENT_2_TAP || ev == BTN_EVENT_BOTH_CHORD) {
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

        /* Lights out. Deliberately here, outside the switch and outside
         * service_remote_confirm() -- which is the one place this loop does
         * not run. That is what stops a live prompt going dark under the
         * person answering it, and it is structural rather than a flag
         * somebody has to remember to set.
         *
         * A QR on screen is safe for the same reason it is safe when it
         * times out: display_sleep() clears the panel, so the secret leaves
         * the glass rather than merely losing its backlight. */
        if (screen_sleep_expired(&g_screen, esp_timer_get_time())) {
            display_sleep();
            /* Going dark ends any browse. Waking to a position picked a
             * minute ago -- or with a note still selected for the chord that
             * unveils it -- is not where a vault should resume. */
            mode = UI_IDLE;
            browse_index = -1;
            browse_id[0] = '\0';
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
    publish_confirm_side();
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

confirm_result_t ui_task_request_prune_confirm(uint32_t count, uint32_t timeout_ms) {
    if (!g_request_q) {
        return CONFIRM_UNAVAILABLE;
    }
    QueueHandle_t resp_q = xQueueCreate(1, sizeof(confirm_result_t));
    if (!resp_q) {
        return CONFIRM_UNAVAILABLE;
    }
    remote_confirm_request_t req = {.timeout_ms = timeout_ms, .response_q = resp_q};
    /* Not routed through request_confirm_detailed(): that one formats a
     * note's amount, and what has to be on this card is a COUNT. It borrows
     * the amount's slot on purpose -- same position, same size -- because
     * "25" is exactly as much the reviewable fact here as "21 000" is on a
     * disclosure. */
    snprintf(req.action, sizeof(req.action), "PRUNE SPENT");
    req.has_detail = true;
    snprintf(req.amount, sizeof(req.amount), "%u", (unsigned)count);
    snprintf(req.unit, sizeof(req.unit), "%s", count == 1 ? "note" : "notes");

    xQueueSend(g_request_q, &req, portMAX_DELAY);
    confirm_result_t result = CONFIRM_TIMEOUT;
    xQueueReceive(resp_q, &result, portMAX_DELAY);
    vQueueDelete(resp_q);
    return result;
}

confirm_result_t ui_task_request_wipe_confirm(uint32_t timeout_ms) {
    /* No note to name: a wipe is about all of them, which is exactly why the
     * verb has to be on screen. This card and a single note's disclosure used
     * to be the same flat amber. */
    return request_confirm(timeout_ms, "WIPE ALL");
}
