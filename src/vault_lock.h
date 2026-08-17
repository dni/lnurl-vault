#ifndef LNURLVAULT_VAULT_LOCK_H
#define LNURLVAULT_VAULT_LOCK_H

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
 * export_secret's confirm wait is the one place that's actually enforced,
 * not just intended: main.c's confirm_export_on_device() explicitly
 * releases this lock before ui_task_request_remote_confirm()'s up-to-30s
 * wait and reacquires it after, specifically so dispatcher_handle() (called
 * from inside this lock by both transports) doesn't hold it — and, for
 * serial, stall the TinyUSB task handle_rx() runs nested inside — for the
 * whole confirm window. See main.c's comment there for why that release is
 * safe (vault_export_secret() re-validates state after reacquiring).
 * ota_begin's confirm wait (main.c's ota_approve_on_device()) still holds
 * this lock the whole time, a known but NOT yet fixed instance of the same
 * problem — see that function's comment in main.c for why it wasn't given
 * the same treatment. */
void vault_lock_init(void);
void vault_lock_acquire(void);
void vault_lock_release(void);

#endif
