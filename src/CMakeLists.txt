# Main component for the lnurl-vault firmware.
#
# Board-specific bring-up lives in board/ (one file per board, see board.h);
# everything above it is board-agnostic.
idf_component_register(
    SRC_DIRS "." "board" "vault" "proto" "storage" "transport" "ui"
    INCLUDE_DIRS "." "board" "vault" "proto" "storage" "transport" "ui"
    REQUIRES nvs_flash esp_lcd driver bt esp_hw_support esp_timer esp_tinyusb esp_partition freertos
)
