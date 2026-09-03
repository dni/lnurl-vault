/* Confirmed to compile as part of a full firmware build against ESP-IDF
 * 6.0.1 (see README.md's "Status" section). Standard FreeRTOS mutex
 * semaphore APIs, stable across ESP-IDF releases. */
#include "vault_lock.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "vault_lock";

/* How long a wait has to run before it is worth saying so. Every critical
 * section this lock protects is a handful of vault_* calls over an in-RAM
 * array, so microseconds; a wait measured in seconds means somebody is
 * holding it across something they were told not to (see vault_lock.h's
 * rule), and the device is wedged rather than busy. */
#define VAULT_LOCK_WARN_MS 5000

static SemaphoreHandle_t g_mutex;

void vault_lock_init(void) {
    g_mutex = xSemaphoreCreateMutex();
}

/* Still waits forever, deliberately: every caller treats acquiring as
 * infallible and there is no correct thing for a vault_* caller to do with a
 * failure, so giving up would trade a visible hang for a silent skipped
 * write on a device holding money. What changed is that it no longer waits
 * SILENTLY. A single portMAX_DELAY take is indistinguishable from a crash
 * from the outside: the device stops answering, on every transport at once,
 * and nothing is logged because nothing failed. That cost a long a field
 * debugging session in which "it stopped responding" could not be told apart
 * from "it rebooted" or "the link died". Now a task starving on this lock
 * names itself once every VAULT_LOCK_WARN_MS on the console, and the boot
 * log says which task and for how long. */
void vault_lock_acquire(void) {
    TickType_t waited = 0;
    while (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(VAULT_LOCK_WARN_MS)) != pdTRUE) {
        waited += pdMS_TO_TICKS(VAULT_LOCK_WARN_MS);
        ESP_LOGE(TAG, "task '%s' has waited %ums for vault_lock -- still held by "
                      "another task; something is holding it across a wait it "
                      "should not (see vault_lock.h)",
                 pcTaskGetName(NULL), (unsigned)pdTICKS_TO_MS(waited));
    }
}

bool vault_lock_try_acquire(void) {
    return xSemaphoreTake(g_mutex, 0) == pdTRUE;
}

void vault_lock_release(void) {
    xSemaphoreGive(g_mutex);
}
