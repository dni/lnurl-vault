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
 * kept short — a handful of vault_* calls, never something like a display
 * animation or the 30s remote-confirm wait — so contention should be rare
 * and brief in practice. */
void vault_lock_init(void);
void vault_lock_acquire(void);
void vault_lock_release(void);

#endif
