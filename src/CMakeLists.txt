# Main component for the lnurl-vault firmware.
#
# NOTE: unverified by compilation (see README.md) — reconcile REQUIRES against
# actual build errors on your installed ESP-IDF version if any component name
# has changed.
idf_component_register(
    SRC_DIRS "." "vault" "proto" "storage" "transport" "ui"
    INCLUDE_DIRS "." "vault" "proto" "storage" "transport" "ui"
    REQUIRES nvs_flash esp_lcd driver bt esp_hw_support esp_timer esp_tinyusb esp_partition
)
