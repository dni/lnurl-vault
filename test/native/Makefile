# Builds and runs the portable core's unit tests with the host's plain gcc —
# no PlatformIO, no ESP-IDF, no board required. See README.md's Verification
# section.
#
# json.c links against the system cJSON library (same upstream version the
# firmware pulls via src/idf_component.yml's espressif/cjson managed
# component). Install it first: `pacman -S cjson` (Arch) or
# `apt-get install libcjson-dev` (Debian/Ubuntu, incl. CI).
CC ?= gcc
# Split deliberately: GNU ld resolves symbols left to right, so a library
# named before the objects that reference it contributes nothing and the link
# fails with "undefined reference". Apple's linker does not care about order,
# so putting --libs in CFLAGS builds fine on macOS and fails only on CI --
# which is exactly how it reached main.
CJSON_CFLAGS := $(shell pkg-config --cflags libcjson)
CJSON_LIBS := $(shell pkg-config --libs libcjson)
CFLAGS = -std=c11 -Wall -Wextra -Werror -I../../src/vault -I../../src/proto -I../../src/ota -I../../src/ui -I. $(CJSON_CFLAGS)
SRC = ../../src/vault/sha256.c \
      ../../src/vault/hex.c \
      ../../src/vault/vault.c \
      ../../src/proto/json.c \
      ../../src/proto/base64.c \
      ../../src/proto/dispatcher.c \
      ../../src/proto/button_fsm.c \
      ../../src/proto/note_url.c \
      ../../src/proto/ble_frame.c \
      ../../src/ui/qr_capacity.c \
      ../../src/ota/monocypher.c \
      ../../src/ota/monocypher-ed25519.c \
      ../../src/ota/ota_sign.c \
      test_sha256.c \
      test_hex.c \
      test_json.c \
      test_vault.c \
      test_dispatcher.c \
      test_button_fsm.c \
      test_note_url.c \
      test_ble_frame.c \
      test_qr_capacity.c \
      test_wipe.c \
      test_regressions.c \
      test_base64.c \
      test_ota_sign.c \
      test_ota_dispatch.c \
      main.c
BIN = vault_tests

# Headers are prerequisites too: without them, editing a .h leaves a stale
# binary in place and the next `make test` silently reports the old result.
HDR = $(wildcard ../../src/vault/*.h ../../src/proto/*.h *.h)

.PHONY: test clean
test: $(BIN)
	./$(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(CJSON_LIBS)

clean:
	rm -f $(BIN)
