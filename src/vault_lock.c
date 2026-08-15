/* NOTE: unverified by compilation (see README.md). Standard FreeRTOS mutex
 * semaphore APIs, stable across ESP-IDF releases — low risk relative to the
 * rest of this project's ESP-IDF glue. */
#include "vault_lock.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t g_mutex;

void vault_lock_init(void) {
    g_mutex = xSemaphoreCreateMutex();
}

void vault_lock_acquire(void) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
}

void vault_lock_release(void) {
    xSemaphoreGive(g_mutex);
}
