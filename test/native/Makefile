# Builds and runs the portable core's unit tests with the host's plain gcc —
# no PlatformIO, no ESP-IDF, no board required. See README.md's Verification
# section.
#
# json.c links against the system cJSON library (same upstream version the
# firmware pulls via src/idf_component.yml's espressif/cjson managed
# component). Install it first: `pacman -S cjson` (Arch) or
# `apt-get install libcjson-dev` (Debian/Ubuntu, incl. CI).
# CI runs this with GNU gcc on Linux. On macOS `gcc` is Apple clang, which
# does NOT diagnose everything gcc does under -Werror -- and that difference
# has broken main twice:
#
#   - -lcjson placed before the objects that reference it. GNU ld resolves
#     left to right and fails; Apple's linker is order-independent and does
#     not care. (Fixed by splitting CJSON_CFLAGS from CJSON_LIBS below.)
#   - -Werror=format-truncation on an snprintf whose output could not fit.
#     gcc diagnoses it; clang has no such warning at all.
#
# So a green run on macOS is NOT evidence of a green run on CI. Install a real
# gcc (`brew install gcc`) and check with it before claiming a branch is
# green:
#
#     CC=gcc-16 make test
CC ?= gcc
# Split deliberately: GNU ld resolves symbols left to right, so a library
# named before the objects that reference it contributes nothing and the link
# fails with "undefined reference". Apple's linker does not care about order,
# so putting --libs in CFLAGS builds fine on macOS and fails only on CI --
# which is exactly how it reached main.
CJSON_CFLAGS := $(shell pkg-config --cflags libcjson)
CJSON_LIBS := $(shell pkg-config --libs libcjson)
CFLAGS = -std=c11 -Wall -Wextra -Werror -I../../src/vault -I../../src/proto -I../../src/ota -I../../src/ui -I../../src/board -Ihostgfx -I. $(CJSON_CFLAGS)
SRC = ../../src/vault/sha256.c \
      ../../src/vault/hex.c \
      ../../src/vault/entropy.c \
      ../../src/vault/vault.c \
      ../../src/vault/identity.c \
      ../../src/proto/json.c \
      ../../src/proto/base64.c \
      ../../src/proto/dispatcher.c \
      ../../src/proto/button_fsm.c \
      ../../src/proto/note_url.c \
      ../../src/proto/ble_frame.c \
      ../../src/proto/note_display.c \
      ../../src/ui/font5x7.c \
      ../../src/ui/display.c \
      hostgfx/hostgfx.c \
      ../../src/proto/line_proto.c \
      ../../src/proto/approval.c \
      ../../src/proto/input_health.c \
      ../../src/ui/qr_capacity.c \
      ../../src/ota/monocypher.c \
      ../../src/ota/monocypher-ed25519.c \
      ../../src/ota/ota_sign.c \
      test_sha256.c \
      test_hex.c \
      test_entropy.c \
      test_json.c \
      test_vault.c \
      test_dispatcher.c \
      test_secret_leak.c \
      test_response_bounds.c \
      test_button_fsm.c \
      test_note_url.c \
      test_ble_frame.c \
      test_note_display.c \
      test_card_render.c \
      test_line_proto.c \
      test_approval.c \
      test_input_health.c \
      test_identity.c \
      test_qr_capacity.c \
      test_wipe.c \
      test_gated_actions.c \
      test_list_paging.c \
      test_regressions.c \
      test_base64.c \
      test_ota_sign.c \
      test_ota_dispatch.c \
      main.c
BIN = vault_tests

# Headers are prerequisites too: without them, editing a .h leaves a stale
# binary in place and the next `make test` silently reports the old result.
HDR = $(wildcard ../../src/vault/*.h ../../src/proto/*.h ../../src/ui/*.h hostgfx/*.h hostgfx/freertos/*.h *.h)

# Address + Undefined Behaviour sanitizers over the same suite. Near-free and
# high-yield in a codebase built on fixed buffers and memcpy; CI runs this as a
# separate job. Built as a distinct binary so it never links stale objects from
# the plain build, and -fno-sanitize-recover=all makes any finding a non-zero
# exit so CI actually fails on it.
SAN_BIN = vault_tests_san
SAN_FLAGS = -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1

# Renders every screen to a PNG, at both panel geometries, from the same
# src/ui/display.c the firmware runs. See hostgfx/hostgfx.h.
PREVIEW_BIN = vault_preview
PREVIEW_SRC = ../../src/ui/display.c ../../src/ui/font5x7.c ../../src/proto/note_display.c \
              hostgfx/hostgfx.c preview.c
PREVIEW_OUT ?= preview
PREVIEW_ZOOM ?= 3

.PHONY: test test-sanitize preview clean
test: $(BIN)
	./$(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(CJSON_LIBS)

test-sanitize: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SAN_FLAGS) -o $(SAN_BIN) $(SRC) $(CJSON_LIBS)
	./$(SAN_BIN)

preview: $(PREVIEW_BIN)
	./$(PREVIEW_BIN) $(PREVIEW_OUT) $(PREVIEW_ZOOM)

$(PREVIEW_BIN): $(PREVIEW_SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(PREVIEW_BIN) $(PREVIEW_SRC)

clean:
	rm -f $(BIN) $(SAN_BIN) $(PREVIEW_BIN)
	rm -rf $(PREVIEW_OUT)
