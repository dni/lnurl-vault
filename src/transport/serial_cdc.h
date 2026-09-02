#ifndef LNURLVAULT_SERIAL_CDC_H
#define LNURLVAULT_SERIAL_CDC_H

#include "dispatcher.h" /* transport_drops_t */

/* WebSerial transport: the ESP32-S3's native USB presents as a USB-CDC
 * device (via TinyUSB), which `navigator.serial` on the browser side can
 * open directly — no separate USB-UART bridge chip, no drivers. Wire format
 * is newline-delimited JSON (see docs/PROTOCOL.md): one command object per
 * line in, one response object per line out. */
void serial_cdc_start(void);

/* Fills in what this transport has thrown away since boot -- see
 * dispatcher.h's transport_drops_fn for why anyone needs to know. Every drop
 * site below also logs, but on this board the log goes to UART0 and the host
 * is on the USB-C cable, so the log is exactly the thing a remote report
 * cannot include. */
void serial_cdc_drops(transport_drops_t *out);

/* Fills in what the USB bus itself has done since boot -- see dispatcher.h's
 * usb_link_fn. drops is what this file gave up on; this is what the host and
 * the controller did around it, which a report of "it keeps disconnecting"
 * cannot otherwise distinguish from an app that closed the port. */
void serial_cdc_usb_link(usb_link_t *out);

#endif
