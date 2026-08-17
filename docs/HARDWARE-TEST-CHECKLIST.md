# Hardware test checklist

Nothing past `dispatcher_handle()` is reachable by the native tests. This is
the record of what has actually been run on a board, and — just as
importantly — what has not.

**How to read a bench record.** Every section ends with one. A record names
the date, the board and the firmware version `get_info` reported, because
"it worked" is worth very little without those three. A section with no
record says **NOT YET BENCH-RUN** in those words, so nothing quietly counts as
verified by having been written down.

**Run the machine-driven parts first:**

```
pip install pyserial bleak
python3 test/hardware/bench.py --port /dev/ttyUSB0 --ble
```

`bench.py` covers every check that does not need a finger on the board, and
exits non-zero if any fails. It never grants a wipe and never approves an
export — an automated test one press away from erasing a device is not a test
worth having. Those rows are human-run, and marked as such.

Two things about the serial port that cost real time to learn, both handled
inside `bench.py` and worth knowing if you drive the device by hand:

- Opening the port drives DTR/RTS into the board's auto-reset circuit, so the
  device **reboots when you connect**. A script that opens the port per
  command reboots between every command, which silently invalidates anything
  measuring uptime or persistence. Hold one connection open.
- Boot chatter, and any enabled diagnostics, write plain text to the same
  UART that carries the protocol on the classic board. Match responses by
  looking for a line starting with `{`, not by taking the next line.

---

## 1. Build and flash

Both environments build, and a flashed device boots and answers.

```
python3 test/native/…      # make test in test/native/ — must be green first
pio run -e t-display-s3
pio run -e t-display
pio run -e t-display -t upload --upload-port /dev/ttyUSB0
```

Pass: both builds succeed, and `{"cmd":"get_info"}` answers after flashing.

> **Bench record — 2026-08-17.** Classic LilyGo T-Display (ESP32-D0WDQ6),
> firmware `0.0.2-25-g0b3477c`. Both environments build; `t-display` flashed
> and answered. 319/319 native assertions green. ESP-IDF 6.0.1 via
> PlatformIO 6.1.19, `espressif32@7.0.1`.
> S3 target: **builds only.** Not bench-run in this pass — its port was held
> by another process on the test machine, so the i80 display path and native
> USB-CDC were not exercised. Treat the S3 as unverified for anything below
> that says "classic" in the record.

## 2. Host transport — serial

`get_info`, `list_notes` and a note mint all round-trip; the device reports a
real firmware version and a board identifier.

Pass: `fw_version` is **not** `0.1.0` — that was a hardcoded literal every
release reported regardless of what it was built from (issue #11). It should
be a tag, or a `git describe` string for a local build.

> **Bench record — 2026-08-17.** Classic T-Display, `0.0.2-25-g0b3477c`.
> `get_info` round-trips; `fw_version='0.0.2-25-g0b3477c'`,
> `board='t-display'`, `storage='ok'`. Via `bench.py`.

## 3. Host transport — BLE

Connect, subscribe, and exercise both a read and a command that **writes to
flash**.

Pass: the write works and the link survives it. This is not a formality — a
write over BLE was impossible before #29, because `dispatcher_handle()` ran on
the NimBLE host task: the link dropped and the note was never created, while
reads worked fine, which is why it went unnoticed.

> **Bench record — 2026-08-17.** Classic T-Display, `0.0.2-25-g0b3477c`.
> Advertises as `lnurl-vault`; ATT MTU negotiated to 256; `get_info` in
> 0.05s; `new_secret` over BLE succeeded in 0.05s with the link intact.
> Via `bench.py --ble`.

## 4. Note lifecycle

Mint, confirm, list, rename, mark spent, delete. Counts move as expected.

> **Bench record — 2026-08-17.** Classic T-Display, `0.0.2-25-g0b3477c`.
> `new_secret` + `confirm` succeeded, note count 11 → 12, and the new id
> appeared in `list_notes`. Rename/mark-spent/delete not exercised in this
> pass.

## 5. The two-phase commit property

The security claim the whole design rests on: the `h` disclosed to the mint at
mint time must equal `sha256` of the secret the device later exports.

Mint a note, record `h`, confirm it, export the secret, and hash it
independently — not with the device's own SHA-256.

> **Bench record — 2026-08-17.** Classic T-Display. `sha256(exported k1)`
> matched the `h` disclosed at mint time, hashed with Python's `hashlib`
> rather than the firmware's own implementation. The hand-written
> `src/vault/sha256.c` was separately cross-checked against `hashlib` on
> device-generated data.

## 6. The disclosure gate

`export_secret` must not disclose anything without a physical approval, and an
unanswered request must time out cleanly with the transport still usable.

Pass: `{"ok":false,"error":"timeout"}` after roughly the confirm window, over
both transports, with the BLE link still up.

> **Bench record — 2026-08-17.** Classic T-Display, `0.0.2-25-g0b3477c`.
> Serial: timed out at 31.0s, device responsive after. BLE: timed out at
> 31.0s with the link intact across the whole window. Via `bench.py --ble`.
> This is the specific thing issue #4 predicted would fail — it predicted
> link supervision would tear the connection down during the wait. It does
> not, on this controller; measured on unfixed firmware too.

## 7. The approval gesture — NOT YET BENCH-RUN

Needs a finger on the board. Send `export_secret`, then:

| Do | Expect |
|---|---|
| Hold button 1 | a bar fills, and it approves at about 2s |
| Tap button 1 | nothing at all |
| Press button 2 | declines immediately |
| Hold button 1, let go at ~1s, hold again | it does **not** deny — a slipped finger is not a decision |
| Let it lapse | a distinct "expired" screen, not the decline colour |

**NOT YET BENCH-RUN.** The logic is covered by
`test/native/test_approval.c` a tick at a time, including contact bounce
mid-hold, but no press has been made on hardware.

## 8. Display and orientation

Pass: the image is the right way up, not mirrored, with no offset band at any
edge.

> **Bench record — 2026-08-17.** Classic T-Display. Orientation resolved
> empirically over three flash cycles using the colour/geometry self-test
> (`-DLNURLVAULT_DISPLAY_SELFTEST`), landing on `invert_color(true)`,
> `swap_xy(true)`, `mirror(true, true)`, `set_gap(40, 52)`. The walk that got
> there — including the finding that toggling `mirror_Y` moved the image
> *horizontally*, because esp_lcd mirrors in panel coordinates before
> `swap_xy` transposes the axes — is recorded in
> `src/board/board_t_display.c`.

## 9. On-device browsing and QR

Tap to cycle CONFIRMED notes, chord to unveil one as a QR, and scan it with a
phone.

Pass: the code scans. Note that "does it scan" collapses the encoder, the
renderer, the optics and the phone's decoder into a single bit — if it fails,
the QR density ladder (`-DLNURLVAULT_QR_SELFTEST`) is what separates them.

> **Bench record — 2026-08-17.** Classic T-Display. Codes render and a phone
> camera decodes them. **But** nothing opened them: the URL is a
> `lnurlw://` (LUD-17) scheme with no handler on the phones tried — issue
> #26, a product decision, not a rendering fault.
>
> Do not enable `-DLNURLVAULT_QR_SELFTEST` in a shipping build: `app_main()`
> runs the diagnostics before `ui_task_start()`, and the ladder waits for a
> press per code, so while it runs `ui_task` does not exist and no approval
> can be serviced at all. Leaving that flag set was enough to make
> `export_secret` fail on hardware.

## 10. Buttons

Pass: no spurious events at rest, and both buttons register.

> **Bench record — 2026-08-17.** Classic T-Display. `button_fsm` produced
> zero spurious events across 31s at rest. GPIO35 is input-only with no
> internal pull, and the board's external pull-up was confirmed present.

## 11. Persistence across power cycles

Mint and confirm notes, power-cycle the board, and confirm they are still
there with the right states.

> **Bench record — 2026-08-17.** Classic T-Display. Notes survived repeated
> reboots (software resets and EN-pin resets) with counts and states intact.
> A full unplug/replug cycle was not specifically isolated in this pass.

## 12. Storage exhaustion, and wipe

The device must **never** erase to recover. On a full partition it reports
`storage: "full"` and leaves every note on flash.

`wipe` must refuse everything except a deliberate, confirmed request:

| Send | Expect |
|---|---|
| `{"cmd":"wipe"}` | `bad_request`, immediately, no prompt |
| `{"cmd":"wipe","confirm":"yes"}` | `bad_request` |
| `{"cmd":"wipe","confirm":"WIPE"}`, press nothing | `timeout`, nothing erased |
| `{"cmd":"wipe","confirm":"WIPE"}`, approve | `{"ok":true,"wiped":true}`, then a reboot into an empty vault |

> **Bench record — 2026-08-17.** Classic T-Display, `0.0.2-25-g0b3477c`.
> The three refusals all behaved: bare wipe refused in 0.1s with no prompt,
> wrong phrase refused, correct phrase prompted then timed out. Note count
> unchanged at 13 throughout. Via `bench.py`.
>
> **Granting a wipe: NOT YET BENCH-RUN**, deliberately — kept out of the
> automated bench so no test run is one press from erasing a device. Also
> **not bench-run**: reaching a genuinely full NVS partition to see
> `storage: "full"` in the wild. `list_notes` returning
> `response_too_large` at around 30 notes *was* reproduced, which is issue #7
> and the nearest thing to it.

## 13. Crash reporting and the watchdog

After an unexpected reset, `get_info` must say what happened.

> **Bench record — 2026-08-17.** Classic T-Display. Verified by crashing a
> board on purpose — a null dereference behind a temporary command, built and
> flashed for the test only and not committed:
>
> ```
> before   reason='poweron'  boot=1  unexpected=False
> after    reason='panic'    boot=2  unexpected=True
>          last_cmd_in_flight='__crash_for_test'
> ```
>
> Also confirmed: a software reset reports `reason='sw'` with the boot count
> incrementing and no command named (correct — `reset` returns before the
> delayed restart fires). And an EN-pin reset **loses** the breadcrumb and
> reads as `poweron`, because it resets the RTC domain too. That is the
> intended lifetime, not a gap.
>
> **The watchdog firing: NOT YET BENCH-RUN.** Staging a >60s stall in
> `ui_task` needs code that would then have to be removed. The subscribe and
> feed calls run every boot; the timeout path itself is ESP-IDF's.

## 14. OTA — NOT YET BENCH-RUN

Sign an image, push it with `tools/ota_push.py`, approve on the device, and
confirm it boots the new firmware and reports the new version.

**NOT YET BENCH-RUN.** Needs the release signing seed, which is a CI secret
and not in this repo. The signature verification and session sequencing are
covered by `test/native/test_ota_dispatch.c` and `test_ota_sign.c` against a
fake in-memory flash; nothing about the real `esp_ota_*` writes, the partition
switch, or a rollback has been exercised on hardware.

## 15. Release artefacts — NOT YET BENCH-RUN

Cut a tag, then check the published release carries `firmware.bin`,
`firmware.bin.sig`, `SHA256SUMS` and `manifest.json`; that the checksums
match; that `firmware.bin.sig` verifies against the committed public key; and
that the web installer flashes `merged-firmware.bin` onto a blank board which
then boots and reports the tag as its `fw_version`.

**NOT YET BENCH-RUN** since the release pipeline was pinned and given
checksums.
