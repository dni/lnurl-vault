# lnurl-vault

**LNURLvault** — an ESP32-S3 hardware vault for [LNURLcash](https://github.com/lnurl/luds/pull/301) bearer notes — the
offline "banknote" scheme [lnurl-wallet](https://github.com/dni/lnurl-wallet) and
[lnurl-mint](https://github.com/dni/lnurl-mint) implement on top of plain LUD-03
`withdrawRequest`. The device generates note secrets from a hardware RNG,
discloses only their SHA-256 hash until a mint confirms a rotate/split/merge
succeeded, stores notes through their `pending → confirmed → spent`
lifecycle, and gates every plaintext-secret export behind a physical
button press. It talks to a paired browser session over WebSerial (native
USB-CDC) or BLE — see [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the wire
protocol.

It also works fully offline, with nothing paired at all: tap either button
to browse `CONFIRMED` notes, hold both together for ~200ms to unveil the
selected note as an on-screen QR code — the offline
banknote handoff [LUD-25](https://github.com/lnurl/luds/pull/301) itself describes. See
[`docs/PROTOCOL.md`](docs/PROTOCOL.md)'s "On-device note browsing" section.

Browser-side integration lives in
[lnurl-wallet](https://github.com/dni/lnurl-wallet), not here: `src/device.ts`
(the client for the protocol below), `deviceQueue.ts`, `deviceOrchestration.ts`,
`DeviceContext.tsx` and `pages/Vault.tsx`, with `device.test.ts` and
`deviceOrchestration.test.ts` alongside them. This repo is the firmware and
its wire protocol; that one consumes it.

## Status: builds, flashes, and the serial command protocol is confirmed working on real hardware

This firmware **has been built successfully end to end** — bootloader,
partition table, and `firmware.bin` all produced by a real
`pio run -e t-display-s3` against ESP-IDF 6.0.1 (GCC 15.2.0,
`xtensa-esp-elf` toolchain), 36.3% RAM / 27.2% flash used. That build also
found and fixed several real, non-obvious bugs along the way, worth
knowing about even though they're now fixed:

- **A real flashed board reported backlight-on/black-screen/no-serial-output-
  at-all**, which led to catching the most significant bug of the bunch:
  this board's display is an **8-bit parallel (Intel 8080 / "i80") bus, not
  SPI**. `display.c` used to call the SPI panel-IO driver against a
  MOSI/SCLK pair that was never actually connected to the display — no such
  signals exist on this board at all — confirmed by diffing against
  LilyGo's own `pin_config.h` and ESP-IDF's bundled i80 LCD example. Since
  `display_init()` runs first in `app_main()`, before BLE/storage/our own
  USB-CDC, a bus that hangs or misbehaves talking to the wrong pins fully
  explains why *nothing* ran — not just the screen. Fixed by rewriting
  `display.c` against `esp_lcd_io_i80.h` and correcting `board_pins.h`
  (every other pin — CS/DC/RST/BL/POWER_ON/both buttons — already matched
  LilyGo's config exactly; only the display *bus type* was wrong). A
  reflash after that got the bus genuinely talking to the panel (a
  full-white screen with color glitches, not black) but surfaced two more
  real, confirmed issues: ESP-IDF's generic ST7789 init only sends
  SLPOUT/MADCTL/COLMOD/RAMCTRL, and this exact panel needs additional
  gamma/power/porch tuning beyond that (command bytes transcribed verbatim
  from TFT_eSPI's own T-Display-S3-specific ST7789 init sequence, not
  invented — see `display.c`'s `send_lilygo_t_display_s3_tuning()`); and
  ESP-IDF's ST7789 driver defaults to big-endian pixel data while this
  file writes native little-endian `uint16_t` colors — confirmed by
  reading `esp_lcd_panel_st7789.c` directly, and independently by ESP-IDF's
  own bundled i80/LVGL example needing to manually byte-swap for the exact
  same reason. **A third reflash with both of those fixes still glitched**,
  tracing to a third issue: `fill_screen()` wrote pixel data into a plain
  `static uint16_t[]`, not a buffer allocated via
  `esp_lcd_i80_alloc_draw_buffer()` — which `esp_lcd_io_i80.h`'s own doc
  comment says exists specifically to "handle the alignment required by
  DMA burst, cache line size, etc.", and which ESP-IDF's own bundled i80
  example uses for exactly that reason instead of a plain array. A
  misaligned buffer feeding DMA bursts is a plausible cause of "the bus is
  provably talking to the panel now, but the image is corrupted" on its
  own — independent of the command-sequence and endian fixes above, not a
  replacement for them. **A fourth reflash, with all three of the above
  fixed, was reported as still just a white screen with color glitches** —
  tracing to a fourth, independent issue: this panel is a 170x320 physical
  glass on an ST7789 controller that natively addresses a 240x320 RAM
  window, and `display_init()` never called `esp_lcd_panel_swap_xy()`,
  `esp_lcd_panel_mirror()`, or `esp_lcd_panel_set_gap()` — it drew
  `LCD_WIDTH`x`LCD_HEIGHT` (320x170, landscape) rows straight into the
  controller's default unrotated, zero-gap portrait addressing. Every row
  write went out at the wrong width against the wrong axis and walked into
  adjacent controller RAM — independently plausible as a cause of exactly
  this symptom, and consistent with the bus/tuning/endian/DMA-alignment
  fixes above all being individually correct yet the screen still wrong.
  Fixed with `esp_lcd_panel_swap_xy(panel, true)`,
  `esp_lcd_panel_mirror(panel, true, false)`, and
  `esp_lcd_panel_set_gap(panel, 0, 35)` (`display.c`, right after
  `esp_lcd_panel_init()`) — values confirmed, not guessed, against
  [russhughes/s3lcd](https://github.com/russhughes/s3lcd)'s
  `ROTATIONS_170x320` table (`src/s3lcd.c`), an ESP_LCD-based driver written
  specifically for this panel size, whose `{320, 170, 0, 35, true, true,
  false}` entry (width, height, x_gap, y_gap, swap_xy, mirror_x, mirror_y)
  is the landscape rotation matching this file's `LCD_WIDTH`/`LCD_HEIGHT`.
  **Not yet confirmed against real hardware** — if the image renders upside
  down or mirrored instead of glitched, that table's other 320x170 entry
  (`mirror_x`/`mirror_y` swapped) is the first thing to try; see
  `display.c`'s comment at the `esp_lcd_panel_swap_xy()` call.
- **Reading `vault_lock.h`'s own header comment against what `main.c`
  actually wired up turned up a real, hardware-independent bug**: that
  comment states critical sections holding `vault_lock` are "never
  something like ... the 30s remote-confirm wait," and `serial_cdc.c`'s own
  header separately states nothing may block inside `handle_rx()` since
  TinyUSB invokes it nested inside `tud_task()` — but `main.c`'s
  `confirm_export_on_device()` (wired to `dispatcher.c`'s `export_secret`
  handler) called straight into `ui_task_request_remote_confirm()`'s
  up-to-30s wait *while still holding `vault_lock`*, because both
  transports (`serial_cdc.c`, `ble_gatt.c`) wrap their entire
  `dispatcher_handle()` call in `vault_lock_acquire()`/`release()`. A client
  that sent `export_secret` and never tapped a button stalled the whole
  transport task — for serial, all of TinyUSB, not just this one response —
  and the vault mutex, for up to 30 seconds. Fixed in `main.c`: 
  `confirm_export_on_device()` now releases `vault_lock` before the wait and
  reacquires it after, which is safe because `vault_export_secret()` (called
  right after this returns, still under the reacquired lock) independently
  re-checks the note is still `CONFIRMED` — the same
  re-validate-after-reacquire pattern `ui_task.c`'s own `unveil()` already
  relies on for the equivalent local-browsing race. `ota_begin`'s confirm
  wait (`ota_approve_on_device()`) has the identical problem and is
  **deliberately left unfixed**: `dispatcher.c`'s OTA session state (`g_ota`)
  has no locking of its own — `vault_lock` is today the only thing
  serializing `dispatcher_handle()` calls across transports at all, so
  releasing it there would trade this known, bounded issue for an
  unbounded, unstudied race in an already real-hardware-unverified path. See
  `main.c`'s comment on `ota_approve_on_device()` for what a real fix needs
  (`g_ota` getting its own lock, at minimum).
- A comment in `ble_gatt.c` containing `*/` mid-sentence closed early,
  corrupting everything parsed after it into garbage compile errors.
- `BLE_UUID128_INIT(...)` was assigned directly to a `.uuid` field, which
  wants a `const ble_uuid_t *` pointer, not the macro's brace-initializer
  value — fixed by giving each UUID its own named variable and taking
  `&var.u`.
- `ble_gatt_start()` called a `esp_nimble_hci_and_controller_init()` that
  doesn't exist in this ESP-IDF version (nor, it turns out, anywhere in the
  framework at all) — `nimble_port_init()` alone already brings up the
  controller internally.
- The vendored QR library's C-mode `bool`/`true`/`false` shim breaks under
  this toolchain's default `-std=gnu23` (where they're language keywords,
  not `<stdbool.h>` macros) in a way no `#undef`/re-`#include` trick on our
  side could work around — fixed by patching the shim out of the vendored
  header at vendor time instead (see `qr_display.c`'s header comment and
  "Build & flash" below).
- `sdkconfig.defaults` enabled NVS encryption without pinning a key-
  protection scheme, so ESP-IDF defaulted to the HMAC-peripheral scheme —
  while `nvs_storage.c` separately hand-implemented the *other* (Flash-
  Encryption-partition-based) scheme by calling an older API directly. The
  two were fighting each other. Fixed by simplifying `nvs_storage.c` to
  call plain `nvs_flash_init()` (which — per its own doc comment — already
  handles whichever scheme Kconfig selects internally) and pinning
  `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID`; the now-unused `nvs_keys` partition
  was removed from `partitions.csv`, and there is no such partition in this
  repo today. (NVS encryption has since been turned off entirely — the HMAC
  scheme burns an eFuse on first boot, irreversibly, as a silent side effect
  of first boot. **Note storage is therefore not encrypted at rest: a
  physical flash dump recovers every secret.** Physical possession is the
  protection model. See the security posture section below, and
  `sdkconfig.defaults`, which spells out the whole call. The `nvs_storage.c`
  simplification and the partition removal both still stand.)
- Two mechanisms that would have auto-fetched the QR library instead of
  vendoring it by hand were tried and empirically confirmed *not* to work
  for this framework/library combination — see `platformio.ini`'s comment.
- **A real flashed board hung permanently** — no response, ever, to *any*
  serial command whose JSON body contained a `"cmd"` key, regardless of its
  value's type or validity — the moment `src/proto/json.c`'s hand-rolled
  `json_find_raw()` reached its key-match branch. Isolated via
  `test/hardware/test_serial.py` against real hardware (bisected: `{}` and
  `{"foo":"bar"}` always responded fine; `{"cmd":123}` and
  `{"cmd":"get_info"}` never did), not reproducible in native `-Og` builds,
  and the root cause was never pinned down despite careful manual tracing.
  Fixed by replacing the entire hand-rolled reader/writer with the cJSON
  library (`json.c` is now a thin adapter preserving `json.h`'s exact public
  API, so `dispatcher.c` needed no changes) — the firmware pulls
  `espressif/cjson` via the ESP-IDF Component Registry
  (`src/idf_component.yml`), native tests link the system `libcjson`
  (`pacman -S cjson` / `apt-get install libcjson-dev`). Confirmed fixed
  against real hardware across several `test/hardware/test_serial.py` runs:
  the *permanent* hang never recurs — every command that used to hang
  forever (`{"cmd":123}`, `{"cmd":"get_info"}`, etc.) now gets a response
  every time. A separate, milder issue remains: individual responses
  occasionally arrive late or (rarely) truncated — 7-13 of 14 checks pass
  per run, never the same 1-2 that fail, and it gets *worse* across
  repeated runs against the same boot without a power cycle in between
  (confirmed: 10/12 then 7/12 back to back on the same boot).

  Two concrete theories were tested against real hardware and **both ruled
  out**, not just suspected:
  - *Heap leak/fragmentation from cJSON's per-command allocations.* Added a
    `free_heap_bytes` field to `get_info` (see `dispatcher.h`'s
    `free_heap_fn`, wired to `esp_get_free_heap_size()` in `main.c`) and
    sampled it across 30 consecutive `get_info` calls on one boot: **rock
    steady at exactly 200692 bytes every single time**, success or failure.
    No leak.
  - *BLE competing with TinyUSB for CPU1.* TinyUSB's task is pinned to
    CPU1 (`CONFIG_TINYUSB_TASK_AFFINITY_CPU1`), so a same-core task could
    plausibly starve it. But both NimBLE's controller and host tasks are
    pinned to CPU0 (`CONFIG_BT_CTRL_PINNED_TO_CORE_0` /
    `CONFIG_BT_NIMBLE_PINNED_TO_CORE_0`, confirmed in
    `sdkconfig.t-display-s3`) — can't be the cause. `ui_task` *is*
    unpinned and could land on CPU1 at the same priority as TinyUSB, but
    its per-wake work (`buttons_poll()` — two `gpio_get_level()` reads and
    a pure state-machine call) is too cheap to plausibly cause
    multi-second stalls on its own.

  **`serial_cdc.c`'s write path was rewritten once real bugs were found in
  it** (confirmed by reading
  `managed_components/espressif__esp_tinyusb/tusb_cdc_acm.c` directly, not
  guessed): `tinyusb_cdcacm_write_queue()` returns the number of bytes it
  actually staged, silently clamped to whatever's free in the 512-byte TX
  ring buffer (`CONFIG_TINYUSB_CDC_TX_BUFSIZE`) — the original code ignored
  that return value entirely, so a response that didn't fully fit got
  silently truncated or, if the buffer was already full from a
  still-draining prior response, dropped outright. The fix moved the
  actual write off `handle_rx()` (which runs nested inside TinyUSB's own
  task, where a *blocking* flush self-deadlocks — tried and confirmed
  worse on hardware) onto a dedicated `serial_tx_task` that checks
  `write_queue()`'s return value and retries with a bounded, non-blocking
  loop. See `serial_cdc.c`'s header comment for the full mechanism and the
  reasoning behind each constraint on that loop.

  **Getting that rewrite right took three more rounds of real hardware
  testing**, each catching a genuine regression the previous round
  introduced: an oversized (~4.1KB) response struct declared as a stack
  local overflowed `serial_tx_task`'s stack outright and reset the device
  (fixed: made `static`); a *blocking* flush called on every retry
  iteration stacked with this device's own separate ~2s+ baseline latency
  (see `test_serial.py`'s module docstring) until responses arrived outside
  the test client's read window and got misattributed to the wrong command
  (fixed: non-blocking flush, only wait when a write genuinely staged zero
  bytes); and an unbounded retry loop permanently wedged the tx task — and
  every response queued behind it — the one time `write_queue()` stopped
  making progress (fixed: a bounded give-up, `TX_GIVE_UP_US` in
  `serial_cdc.c`, generous enough to outlast the ~2s+ baseline latency
  without waiting forever).

  **Current state, confirmed on real hardware across several runs**: no
  more device resets, no more permanent lockups — every command always
  gets a chance, and a stuck transfer recovers on its own within the cap
  instead of wedging the transport. What's **not** fixed, and was never
  claimed to be: individual responses still occasionally arrive late,
  truncated, or (rarely) torn/misattributed to the wrong command — the same
  class of failure this section opened with, just no longer able to
  cascade into a full lockup. It's genuinely intermittent (a different
  command fails each run) and gets worse over a longer-running boot,
  consistent with the original description. The two ruled-out theories
  above still stand.

  **A live hardware session pointed at the read side, not the write side**:
  responses, once they start, arrive fast — the delay is in the device
  seeming slow to notice a command in the first place. Reading
  `serial_cdc.c` against that report turned up a real, confirmed-by-code
  match: `handle_rx()` called `dispatcher_handle()` (JSON parse/build,
  SHA-256, vault iteration — genuine work, not a fixed small cost) directly,
  inline, still nested inside `tud_task()` — the exact same "don't block
  the one task servicing the USB peripheral" rule points 1-3 above already
  established and fixed for the *write* path, just never applied to the
  *read/process* side. While `tud_task()` is captive running
  `dispatcher_handle()` for one message, it can't service the USB
  peripheral for anything else, including noticing the next incoming
  message — which is exactly the reported symptom. Fixed the same way the
  write path was: `handle_rx()` now only assembles a complete line and
  hands it to a new `serial_rx_task` over a queue; `dispatcher_handle()`
  (and the `vault_lock` around it) moved there, off `tud_task()` entirely.
  Compiles and passes the native suite; **not yet confirmed on real
  hardware** — this is the strongest untested lead so far, not a confirmed
  fix, and the underlying USB-OTG/DWC2-level suspicion from before isn't
  ruled out just because this one is plausible. If it's re-tested and the
  flakiness persists, live instrumentation around the actual write/flush
  call (e.g. logging `tud_cdc_n_write_available()` at the moment a stall is
  detected) is still the next step, not more static reasoning from source.

  **Added a `reset` command** (`dispatcher.c`/`dispatcher.h`,
  `main.c`, [`docs/PROTOCOL.md`](docs/PROTOCOL.md)) as a mitigation, not a
  fix, for the pattern documented above: it's a mitigation because the
  degradation is observed to get *worse* the longer a boot runs and to
  clear after a power cycle, so giving a remote client a way to force that
  power cycle is directly useful even without knowing the root cause. It
  responds `{"ok":true}` first, then reboots on a delay (`RESET_DELAY_US`
  in `main.c`, 10s) rather than immediately, specifically so the response
  has a real chance to actually leave the TX buffer before the device
  disappears — an immediate `esp_restart()` would race that. Available from
  the [device console](webinstaller/console.html) too. Not yet run against
  real hardware.

**What has and has not been run on hardware** is tracked per area, with
dates and firmware versions, in
[`docs/HARDWARE-TEST-CHECKLIST.md`](docs/HARDWARE-TEST-CHECKLIST.md) — that
file, not this paragraph, is the authority, and it marks anything unrun as
`NOT YET BENCH-RUN` in those words rather than leaving it implied. In
outline, as of 2026-08-17: the serial and BLE command protocols, the note
lifecycle, the export confirm gate's timeout path, display orientation, the
button state machine at rest, QR rendering and scanning, NVS persistence
across resets, and crash reporting have all been exercised on a classic
T-Display. The approval gesture itself, granting a wipe, an actual OTA
transfer, and the ESP32-S3 target's display and native-USB paths have not.

`test/hardware/bench.py` runs every check that does not need a finger on the
board. A different installed ESP-IDF/PlatformIO version than the pinned one
could still turn up something new; each fix above is documented in the
relevant file's own header comment as a starting point if it does.

`src/vault/` and `src/proto/` (the note state machine, SHA-256, hex, base64,
JSON reader/writer, command dispatcher, button gesture state machine, and
`lnurlw://` URL builder) are pure portable C with no ESP-IDF dependency at
all, and are additionally exercised by `test/native/` — **767/767
assertions pass** — independent of the ESP32 build. This is the security-
and protocol-critical logic, including the debounce/tap/chord logic gating
every plaintext-secret disclosure and the OTA signature-verification/
sequencing state machine (see "OTA firmware updates" below).

## Architecture

```
src/
  vault/     note state machine (PENDING/CONFIRMED/SPENT), SHA-256, hex —
             portable, no ESP-IDF dependency
  proto/     JSON reader/writer, base64 (OTA chunk payloads), the
             transport-agnostic command dispatcher, the button gesture
             state machine, the lnurlw:// URL builder — all portable, no
             ESP-IDF dependency
  ota/       OTA signature verification (ota_sign.c, portable, over vendored
             Monocypher — monocypher*.c/.h, see "OTA firmware updates"),
             plus ota.c's esp_ota_ops.h glue and release_key.c's baked-in
             public key (both ESP-IDF only)
  storage/   NVS-backed vault_storage_t implementation (ESP-IDF only)
  transport/ serial_cdc.c (USB-CDC/WebSerial), ble_gatt.c (NimBLE) — both
             take vault_lock.h's mutex around each dispatcher_handle() call
  board/     the hardware-abstraction seam: one board_*.c per physical
             board brings up the panel (bus, pins, rotation, offsets) and
             the buttons, and hands back a plain width x height surface.
             Nothing above this layer names a pin or a bus type. Adding a
             board is one file plus one line in CMakeLists.txt
  ui/        display.c (drawing only), buttons.c (thin adapter over
             proto/button_fsm.c), qr_display.c (needs a vendored QR
             library, see Build & flash), and ui_task.c — the single task
             owning buttons+display for both local note browsing and
             remote export_secret/OTA confirm requests (T-Display S3)
  vault_lock.c  the mutex serializing vault.c access between the transport
                task and ui_task (ESP-IDF only, see its header comment)
  device_reboot.c  shared delayed-reboot helper for `reset` and a
                   successfully finalized `ota_finish` (ESP-IDF only)
  main.c     wires RNG, storage, transports, OTA, and UI together
test/native/ unit tests for src/vault + src/proto + src/ota, plain gcc, no
             hardware
tools/ota_push.py  host-side OTA signing/push CLI — see "OTA firmware
                    updates" below
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
session itself. That includes firmware updates: OTA (below) is deliberately
serial-only, over the exact same JSON protocol as everything else — no
network stack was added to get it, and none is needed.

## OTA firmware updates

Adapted from [forgesworn/heartwood-esp32](https://github.com/forgesworn/heartwood-esp32)'s
OTA design — investigated after the user pointed at that project as prior
art. Their device also does this over USB serial rather than the network
their firmware otherwise uses for its main function, deliberately: "There is
deliberately no remote OTA implementation. Firmware updates remain USB-only,
release-signed, and physically approved" (their `docs/SECURITY-MODEL.md`).
That's a clean match for this project's own no-networking design, so the
scheme is ported here close to as-is: release images are ed25519-signed
(the device only ever verifies — the private key never touches it), the
signature is checked twice (`ota_begin` over the claimed digest, before the
owner is even asked; `ota_finish` over the digest of what was actually
written to flash — the check that carries the real guarantee), and the
transfer is gated by the same physical confirm/cancel tap `export_secret`
already uses.

Two adaptations from heartwood's version, both because this project's wire
protocol and toolchain are different from theirs (Rust/esp-idf-hal; a
binary-framed protocol from the start) rather than because anything here is
better or worse:

- **Base64-over-JSON, not binary framing.** heartwood's whole serial
  protocol is binary-framed; this project's is newline-delimited JSON
  throughout, so OTA chunks (`ota_chunk`'s `data` field,
  `src/proto/base64.c`) stay on that same wire format instead of
  introducing a second, binary sub-protocol multiplexed onto the same
  connection — simpler to reason about, at the cost of ~33% more bytes on
  the wire per chunk. Given this device's own documented serial latency
  (see Status below), that tradeoff favored simplicity.
- **[Monocypher](https://monocypher.org/) instead of a Rust crate.** This is
  C, not Rust — `ed25519-compact` wasn't an option. `davylandman/Monocypher`
  IS a real PlatformIO registry package, and — unlike `ricmoo/QRCode` (see
  "Build & flash", and `platformio.ini`'s comment for exactly why that one
  specifically gets filtered out) — it actually resolves via `lib_deps`
  here: it ships a proper `library.json`, confirmed with a real
  `pio run` (`LDF: ... Found 1 compatible libraries`). Not used anyway:
  it's pinned to 2.0.6 from 2019, four major versions behind the 4.0.3
  vendored here, and pulling both would mean two physically different
  copies of the same crypto code in one firmware image, only avoided
  colliding by accident of which one the linker resolves symbols from
  first — not something to depend on. Vendored whole instead, the same way
  `qrcode.c` already is (see "Build & flash"): `src/ota/monocypher.c`/`.h`
  and the optional `monocypher-ed25519.c`/`.h` module, unmodified from
  upstream 4.0.3. `src/ota/ota_sign.c` wraps just the one call the device
  needs (`crypto_ed25519_check`) behind the exact same domain-separated message
  scheme heartwood's `common/src/ota_sign.rs` uses (`"lnurlvault-ota-v1" ||
  0x00 || sha256(image)`, no board id — this project targets exactly one
  board today). **Cross-checked, not just unit-tested**: an image signed by
  `tools/ota_push.py` (Python's `cryptography` library) was verified
  successfully by `ota_verify_signature()` (Monocypher/C) directly, and a
  tampered digest against that same signature was correctly rejected —
  confirms the two implementations actually agree on the wire format, not
  just that each one is internally consistent.

**What's tested and what isn't.** The portable parts — parsing, base64,
signature verification, the `ota_begin`/`ota_chunk`/`ota_finish` sequencing
state machine (bad signature, declined/timed-out approval, out-of-order
chunks, early finish, a corrupted transfer caught at the finish-time
re-verify, recovery via a fresh `ota_begin` after an abort) — are exercised
by `test/native/` against a fake in-memory "flash", and the whole thing
(including the new `app_update`-based `ota.c` glue and the new
`otadata`/`ota_0`/`ota_1` partition table, see `partitions.csv`) compiles
and links in a real `pio run -e t-display-s3` build. **None of it has run
against a real device** — no actual OTA transfer over USB has happened.
The physical-approval gate (`ui_task_request_ota_confirm`, reusing
`ui_task.c`'s existing request queue) is architecturally identical to
`export_secret`'s. That one's timeout path *is* confirmed on hardware — an
unanswered request returns `{"ok":false,"error":"timeout"}` after ~31s over
both transports, with the BLE link intact throughout — but nobody has yet
pressed a button to approve an OTA image. `src/ota/release_key.c`'s `OTA_RELEASE_PUBKEY` now holds a
real generated key (`python3 tools/ota_push.py keygen`), not the all-zero
placeholder — `ota_begin` will accept images signed with the matching seed
instead of failing closed against everything. That seed needs to actually
be set as this repo's `OTA_SIGNING_SEED` CI secret for `release.yml`'s
signing step to work (see just below) — not confirmed from here.

**CI signing is wired**: `release.yml` now signs every tagged release's
`firmware.bin` automatically via the `OTA_SIGNING_SEED` repository secret
(`tools/ota_push.py sign`, invoked from a new "Sign firmware for OTA"
step), publishing `firmware.bin.sig` as a release asset — same shape as
heartwood's `OTA_SIGNING_SEED` GitHub secret (see their
`docs/ota-signing.md`), adapted to this project's Python tooling instead
of their Rust one. The release fails closed if the secret isn't set —
`python3 tools/ota_push.py keygen` prints the exact `gh secret set` command
to configure it, and the seed is stored **hex-encoded**, not as raw bytes:
GitHub Actions secrets are handled as text/env vars, and a raw 32-byte
value can contain bytes that don't survive that round-trip intact; the
workflow decodes the hex back to bytes itself before signing. This part of the workflow **has** now been exercised on a real tagged
release. v0.0.4 was signed by CI, and its `firmware.bin.sig` verifies
against the public key compiled into the firmware — checked from the
published artefact rather than from the build that made it, and confirmed on
a real board, which accepts that signature at `ota_begin` and refuses a
tampered one in 0.2s.

**Key rotation is a two-release operation**, the same shape as heartwood's
own (`docs/ota-signing.md`), because a device only trusts the key it was
*built* with, not whatever the CI secret currently holds: generate the new
keypair, but ship the *next* release still signed with the **old** seed —
that release just carries the new public key baked into `release_key.c`.
Only once devices have updated through that release do you swap
`OTA_SIGNING_SEED` to the new seed; from the release after *that* one
onward, new signatures use the new key, which those now-updated devices
can verify. A device that skips the in-between release has to update
through it (or get a fresh USB flash) before it can accept anything signed
with the new key. If the seed is ever compromised, treat OTA as untrusted
until rotation completes — an attacker with the seed still needs the
physical approval tap to push anything, so the blast radius is local, but
rotate promptly regardless.

**What OTA signing does and doesn't protect against** — stated as plainly
as heartwood states it for their own: it protects the *update* path only.
It does nothing for the *first* USB flash (today's `pio run -t upload` /
the web installer both write an unsigned image to a blank device — trust-
on-first-use) or for an attacker with physical access and a debugger.
Closing that gap needs Secure Boot V2, which — like Flash Encryption,
covered in Security posture below — is a separate, not-yet-taken hardening
step, not something OTA signing substitutes for.

## Build & flash

Requires [PlatformIO](https://platformio.org/) (`pip install platformio`
if you don't have it), which resolves the ESP-IDF toolchain itself on
first build.

**Before the first build**, vendor the QR encoder `qr_display.c` needs —
not included in this repo (see its header comment for why, and for two
auto-fetch mechanisms that were actually tried and confirmed not to work
for this library, not just skipped):

```sh
./tools/vendor_qrcode.sh src/ui
```

This is the same step both workflows run. It fetches
[ricmoo/QRCode](https://github.com/ricmoo/QRCode) (MIT) at a **pinned
commit**, checks both files against **pinned SHA-256 hashes**, and refuses
to proceed on a mismatch — this library is compiled into a firmware that
holds bearer secrets, and a moved tag or a force-push would otherwise
change what ships with no record of it. It then applies the one patch
`qrcode.h` needs to build here (see `qr_display.c`'s header comment for
why). Download it by hand and you skip the verification, so use the
script.

```sh
pio run -e t-display-s3 -t upload
pio device monitor
```

Board: [LilyGo T-Display S3](https://github.com/Xinyuan-LilyGO/T-Display-S3)
(ESP32-S3R8, 170×320 ST7789 LCD on an 8-bit i80 parallel bus, 2 buttons).
**Verify `src/board/board_t_display_s3.c` against your specific board
revision** using LilyGo's own `pin_config.h` before flashing — LilyGo has
shipped more than one board under similar names. See that file's header
comment.

If `pio run` fails on a specific symbol (an NVS-encryption call, a NimBLE
header, an esp_lcd struct field), that's expected per the Status section
above — the failing file's own top comment says which ESP-IDF version
change is the likely cause and what to check against.

## Releases & web installer

Pushing a tag (`git tag v0.1.0 && git push origin v0.1.0`) triggers
[`.github/workflows/release.yml`](.github/workflows/release.yml), which:

1. Builds the firmware with PlatformIO for **both** supported boards (same
   as "Build & flash" above). Releases used to build only the S3, so anyone
   holding a classic T-Display got no installer image and no OTA image at
   all, with nothing saying so.
2. Signs `firmware.bin` for OTA (`tools/ota_push.py sign`, driven by the
   `OTA_SIGNING_SEED` repository secret) and publishes `firmware.bin.sig`
   alongside it — see "OTA firmware updates" above for the scheme and
   `tools/ota_push.py keygen`'s own output for how to set that secret in
   the first place. Fails the whole release closed if the secret isn't
   set, deliberately, rather than shipping a release nobody's device can
   ever accept over OTA.
3. Merges `bootloader.bin` + `partitions.bin` + `ota_data_initial.bin` +
   `firmware.bin` into one `merged-firmware.bin` per board via
   `esptool.py merge_bin`, with each board's SPI flash mode/frequency/size
   and — importantly — its own bootloader offset baked in, all read off
   that board's own `pio run -t upload -v` invocation rather than guessed:

   | Board | Flash | Bootloader at |
   |---|---|---|
   | ESP32-S3 | `dio` / `80m` / `16MB` | `0x0` |
   | classic ESP32 | `dio` / `40m` / `16MB` | `0x1000` |

   That offset differs by chip and an image built with the wrong one flashes
   cleanly and never boots.
   [esp-web-tools](https://esphome.github.io/esp-web-tools/)' own docs are
   explicit that it can't patch these on the fly when writing multiple
   separate parts over Web Serial, and that ESP-IDF firmware needs to be
   pre-merged — skipping this is a real, confirmed way to get "the
   installer reports success, but the board never actually boots the
   firmware." (This merged image is for that *initial* flash only — OTA
   updates only ever replace the plain `firmware.bin` app image, never the
   bootloader or partition table, so it's signed separately in step 2
   rather than as part of this merge.)
4. Publishes a GitHub Release for the tag with all of the above (including
   both `.sig` files and a `SHA256SUMS` over everything) plus a
   `manifest.json` carrying **both** boards — esp-web-tools picks the build
   matching the chip it finds. Each build points at its own merged binary at
   offset `0`; `partitions.csv`'s offsets are already baked into the merge,
   not re-declared in the manifest the way an earlier version did it.
5. Regenerates and deploys **[`webinstaller/`](webinstaller/)** to GitHub
   Pages: a page that lists every published release (fetched via `gh
   release list`, no server needed beyond static Pages hosting) and lets a
   visitor flash any of them directly from Chrome/Edge over Web Serial —
   no PlatformIO, no command line, just a USB cable. Every release's
   binaries are copied into the deployed site itself (`firmware/<tag>/`)
   rather than linked cross-origin to GitHub's release CDN, which doesn't
   serve CORS headers `fetch()` would need — see the workflow's "Fetch
   every release's firmware assets" step, which then **verifies that every
   file each manifest references actually landed in the site** and fails the
   job otherwise. That check exists because the installer had never once
   worked: `merged-firmware.bin` was not among the copied patterns even
   though it is the only file the manifest names, and the step reported
   success while producing an empty `firmware/` directory, so every release
   the page offered returned 404. A release that does not install is worse
   than no release, because it looks like it worked.

   A release whose assets cannot satisfy its manifest is dropped from the
   page rather than failing the job — `v0.0.2` predates `merged-firmware.bin`
   and nobody can change that history — and `releases.json` is filtered to
   match, so the dropdown never offers a version that 404s on selection.

That last step also runs standalone (`workflow_dispatch`, no new tag
needed) — useful for updating `webinstaller/index.html` itself, or after
manually deleting a bad release, without cutting a new firmware version.

**One-time manual setup this workflow can't do for you**: Settings → Pages
→ Source: "GitHub Actions" — the `github.io` URL won't serve anything until
that's set; and the `OTA_SIGNING_SEED` repository secret (Settings →
Secrets and variables → Actions), from `python3 tools/ota_push.py keygen`
— see "OTA firmware updates" above. The web installer page currently
hardcodes this repo as `dni/lnurl-vault` in a couple of "view source"
links.

**A flashed install erases every note on the device.** `merge_bin` pads the
gaps between parts, so an image written from offset `0` covers the NVS
region with `0xFF` — measured, on a board that went from a full vault to
zero notes across a re-install. It is not an update in place, whatever the
erase prompt suggests. Updating a device that already holds notes means a
signed OTA image, which only ever replaces the app partition.

This workflow has run and reported success on real tags. That is not the
same as having worked: v0.0.2 through v0.0.4 all published usable release
artefacts and all deployed an installer page that could not install any of
them. What *is* verified as of v0.0.4 is the artefact side — checksums
verify, and `firmware.bin.sig` verifies against the public key compiled into
the firmware, over the domain-separated message `ota_signing_message()`
builds rather than the bare digest. The device side is verified as far as
`ota_begin`: a real board accepts that signature and refuses a tampered one
in 0.2s, before the owner is prompted. Nothing has yet completed an approved
transfer. See `docs/HARDWARE-TEST-CHECKLIST.md` sections 15 and 16.
If `.pio/build/t-display-s3/`'s actual output filenames ever stop matching
what the "Collect build outputs" step expects (a different PlatformIO/
ESP-IDF version resolved by the runner could still rename them), that
step's `find` patterns are the first thing to check.

## Security posture

- **Secrets** come from `esp_fill_random()` (the ESP32-S3's hardware TRNG).
  Espressif documents its full-entropy guarantee as conditional on Wi-Fi or
  BT having been active at least once — `main.c` starts BLE before the
  first RNG self-test or secret generation to satisfy that. A cheap startup
  self-test (two 16-byte draws must differ and not be all-zero) guards
  against a catastrophically stuck RNG; it is *not* a statistical
  randomness test suite.
- **Storage**: NVS encryption is **off** (`CONFIG_NVS_ENCRYPTION=n`, see
  `sdkconfig.defaults`), so **note storage is not encrypted at rest — a
  physical flash dump recovers every secret**. It is off deliberately: on the
  ESP32-S3 the HMAC key-protection scheme burns an eFuse on first boot,
  irreversibly, as a silent side effect of powering the device on. Physical
  possession is the protection model — treat the device like cash in a wallet.
  **Real protection against physical extraction requires provisioning Flash
  Encryption + Secure Boot V2** — a deliberate, irreversible (eFuse-burning)
  manual production step, intentionally left to you before this device ever
  holds value you care about. OTA image signing (below) does not substitute
  for this: it protects the update path, not a first USB flash or physical/
  debugger access — see "OTA firmware updates" above for exactly where that
  line is. (Making NVS encryption a build-time production option is tracked in
  the issues.)
- **No networking on-device.** The only attack surface is the paired
  USB-CDC or BLE session — OTA firmware updates included, deliberately (see
  "OTA firmware updates" above). All mint calls are the browser's
  responsibility.
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

- **On-screen note detail** is now rendered (`src/ui/display.c`'s
  `display_note_detail`, over the `src/ui/font5x7.c` bitmap font): while
  browsing, the screen shows the selected note's amount, label and id plus its
  1-based position, and the confirm prompt shows the same for the note being
  approved — so approving or unveiling a note is a deliberate read, not a
  miscount. The font is a hand-transcribed 5x7 bitmap; it renders and scans on
  hardware (see `docs/HARDWARE-TEST-CHECKLIST.md`), and moving to LVGL for
  richer text remains a possible future step.
- **The QR encoder is a required external dependency**, not included in
  this repo — see "Build & flash" above. Nothing in `src/ui/qr_display.c`
  will compile until it's vendored (confirmed both PlatformIO `lib_deps`
  and an `idf_component.yml` git dependency don't work around this for this
  particular library — see `platformio.ini`'s comment for what was tried).
- **OTA exists but is untested on hardware and unusable until a release key
  is generated** — see the "OTA firmware updates" section above for what's
  built, what's cross-checked, and what still needs a real device.
- BLE pairing/bonding is unauthenticated in this v1 (any nearby device can
  connect and issue commands, though it still can't extract a secret
  without a physical gesture on the vault itself). Consider BLE
  bonding/passkey pairing before relying on BLE outside a trusted room.

## Verification

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs both of the
following automatically on every push to `main` and every pull request: the
native unit tests, and a PlatformIO build of the firmware (no upload, no
board needed — just "does it compile"). Unlike `release.yml`, it publishes
nothing; it's a signal, not a release gate.

**Native unit tests** (portable core, no hardware, no PlatformIO needed;
needs the system cJSON library — `pacman -S cjson` or
`apt-get install libcjson-dev`, see `test/native/Makefile`):

```sh
cd test/native && make test
```

This actually runs in this environment — 767 assertions across SHA-256
known-answer vectors, JSON reader/writer round-trips (including escaping and
the overflow-detection path), the full vault state machine (legal and
illegal state transitions, split/merge parent lineage, the id-collision
retry path, and a simulated-reboot persistence round-trip against a fake
storage backend satisfying the same `vault_storage_t` interface
`src/storage/nvs_storage.c` implements for real), the `lnurlw://` URL
builder, the button gesture state machine (debounce/bounce filtering,
simple taps, a chord that fires exactly once and doesn't also emit trailing
taps, and staggered chord entry), base64 round-trips (RFC 4648 vectors plus
a 1024-byte binary chunk), the ed25519 OTA signature scheme (a genuine
signature verifies, a tampered digest/wrong key/corrupted signature/
all-zero placeholder key all fail closed), and the full `ota_begin`/
`ota_chunk`/`ota_finish` sequencing state machine against a fake in-memory
"flash" (happy path, bad signature, declined/timed-out approval, a
wrong-offset chunk that doesn't kill the session, no-active-session errors,
an early finish, a corrupted transfer caught at the finish-time re-verify,
and recovery via a fresh `ota_begin` after an abort).

**Hardware verification** (needs a flashed board):

1. ~~`pio run -e t-display-s3 -t upload && pio device monitor`~~ **Done** —
   flashed to a real T-Display S3 and round-tripped over USB-CDC using
   `test/hardware/test_serial.py` (13/14 checks pass; see the Status
   section above for what that confirmed, including the cJSON migration
   fix). Vendoring the QR library (see "Build & flash") is still needed for
   a build that includes the on-device QR/unveil UI.
2. Exercise the rest of the command set by hand, typing JSON lines directly
   into the serial monitor: `new_secret` → `confirm` → `list_notes` →
   `export_secret` (press the confirm button when the display goes amber)
   → `mark_spent` → `list_notes`.
3. Separately validate BLE: connect with a generic GATT explorer (e.g. nRF
   Connect), write a JSON command chunked per `docs/PROTOCOL.md`'s framing
   (including a message deliberately larger than one MTU, to exercise
   reassembly), and confirm the notified response reassembles correctly.
4. Exercise on-device browsing with at least two `CONFIRMED` notes: tap to
   enter browse mode (watch it blink out position 1), tap again (blinks
   position 2), hold both buttons together (~200ms) to unveil — confirm a
   QR appears, and scan it with a stock phone camera: it should open the
   wallet with that note claimable (the payload is an https claim link by
   default — see `docs/PROTOCOL.md`'s "On-device note browsing", and issue
   #26 for why it is not `lnurlw://` any more). Tap once to dismiss and
   confirm the screen actually clears.
5. **Not yet attempted**: a real OTA update end to end.
   `python3 tools/ota_push.py keygen --out ota-release.seed`, paste the
   printed public key into `src/ota/release_key.c`'s `OTA_RELEASE_PUBKEY`,
   reflash once over USB the normal way, then `python3 tools/ota_push.py
   push --port /dev/ttyACM0 --image .pio/build/t-display-s3/firmware.bin
   --seed ota-release.seed` — approve on the device when prompted, and
   confirm it reboots into the new image (`get_info`'s `fw_version` is the
   easiest thing to check before/after if you bump `LNURLVAULT_FW_VERSION`
   between builds). See "OTA firmware updates" above for what's already
   cross-checked (Python↔C signature verification) versus what genuinely
   isn't (a live transfer, the physical approval prompt, and whether
   `esp_ota_set_boot_partition` + a reboot actually lands on the new
   image).
