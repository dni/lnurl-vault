#ifndef LNURLVAULT_CRASH_CRUMB_H
#define LNURLVAULT_CRASH_CRUMB_H

#include <stdbool.h>
#include <stdint.h>

/* Records what the device was doing, so that after an unexpected reset it
 * can say why rather than only that it happened.
 *
 * This exists because of a real debugging session, not in the abstract. A
 * command sent over BLE was killing the link and silently not taking
 * effect. The classic board builds with CONFIG_ESP_CONSOLE_NONE -- correctly,
 * since UART0 carries the command protocol there and log lines interleaved
 * with JSON responses corrupt the stream -- so there was no panic output to
 * read, and no console to attach. Narrowing it down meant inferring device
 * state from note counts and from note created_at timestamps (which are
 * seconds since boot, so they double as a reboot detector). That works, and
 * it should not have been necessary.
 *
 * The breadcrumb lives in RTC slow memory with RTC_NOINIT_ATTR: the startup
 * code does not clear it, so it survives a panic, a watchdog reset and a
 * software restart, and is lost on power-off -- which is exactly the
 * lifetime wanted. A crash that a power cycle already cleared is not one
 * anybody is still chasing.
 *
 * WHAT MUST NEVER GO IN HERE: anything derived from a secret. Only the
 * command NAME is recorded, never the line it came from -- import_secret
 * carries a raw k1 in its arguments, and this memory survives a reset and
 * is readable by whatever runs next. See dispatcher.h's trace_cmd_fn. */

/* Reads whatever the previous boot left behind, then re-arms for this one.
 * Call once, early in app_main(), before anything can crash. */
void crash_crumb_boot(void);

/* Records the command now in flight. Cleared when it completes, so a
 * breadcrumb left set at the next boot names the command that did not. */
void crash_crumb_set_cmd(const char *cmd);
void crash_crumb_clear_cmd(void);

/* True if this boot followed something unplanned -- a panic, a watchdog, a
 * brownout -- rather than a power-on, a software restart or a deliberate
 * external reset. */
bool crash_crumb_last_boot_was_unexpected(void);

/* Short stable identifier for what reset the device: "poweron", "panic",
 * "task_wdt", "int_wdt", "brownout", "sw", "ext", "deepsleep", "unknown".
 * Never NULL. */
const char *crash_crumb_reset_reason(void);

/* The command that was in flight when the device went down, or NULL if it
 * went down cleanly (or this is a power-on boot, where nothing survives). */
const char *crash_crumb_last_cmd(void);

/* Boots since the last power-on. 1 on a cold boot, so a value above 1 says
 * the device has restarted without losing power -- worth seeing next to the
 * reason. */
uint32_t crash_crumb_boot_count(void);

#endif
