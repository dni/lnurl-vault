#ifndef LNURLVAULT_UI_TASK_H
#define LNURLVAULT_UI_TASK_H

#include <stdint.h>

#include "dispatcher.h" /* confirm_result_t */
#include "vault.h"       /* note_meta_t */

/* The single task that owns both buttons and the display, for two
 * purposes that must never fight over them concurrently:
 *
 *  1. Local, offline note browsing: tap either button to cycle through
 *     CONFIRMED notes, each drawn with its amount, label, id and position,
 *     then hold both together for ~200ms ("the chord") to unveil the
 *     selected note as a QR. See docs/PROTOCOL.md's "On-device note
 *     browsing" section.
 *  2. Servicing remote export_secret confirm requests from dispatcher.c,
 *     via ui_task_request_remote_confirm() below — the same physical
 *     confirm/cancel gate main.c wired up before this feature existed.
 *
 * Call ui_task_start() once from app_main(), after vault_init(),
 * dispatcher_init(), buttons_init(), and display_init(). */
void ui_task_start(void);

/* Creates the request queue without starting the task. Call once, before any
 * transport starts, so a gated command arriving during boot is queued rather
 * than sent to a NULL handle; ui_task_start() then starts the task last.
 * Idempotent, and folded into ui_task_start() if you skip it. */
void ui_task_init(void);

/* Called from any other task (in practice, the active transport's task, via
 * main.c's confirm_export_on_device) to request an on-device confirm/
 * cancel for one export_secret call coming in over serial/BLE. Blocks the
 * calling task until ui_task resolves it (a tap decision or timeout) — this
 * is the only way another task touches the confirm decision; it never polls
 * buttons itself. */
confirm_result_t ui_task_request_remote_confirm(const note_meta_t *note, uint32_t timeout_ms);

/* Same physical confirm/cancel gate and the same underlying request queue
 * as ui_task_request_remote_confirm() above, for an incoming OTA image
 * (dispatcher.c's ota_begin) instead of a note export — there's no note to
 * pass, so this is its own entry point rather than overloading the note
 * one with NULL. Shows the same generic "confirm pending" display state;
 * see README.md's "Known limitations" on why this can't yet show the
 * image size on-screen. */
confirm_result_t ui_task_request_ota_confirm(uint32_t timeout_ms);

/* Same gate again, for `wipe` -- erasing every note on the device. Its own
 * entry point rather than a shared one because what the screen ought to say
 * for this is not what it says for an export or a firmware image, and
 * sharing the call would quietly rule that out. Shows the same generic
 * confirm state for now; see README.md's "Known limitations". */
confirm_result_t ui_task_request_wipe_confirm(uint32_t timeout_ms);

/* The gate in front of a command that changes or destroys a note the owner
 * already holds -- mark_spent, delete, discard, rename (see dispatcher.h's
 * action_confirm_fn, and issue #16).
 *
 * `action` is the command name and `note` is what it will happen to, so the
 * screen can say which. That matters more here than for export_secret: there
 * is only one thing an export can do, and four things this can, so approving
 * the wrong one is a mistake a person can actually make. */
confirm_result_t ui_task_request_action_confirm(const char *action, const note_meta_t *note,
                                                 uint32_t timeout_ms);

/* The gate in front of `prune_spent` -- forgetting every note the vault
 * already knows is dead (dispatcher.h's prune_approve_fn).
 *
 * Its own entry point because the card has to carry the COUNT, and the count
 * is not a note. It is drawn where an amount goes, at the size an amount
 * gets, because it is the one number that makes this reviewable: approving
 * "forget some notes" is not approving anything, and the difference between
 * 25 and 1 is the difference between housekeeping and a host having been busy
 * behind your back. */
confirm_result_t ui_task_request_prune_confirm(uint32_t count, uint32_t timeout_ms);

#endif
