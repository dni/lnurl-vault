#ifndef LNURLVAULT_DEVICE_REBOOT_H
#define LNURLVAULT_DEVICE_REBOOT_H

/* Shared by dispatcher_deps_t's reset_fn (the `reset` command) and a
 * successfully finalized `ota_finish` — both need the exact same thing: a
 * reboot that happens on a short delay rather than synchronously, so
 * whatever response the dispatcher just wrote actually has a chance to
 * leave the TX buffer first. See dispatcher.h's reset_fn comment for the
 * full reasoning (an immediate esp_restart() would race that and likely
 * win, leaving the client with a device that vanished instead of a
 * reply). */
void device_reboot_delayed(void);

#endif
