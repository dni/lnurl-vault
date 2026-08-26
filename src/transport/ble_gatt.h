#ifndef LNURLVAULT_BLE_GATT_H
#define LNURLVAULT_BLE_GATT_H

#include "dispatcher.h" /* transport_drops_t */

/* BLE transport (NimBLE GATT server). See docs/PROTOCOL.md for the service/
 * characteristic UUIDs and the chunked framing used to carry a JSON message
 * across GATT's small MTU. */
void ble_gatt_start(void);

/* Fills in what this link has thrown away since boot -- see dispatcher.h's
 * transport_drops_fn. A central that loses its notify subscription mid-answer
 * is the common case here, and it looks exactly like a device that stopped
 * talking. */
void ble_gatt_drops(transport_drops_t *out);

#endif
