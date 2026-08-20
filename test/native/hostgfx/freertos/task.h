/* Host stand-in for FreeRTOS task.h.
 *
 * vTaskDelay is a no-op rather than a sleep: display_flash_count() would
 * otherwise make the test suite sit there for six seconds, and what is being
 * checked is what lands in the framebuffer, not how long it stayed there. */
#ifndef LNURLVAULT_HOSTGFX_TASK_H
#define LNURLVAULT_HOSTGFX_TASK_H

static inline void vTaskDelay(unsigned int ticks) {
    (void)ticks;
}

#endif
