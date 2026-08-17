#ifndef LNURLVAULT_CMD_LOCK_H
#define LNURLVAULT_CMD_LOCK_H

/* The invariant above, enforced rather than only written down. ui_task.c
 * defines LNURLVAULT_TU_IS_UI_TASK before its includes; reaching this header
 * from there means someone is about to take this lock on the task that every
 * human wait depends on, which reintroduces the deadlock this file exists to
 * remove. Same approach as board.h's _Static_assert on panel orientation: an
 * invariant a build can check should not be left to a comment. */
#ifdef LNURLVAULT_TU_IS_UI_TASK
#error "ui_task must never acquire cmd_lock -- see the invariant in this header. \
Holding it across a human wait deadlocks the task that services the buttons, \
and only a power cycle recovers. If ui_task genuinely needs to serialize \
against the transports, the answer is a different mechanism, not this lock."
#endif

/* Serializes whole dispatcher_handle() calls across transports.
 *
 * There are two things that need serializing here, and they were previously
 * both done by vault_lock, which is what made one of them impossible to get
 * right:
 *
 *  1. vault.c's note state, shared between the transports and ui_task.c's
 *     local note browsing. That is vault_lock's job and stays there.
 *  2. dispatcher.c's own internal state -- today the OTA session (g_ota):
 *     which image is being received, how many of its bytes have arrived, the
 *     running SHA-256. Two transports interleaving ota_begin/ota_chunk would
 *     corrupt an image with nothing to catch it, since none of that state
 *     re-validates itself the way vault.c's notes do. That is this lock's
 *     job.
 *
 * The reason they must be different locks: a command may have to stop and
 * ask the device's owner a question, and export_secret and ota_begin both
 * do. That wait is up to 30 seconds of a human deciding whether to press a
 * button, and the button belongs to ui_task -- which takes vault_lock for
 * its own browsing. So vault_lock CANNOT be held across the wait: ui_task
 * blocks on it, never reaches the request queue it is being waited on, and
 * both tasks are stuck until someone power-cycles the vault. Reachable any
 * time someone is browsing notes on the device while a paired host asks to
 * export or to start an update.
 *
 * Splitting the two makes the fix possible: the confirm callbacks in main.c
 * release vault_lock around the wait (and vault.c re-validates after
 * reacquiring it, so a concurrent change fails closed), while this lock is
 * held for the whole command including the wait. Purpose 2 keeps its
 * protection over exactly the window that needs it; purpose 1 stops
 * deadlocking ui_task.
 *
 * THE INVARIANT THAT MAKES THIS SAFE, AND THE ONE TO PRESERVE:
 *
 *   ui_task must never acquire this lock.
 *
 * ui_task is the task on the other side of every human wait. If it ever
 * waits on this lock, the deadlock above comes straight back in a new place
 * -- and this time vault_lock's release would not help, because the thing
 * being held across the wait is this. Only transport tasks take it, only
 * around dispatcher_handle(), and always before vault_lock (the one and only
 * ordering, so there is no cycle to close).
 *
 * A transport blocked here while another is mid-confirm waits, rather than
 * failing fast: the vault really is busy asking its owner a question, and
 * answering a second host's command out from under that would need the
 * dispatcher state this lock exists to protect. It is a task that waits, not
 * an event loop -- see transport/ble_gatt.c and transport/serial_cdc.c on
 * why each of them dispatches from a task of its own. */
void cmd_lock_init(void);
void cmd_lock_acquire(void);
void cmd_lock_release(void);

#endif
