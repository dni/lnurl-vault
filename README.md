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

It also works fully offline, with nothing paired at all: tap either button
to browse `CONFIRMED` notes, hold both together for ~200ms to unveil the
selected note's `lnurlw://` URL as an on-screen QR code — the offline
banknote handoff [LUD-25](../luds/25.md) itself describes. See
[`docs/PROTOCOL.md`](docs/PROTOCOL.md)'s "On-device note browsing" section.

Browser-side integration into `lnurl-wallet` (a `device.ts` client, pairing
UI) is a deliberate follow-up, not part of this repo yet — this is the
firmware and its protocol, designed to be consumed by that later.

## Status: unverified by compilation

This project was built in a sandbox with **no ESP-IDF/PlatformIO install and
no ESP32-S3 board attached** (`command -v idf.py` / `platformio` /
`arduino-cli` all came back empty). Two different confidence levels apply:

- **`src/vault/`, `src/proto/`** (the note state machine, SHA-256, hex, JSON
  reader/writer, command dispatcher, button gesture state machine, and
  `lnurlw://` URL builder): pure portable C, no ESP-IDF dependency,
  exercised by `test/native/` — **112/112 assertions actually pass**, run
  with plain `gcc` in this environment (see Verification below). This is
  the security- and protocol-critical logic, including the debounce/tap/
  chord logic gating every plaintext-secret disclosure.
- **`src/storage/`, `src/transport/`, `src/ui/`, `src/main.c`,
  `src/vault_lock.c`**: ESP-IDF glue (NVS, TinyUSB CDC, NimBLE, esp_lcd,
  GPIO, FreeRTOS mutex/queue) written against documented APIs but never
  compiled here. Each file's header comment flags exactly what's most
  likely to need reconciling against your installed IDF version — NimBLE
  bring-up (`ble_gatt.c`) is the highest-risk one, NVS encryption setup
  (`nvs_storage.c`) and esp_lcd struct field names (`display.c`) the next
  most likely. Treat these as a strong, structured starting point to debug
  against real hardware, not verified-working firmware.
- **`src/ui/qr_display.c`** carries an additional risk beyond "unverified":
  it depends on a third-party QR encoder **not included in this repo** —
  see that file's header comment and "Build & flash" below.

## Architecture

```
src/
  vault/     note state machine (PENDING/CONFIRMED/SPENT), SHA-256, hex —
             portable, no ESP-IDF dependency
  proto/     JSON reader/writer, the transport-agnostic command dispatcher,
             the button gesture state machine, the lnurlw:// URL builder —
             all portable, no ESP-IDF dependency
  storage/   NVS-backed vault_storage_t implementation (ESP-IDF only)
  transport/ serial_cdc.c (USB-CDC/WebSerial), ble_gatt.c (NimBLE) — both
             take vault_lock.h's mutex around each dispatcher_handle() call
  ui/        ST7789 display, buttons.c (thin GPIO adapter over
             proto/button_fsm.c), qr_display.c (needs a vendored QR
             library, see Build & flash), and ui_task.c — the single task
             owning buttons+display for both local note browsing and
             remote export_secret confirm requests (T-Display S3)
  vault_lock.c  the mutex serializing vault.c access between the transport
                task and ui_task (ESP-IDF only, see its header comment)
  main.c     wires RNG, storage, transports, and UI together
test/native/ unit tests for src/vault + src/proto, plain gcc, no hardware
docs/PROTOCOL.md  full wire protocol + on-device browsing reference
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

**Before the first build**, vendor the QR encoder `qr_display.c` needs —
not included in this repo (see its header comment for why): download
`qrcode.h` and `qrcode.c` from
[ricmoo/QRCode](https://github.com/ricmoo/QRCode) (MIT) and drop them
directly into `src/ui/` (flat, no subdirectory).

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

## Releases & web installer

Pushing a tag (`git tag v0.1.0 && git push origin v0.1.0`) triggers
[`.github/workflows/release.yml`](.github/workflows/release.yml), which:

1. Builds the firmware with PlatformIO (same as "Build & flash" above).
2. Publishes a GitHub Release for the tag with four assets: `bootloader.bin`,
   `partitions.bin`, `firmware.bin`, and an [esp-web-tools](https://esphome.github.io/esp-web-tools/)
   `manifest.json` describing where each one flashes (offsets must match
   `partitions.csv` — the workflow's own comments spell out why each value
   is what it is).
3. Regenerates and deploys **[`webinstaller/`](webinstaller/)** to GitHub
   Pages: a page that lists every published release (fetched via `gh
   release list`, no server needed beyond static Pages hosting) and lets a
   visitor flash any of them directly from Chrome/Edge over Web Serial —
   no PlatformIO, no command line, just a USB cable. Every release's
   binaries are copied into the deployed site itself (`firmware/<tag>/`)
   rather than linked cross-origin to GitHub's release CDN, which doesn't
   serve CORS headers `fetch()` would need — see the workflow's "Fetch
   every release's firmware assets" step.

That last step also runs standalone (`workflow_dispatch`, no new tag
needed) — useful for updating `webinstaller/index.html` itself, or after
manually deleting a bad release, without cutting a new firmware version.

**One-time manual setup this workflow can't do for you**: Settings → Pages
→ Source: "GitHub Actions" — the `github.io` URL won't serve anything until
that's set. The web installer page currently hardcodes this repo as
`dni/lnurl-vault` in a couple of "view source" links.

Like the rest of the ESP-IDF-specific parts of this project, this workflow
has never actually run (no GitHub remote was configured for this repo when
it was written) — reconcile `.pio/build/t-display-s3/`'s actual output
filenames against the "Collect build outputs" step if PlatformIO's ESP-IDF
integration names them differently than expected there.

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
- **`export_secret` over serial/BLE** — the only remote command that ever
  discloses a plaintext secret — is gated by `ui_task.c`, requiring a
  physical tap of one button (confirm) or the other (cancel) within a 30s
  timeout, using the same debounce/gesture logic (`src/proto/button_fsm.c`)
  that also drives on-device browsing — see Verification for how that's
  tested.
- **The on-device browse-and-unveil gesture** (see
  [`docs/PROTOCOL.md`](docs/PROTOCOL.md)) is a second, independent path to
  a plaintext secret, gated differently: holding both buttons together for
  ~200ms *is* the confirmation (reaching that point already required
  physical possession of the device), rather than a separate confirm/cancel
  step. Once shown, a QR code **is** the bearer secret in the clear — that's
  the point, it's meant to be scanned and redeemed by whoever it's shown
  to, exactly like handing over a banknote.
- **`vault.c` has no locking of its own** (it's shared with the native test
  binary, which is single-threaded) — on firmware, `vault_lock.c` serializes
  every call to it between the active transport's task and `ui_task.c`. See
  that file's header comment for the design and why holding the lock for up
  to 30s during a pending remote confirm is safe (ui_task isn't trying to
  touch vault state locally during that same window).

## Known limitations / next steps

- **No on-screen note detail.** Both confirm/cancel gating and on-device
  browsing are fully functional (a real button gesture is required before
  any secret is ever shown), but the display shows only a full-screen color
  per state and, while browsing, a blinked-out 1-based position — never a
  note's actual id/amount/label — a hand-transcribed bitmap font couldn't be
  visually verified without hardware in this environment, and a silently-
  wrong glyph felt like the wrong risk to ship. Adding real text (most
  likely via LVGL) is the natural next step, particularly before relying on
  this for anything beyond bench testing — right now, browsing to the wrong
  note and unveiling it is a real, unmitigated risk of this v1.
- **The QR encoder is a required external dependency**, not included in
  this repo — see "Build & flash" above. Nothing in `src/ui/qr_display.c`
  will compile until it's vendored.
- **No OTA**, single factory app partition — see `partitions.csv`.
- **No `lnurl-wallet` integration yet** — see the top of this file.
- BLE pairing/bonding is unauthenticated in this v1 (any nearby device can
  connect and issue commands, though it still can't extract a secret
  without a physical gesture on the vault itself). Consider BLE
  bonding/passkey pairing before relying on BLE outside a trusted room.

## Verification

**Native unit tests** (portable core, no hardware, no PlatformIO needed):

```sh
cd test/native && make test
```

This actually runs in this environment — 112 assertions across SHA-256
known-answer vectors, JSON reader/writer round-trips (including escaping and
the overflow-detection path), the full vault state machine (legal and
illegal state transitions, split/merge parent lineage, the id-collision
retry path, and a simulated-reboot persistence round-trip against a fake
storage backend satisfying the same `vault_storage_t` interface
`src/storage/nvs_storage.c` implements for real), the `lnurlw://` URL
builder, and the button gesture state machine (debounce/bounce filtering,
simple taps, a chord that fires exactly once and doesn't also emit trailing
taps, and staggered chord entry).

**Hardware verification** (needs a flashed board — not possible in this
environment):

1. `pio run -e t-display-s3 -t upload && pio device monitor` (after
   vendoring the QR library, see "Build & flash")
2. Exercise the command set by hand, typing JSON lines directly into the
   serial monitor: `new_secret` → `confirm` → `list_notes` → `export_secret`
   (press the confirm button when the display goes amber) → `mark_spent` →
   `list_notes`.
3. Separately validate BLE: connect with a generic GATT explorer (e.g. nRF
   Connect), write a JSON command chunked per `docs/PROTOCOL.md`'s framing
   (including a message deliberately larger than one MTU, to exercise
   reassembly), and confirm the notified response reassembles correctly.
4. Exercise on-device browsing with at least two `CONFIRMED` notes: tap to
   enter browse mode (watch it blink out position 1), tap again (blinks
   position 2), hold both buttons together (~200ms) to unveil — confirm a
   QR appears, scan it with any phone, and check the decoded URL matches
   `docs/PROTOCOL.md`'s `lnurlw://<host>?k1=...&amount=...` shape for that
   note. Tap once to dismiss and confirm the screen actually clears.
