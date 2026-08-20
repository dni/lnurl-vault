/* Host stand-in for ESP-IDF's esp_heap_caps.h. The capability bits mean
 * nothing off-chip; what matters is that display.c's row-buffer allocation
 * succeeds so display_ready() is true.
 *
 * Backed by a static pool in hostgfx.c rather than malloc, and deliberately:
 * display_init() allocates its sixteen row buffers into a module-static array
 * and never frees them, so a second call from a second panel geometry strands
 * the first sixteen. Under CI's ASan that is sixteen reported leaks in code
 * that has no leak on the device -- display_init() runs exactly once there.
 * A pool the test harness rewinds keeps the firmware honest and the harness
 * quiet. */
#ifndef LNURLVAULT_HOSTGFX_ESP_HEAP_CAPS_H
#define LNURLVAULT_HOSTGFX_ESP_HEAP_CAPS_H

#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_DMA 0

void *heap_caps_malloc(size_t size, uint32_t caps);

#endif
