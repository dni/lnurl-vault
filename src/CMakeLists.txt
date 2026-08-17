# Main component for the lnurl-vault firmware.
#
# Confirmed by actual `pio run -e t-display-s3` builds against ESP-IDF 6.0.1
# (see README.md's Status section) — reconcile REQUIRES against actual build
# errors on a different installed ESP-IDF version if any component name has
# changed there.
idf_component_register(
    SRC_DIRS "." "board" "vault" "proto" "storage" "transport" "ui" "ota"
    INCLUDE_DIRS "." "board" "vault" "proto" "storage" "transport" "ui" "ota"
    REQUIRES nvs_flash esp_lcd driver bt esp_hw_support esp_timer esp_tinyusb esp_partition app_update freertos
)
