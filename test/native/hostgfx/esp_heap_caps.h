/* Host stand-in. Backed by a static pool in hostgfx.c, not malloc:
 * display_init() never frees its sixteen row buffers, which is right on a
 * device that calls it once and sixteen ASan leaks in a harness that calls it
 * per geometry. */
#ifndef LNURLVAULT_HOSTGFX_ESP_HEAP_CAPS_H
#define LNURLVAULT_HOSTGFX_ESP_HEAP_CAPS_H

#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_DMA 0

void *heap_caps_malloc(size_t size, uint32_t caps);

#endif
