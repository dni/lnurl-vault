/* Confirmed to compile as part of a full firmware build against ESP-IDF
 * 6.0.1 (see README.md's "Status" section). Standard FreeRTOS mutex
 * semaphore APIs, stable across ESP-IDF releases. */
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

bool vault_lock_try_acquire(void) {
    return xSemaphoreTake(g_mutex, 0) == pdTRUE;
}

void vault_lock_release(void) {
    xSemaphoreGive(g_mutex);
}
