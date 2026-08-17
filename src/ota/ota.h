#ifndef LNURLVAULT_OTA_H
#define LNURLVAULT_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ESP-IDF glue implementing dispatcher.h's ota_write_begin_fn/
 * ota_write_chunk_fn/ota_write_finish_fn/ota_write_abort_fn contract via
 * esp_ota_ops.h against the inactive ota_0/ota_1 partition (see
 * partitions.csv). Session state here (the esp_ota_handle_t and target
 * partition) is ESP-IDF-specific and deliberately separate from
 * dispatcher.c's own g_ota, which owns the portable parsing/signature/
 * sequencing state and is what test/native/ exercises against a fake
 * in-memory "flash" instead of these functions. */
bool ota_write_begin(uint32_t total_size);
bool ota_write_chunk(const uint8_t *data, size_t len);
bool ota_write_finish(void);
void ota_write_abort(void);

#endif
