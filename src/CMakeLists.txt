# Main component for the lnurl-vault firmware.
#
# Board-specific bring-up lives in board/ (one file per board, see board.h);
# everything above it is board-agnostic. Every board's source is compiled for
# every target -- each file guards itself on the LNURLVAULT_BOARD_* flag its
# platformio.ini environment defines -- so this list does not vary per board.
set(LNURLVAULT_REQUIRES
    nvs_flash
    esp_lcd
    driver
    bt
    esp_hw_support
    esp_timer
    esp_partition
    app_update
    freertos)

# esp_tinyusb exists only for targets with a USB-OTG peripheral, and
# src/idf_component.yml declines to fetch it elsewhere. The classic ESP32
# reaches the host through an external USB-UART bridge instead (see
# src/transport/serial_uart.c), so requiring it there fails the build outright.
if(IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32s2" OR IDF_TARGET STREQUAL "esp32p4")
    list(APPEND LNURLVAULT_REQUIRES esp_tinyusb)
endif()

idf_component_register(
    SRC_DIRS "." "board" "vault" "proto" "storage" "transport" "ui" "ota"
    INCLUDE_DIRS "." "board" "vault" "proto" "storage" "transport" "ui" "ota"
    REQUIRES ${LNURLVAULT_REQUIRES}
)
