#ifndef LNURLVAULT_VAULT_LOCK_H
#define LNURLVAULT_VAULT_LOCK_H

#include <stdbool.h>

/* src/vault/vault.c is deliberately plain portable C with no synchronization
 * of its own — it's shared with the native test binary, which is single-
 * threaded. On firmware, vault_* functions are now called from more than
 * one FreeRTOS task: whichever transport task is running dispatcher_handle()
 * (TinyUSB's rx callback for serial, the NimBLE host task for BLE), and
 * ui_task.c for local on-device note browsing. Every one of those call
 * sites takes this single global lock around its vault_* calls:
 * main.c (creates it, first thing in app_main), transport/serial_cdc.c and
 * transport/ble_gatt.c (around each dispatcher_handle() call), and
 * ui/ui_task.c (around its own direct vault_* calls). Critical sections are
 * meant to be kept short — a handful of vault_* calls, never something like
 * a display animation or the 30s remote-confirm wait.
 *
 * Both of the places that stop to ask the device's owner a question now
 * enforce that, rather than merely intending it: main.c's
 * confirm_export_on_device() and ota_approve_on_device() each release this
 * lock before their up-to-30s wait and reacquire it after. That is not
 * politeness, it is the fix for a real deadlock — ui_task is the task on
 * the other side of that wait, and it takes this lock for its own note
 * browsing, so a caller holding it across the wait blocks ui_task on the
 * one lock it needs to reach the request queue it is being waited on.
 * Neither task ever recovers. See cmd_lock.h for how the dispatcher's own
 * state stays protected across that release, and main.c's comments there
 * for why the release is safe (vault.c re-validates after reacquiring).
 *
 * The rule, then: this lock may be held across a handful of vault_* calls
 * and nothing else. Never across a wait for a human, a display animation,
 * or anything else ui_task might be in the middle of. */
void vault_lock_init(void);
void vault_lock_acquire(void);
void vault_lock_release(void);

/* Takes the lock if it is free, false if not. For callers that would rather
 * skip this pass than wait -- the idle screen's note count is advisory, and
 * acquire() has no timeout, so polling it once a second would put the one
 * task that must always make progress behind a lock it cannot give up on. */
bool vault_lock_try_acquire(void);

#endif
