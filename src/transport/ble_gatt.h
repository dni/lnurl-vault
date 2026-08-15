#ifndef LNURLVAULT_BLE_GATT_H
#define LNURLVAULT_BLE_GATT_H

/* BLE transport (NimBLE GATT server). See docs/PROTOCOL.md for the service/
 * characteristic UUIDs and the chunked framing used to carry a JSON message
 * across GATT's small MTU. */
void ble_gatt_start(void);

#endif
