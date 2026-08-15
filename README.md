# lnurl-vault

**LNURLvault** — an ESP32-S3 hardware vault for [LNURLcash](../luds/25.md) bearer notes — the
offline "banknote" scheme [lnurl-wallet](../lnurl-wallet) and
[lnurl-mint](../lnurl-mint) implement on top of plain LUD-03
`withdrawRequest`. The device generates note secrets from a hardware RNG,
discloses only their SHA-256 hash until a mint confirms a rotate/split/merge
succeeded, stores notes through their `pending → confirmed → spent`
lifecycle, and gates every plaintext-secret export behind a physical
button press. It talks to a paired browser session over WebSerial (native
USB-CDC) or BLE — see [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the wire
protocol.

Browser-side integration into `lnurl-wallet` (a `device.ts` client, pairing
UI) is a deliberate follow-up, not part of this repo yet — this is the
firmware and its protocol, designed to be consumed by that later.

## Status: unverified by compilation

This project was built in a sandbox with **no ESP-IDF/PlatformIO install and
no ESP32-S3 board attached** (`command -v idf.py` / `platformio` /
`arduino-cli` all came back empty). Two different confidence levels apply:

- **`src/vault/`, `src/proto/`** (the note state machine, SHA-256, hex, and
  JSON reader/writer, and the command dispatcher): pure portable C, no
  ESP-IDF dependency, exercised by `test/native/` — **93/93 assertions
  actually pass**, run with plain `gcc` in this environment (see
  Verification below). This is the security- and protocol-critical logic.
- **`src/storage/`, `src/transport/`, `src/ui/`, `src/main.c`**: ESP-IDF
  glue (NVS, TinyUSB CDC, NimBLE, esp_lcd, GPIO) written against documented
  APIs but never compiled here. Each file's header comment flags exactly
  what's most likely to need reconciling against your installed IDF
  version — NimBLE bring-up (`ble_gatt.c`) is the highest-risk one, NVS
  encryption setup (`nvs_storage.c`) and esp_lcd struct field names
  (`display.c`) the next most likely. Treat these as a strong, structured
  starting point to debug against real hardware, not verified-working
  firmware.

## Architecture

```
src/
  vault/     note state machine (PENDING/CONFIRMED/SPENT), SHA-256, hex —
             portable, no ESP-IDF dependency
  proto/     JSON reader/writer + the transport-agnostic command dispatcher
  storage/   NVS-backed vault_storage_t implementation (ESP-IDF only)
  transport/ serial_cdc.c (USB-CDC/WebSerial), ble_gatt.c (NimBLE)
  ui/        ST7789 display + button confirm/cancel gating (T-Display S3)
  main.c     wires RNG, storage, transports, and UI together
test/native/ unit tests for src/vault + src/proto, plain gcc, no hardware
docs/PROTOCOL.md  full wire protocol reference
```

The design principle: **the device is the only thing that ever holds a
note's plaintext secret**, except at the moment a client deliberately
exports it (to spend, hand over, or display as a note). Every mutation
(rotate/split/merge) is a two-phase commit — the device generates the new
secret and discloses only its hash; nothing is finalized until the client
reports the mint accepted it. See [`docs/PROTOCOL.md`](docs/PROTOCOL.md)'s
"Orchestration" section for exactly how a client sequences this.

The device does no networking of its own — no Wi-Fi driver is even built in
(see `sdkconfig.defaults`). All mint HTTP calls are the paired browser's
job; the device's only interface to the outside world is the USB-CDC or BLE
session itself.

## Build & flash

Requires [PlatformIO](https://platformio.org/) (not installed in this
environment — `pip install platformio`), which resolves the ESP-IDF
toolchain itself on first build.

```sh
pio run -e t-display-s3 -t upload
pio device monitor
```

Board: [LilyGo T-Display S3](https://github.com/Xinyuan-LilyGO/T-Display-S3)
(ESP32-S3R8, 170×320 ST7789 LCD, 2 buttons). **Verify `src/ui/board_pins.h`
against your specific board revision** using LilyGo's own example repo
before flashing — see that file's header comment.

If `pio run` fails on a specific symbol (an NVS-encryption call, a NimBLE
header, an esp_lcd struct field), that's expected per the Status section
above — the failing file's own top comment says which ESP-IDF version
change is the likely cause and what to check against.

## Security posture

- **Secrets** come from `esp_fill_random()` (the ESP32-S3's hardware TRNG).
  Espressif documents its full-entropy guarantee as conditional on Wi-Fi or
  BT having been active at least once — `main.c` starts BLE before the
  first RNG self-test or secret generation to satisfy that. A cheap startup
  self-test (two 16-byte draws must differ and not be all-zero) guards
  against a catastrophically stuck RNG; it is *not* a statistical
  randomness test suite.
- **Storage**: NVS encryption is enabled by default
  (`CONFIG_NVS_ENCRYPTION=y`, keys in the dedicated `nvs_keys` partition —
  see `partitions.csv`). This protects against casual flash dumping only.
  **Real protection against physical extraction requires provisioning Flash
  Encryption + Secure Boot V2** — a deliberate, irreversible (eFuse-burning)
  manual step, intentionally left to you before this device ever holds
  value you care about.
- **No networking on-device.** The only attack surface is the paired
  USB-CDC or BLE session. All mint calls are the browser's responsibility.
- **`export_secret`** — the only command that ever discloses a plaintext
  secret — blocks on a physical button press with a 30s timeout
  (`src/ui/buttons.c`), requiring both buttons to be released first so a
  button already held from whatever triggered the prompt can't
  auto-confirm it.

## Known limitations / next steps

- **No on-screen note detail.** The confirm/cancel gate is fully functional
  (a real button press is required before any secret is exported), but the
  display currently shows only a full-screen color per state (idle / confirm
  pending / approved / declined), not the note's id/amount/label — a hand-
  transcribed bitmap font couldn't be visually verified without hardware in
  this environment, and a silently-wrong glyph felt like the wrong risk to
  ship. Adding real text (most likely via LVGL) is the natural next step,
  particularly before relying on this for anything beyond bench testing.
- **No OTA**, single factory app partition — see `partitions.csv`.
- **No `lnurl-wallet` integration yet** — see the top of this file.
- BLE pairing/bonding is unauthenticated in this v1 (any nearby device can
  connect and issue commands, though it still can't extract a secret
  without a physical button press on the vault itself). Consider BLE
  bonding/passkey pairing before relying on BLE outside a trusted room.

## Verification

**Native unit tests** (portable core, no hardware, no PlatformIO needed):

```sh
cd test/native && make test
```

This actually runs in this environment — 93 assertions across SHA-256 known-
answer vectors, JSON reader/writer round-trips (including escaping and the
overflow-detection path), and the full vault state machine: legal and
illegal state transitions, split/merge parent lineage, the id-collision
retry path, and a simulated-reboot persistence round-trip against a fake
storage backend satisfying the same `vault_storage_t` interface
`src/storage/nvs_storage.c` implements for real.

**Hardware verification** (needs a flashed board — not possible in this
environment):

1. `pio run -e t-display-s3 -t upload && pio device monitor`
2. Exercise the command set by hand, typing JSON lines directly into the
   serial monitor: `new_secret` → `confirm` → `list_notes` → `export_secret`
   (press the confirm button when the display goes amber) → `mark_spent` →
   `list_notes`.
3. Separately validate BLE: connect with a generic GATT explorer (e.g. nRF
   Connect), write a JSON command chunked per `docs/PROTOCOL.md`'s framing
   (including a message deliberately larger than one MTU, to exercise
   reassembly), and confirm the notified response reassembles correctly.
