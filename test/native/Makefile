# Builds and runs the portable core's unit tests with the host's plain gcc —
# no PlatformIO, no ESP-IDF, no board required. See README.md's Verification
# section.
CC ?= gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -I../../src/vault -I../../src/proto -I.
SRC = ../../src/vault/sha256.c \
      ../../src/vault/hex.c \
      ../../src/vault/vault.c \
      ../../src/proto/json.c \
      ../../src/proto/dispatcher.c \
      ../../src/proto/button_fsm.c \
      ../../src/proto/note_url.c \
      test_sha256.c \
      test_hex.c \
      test_json.c \
      test_vault.c \
      test_dispatcher.c \
      test_button_fsm.c \
      test_note_url.c \
      test_regressions.c \
      main.c
BIN = vault_tests

# Headers are prerequisites too: without them, editing a .h leaves a stale
# binary in place and the next `make test` silently reports the old result.
HDR = $(wildcard ../../src/vault/*.h ../../src/proto/*.h *.h)

.PHONY: test clean
test: $(BIN)
	./$(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

clean:
	rm -f $(BIN)
