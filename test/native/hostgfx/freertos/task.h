/* Host stand-in. vTaskDelay is a no-op: display_flash_count() would otherwise
 * cost the suite six seconds, and what is checked is what lands in the
 * framebuffer, not how long it stayed. */
#ifndef LNURLVAULT_HOSTGFX_TASK_H
#define LNURLVAULT_HOSTGFX_TASK_H

static inline void vTaskDelay(unsigned int ticks) {
    (void)ticks;
}

#endif
