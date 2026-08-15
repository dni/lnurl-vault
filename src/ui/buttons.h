#ifndef LNURLVAULT_BUTTONS_H
#define LNURLVAULT_BUTTONS_H

#include <stdint.h>

#include "dispatcher.h" /* for confirm_result_t */

void buttons_init(void);

/* Blocks (polling with simple debounce) until BUTTON_1 (confirm) or
 * BUTTON_2 (cancel) is pressed, or timeout_ms elapses. Requires both
 * buttons to be released first, so a button already held down from
 * whatever triggered the prompt can't auto-confirm it. This is the actual
 * security-relevant gate in front of vault_export_secret — see
 * dispatcher.h's export_confirm_fn and main.c's confirm_export_on_device. */
confirm_result_t buttons_wait_confirm(uint32_t timeout_ms);

#endif
