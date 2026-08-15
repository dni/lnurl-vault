#ifndef LNURLVAULT_SERIAL_CDC_H
#define LNURLVAULT_SERIAL_CDC_H

/* WebSerial transport: the ESP32-S3's native USB presents as a USB-CDC
 * device (via TinyUSB), which `navigator.serial` on the browser side can
 * open directly — no separate USB-UART bridge chip, no drivers. Wire format
 * is newline-delimited JSON (see docs/PROTOCOL.md): one command object per
 * line in, one response object per line out. */
void serial_cdc_start(void);

#endif
