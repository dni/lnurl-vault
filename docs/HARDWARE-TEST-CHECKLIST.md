# Hardware test checklist

Nothing past `dispatcher_handle()` is reachable by the native tests. This is
the record of what has actually been run on a board, and — just as
importantly — what has not.

**How to read a bench record.** Every section ends with one. A record names
the date, the board and the firmware version `get_info` reported, because
"it worked" is worth very little without those three. A section with no
record says **NOT YET BENCH-RUN** in those words, so nothing quietly counts as
verified by having been written down.

**Open faults**, so they are not buried in a numbered section: the ESP32-S3's
cancel button reads as permanently pressed, so that board has no working way
to decline a prompt — section 7a. It is no longer believed to block approval
(the record that said so predates the firmware that fixed it), but that has
**not been re-run on hardware**.

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
> **S3 bench record — 2026-08-17, later the same day.** LilyGo T-Display S3.
> Its port had been held by another process; once freed, the board was flashed
> and exercised for the first time. It boots, brings up its panel, and presents
> its native USB-CDC port, on which the command protocol works. `bench.py`
> against it: 10 passed, 4 failed, 7 skipped. Two of those failures are the
> button fault in section 7a below; one is `fw_version`, expected on the branch
> tested; the rest passed.
>
> Everything else below still says "classic" where only the classic board was
> used. The S3 is no longer wholly unverified, but it is not equally verified.
>
> **Bench record — 2026-08-18, v0.0.6.** Classic T-Display, `fw_version='0.0.6'`
> — first hardware run of the post-#87..#93 firmware. `bench.py --ble`:
> **25 passed, 0 failed, 5 skipped**. Re-confirms serial and BLE transport, the
> note lifecycle, `list_notes` paging, the export gate timing out at 31.1s over
> both transports, and every wipe refusal — none bench-run since v0.0.4. The 5
> skips are the physical-press rows (§8/§10) and a granted wipe (§13).

## 2. Host transport — serial

`get_info`, `list_notes` and a note mint all round-trip; the device reports a
real firmware version and a board identifier.

Pass: `fw_version` is **not** `0.1.0` — that was a hardcoded literal every
release reported regardless of what it was built from (issue #11). It should
be a tag, or a `git describe` string for a local build.

> **Bench record — 2026-08-17.** Classic T-Display, `0.0.2-25-g0b3477c`.
> `get_info` round-trips; `fw_version='0.0.2-25-g0b3477c'`,
> `board='t-display'`, `storage='ok'`. Via `bench.py`.
>
> Re-confirmed against a real release tag rather than a dev build: `main` at
> v0.0.4 reports `fw_version` exactly `0.0.4`, so the version reaches the wire
> from the tag and not from a hardcoded literal.

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
> Re-confirmed later the same day against every open change merged together:
> 25 passed, 0 failed, 5 skipped.
> This is the specific thing issue #4 predicted would fail — it predicted
> link supervision would tear the connection down during the wait. It does
> not, on this controller; measured on unfixed firmware too.

## 7a. The S3's cancel button is dead — OPEN FAULT, and a stale record

**What is still true:** on the ESP32-S3, `PIN_BUTTON_2` (GPIO14) reads as
permanently pressed. Nothing in this repo drives GPIO14 and
`board_buttons_init()` enables the internal pull-up, so it is a wrong pin, a
board revision that moved the button, or a pad left held or in RTC mux by
firmware flashed before this.

**What is no longer true, and was left standing here for a day:** that this
makes `export_secret` impossible on that board.

The record below was taken against firmware `0.0.2-25-g0b3477c`. That commit
**predates PR #33** (hold-to-approve), which introduced the rule that a button
which has not been seen released since a prompt began cannot answer that
prompt. On current firmware a wedged cancel line can no longer deny anything —
`cancel_stale` never clears on a pin that never reads released, so the cancel
branch is never taken, and a genuine two-second hold on button 1 should still
approve.

That is a claim about the logic, not a bench result. It is pinned by
`test/native/test_approval.c`'s
`test_a_wedged_cancel_line_does_not_block_a_real_approval`, which fails if the
rule is ever weakened. **On hardware it is NOT YET BENCH-RUN.**

So the consequence has changed shape rather than gone away. The S3 has not got
a broken confirmation gate; it has got no cancel button. Every gated command
on that board can be approved or left to time out, and nothing else.

**What the device now says about it.** `get_info` reports an `inputs` object
(`docs/PROTOCOL.md`), and on this board it should read
`{"confirm":"ok","cancel":"stuck"}`. That is the single fastest check of
whether the pin is still wrong, and it needs no download mode — the command
protocol works fine on the S3.

**Next steps, cheapest first:**

1. Flash current firmware and read `get_info`. If `cancel` reports `ok`, the
   pad-state fix below was the cause and this section closes.
   `board_buttons_init()` now calls `gpio_hold_dis()` / `rtc_gpio_deinit()` on
   both button pins before configuring them, because neither a pad hold nor
   RTC mux ownership is cleared by `gpio_config()`, both survive a software
   reset, and both present exactly as a permanently-pressed button. This board
   has carried other firmware.
2. If it still reports `stuck`, hold a real export prompt and try approving it
   with a two-second hold on button 1. That answers the question this section
   was wrong about, and it is worth answering before hunting the pin.
3. Only then go looking for the right pin, which does need the board in
   download mode — hold BOOT, tap RESET, release BOOT. Its ROM port disappears
   once the firmware's TinyUSB claims USB and does not return on a software
   reset (checked), so this cannot be automated.

> **Bench record — 2026-08-17, SUPERSEDED.** T-Display S3, firmware
> `0.0.2-25-g0b3477c`. With nothing touched:
>
> ```
> export_secret -> {"ok":false,"error":"user_declined"}  after 0.92s
> wipe          -> {"ok":false,"error":"user_declined"}  after 0.91s
> ```
>
> 0.92s is the cancel debounce plus the result card. Reproduced on demand. The
> classic board does not share it: the same prompt there times out at 31s.
>
> Kept, not deleted, because it is the evidence that GPIO14 reads low — which
> is still the open half. It is superseded only in what it implies about
> approval, for the reason given above: this firmware predates #33.

## 7b. A glitch on the cancel line

> **Bench record — 2026-08-17.** Classic T-Display. A confirm over serial
> followed by a confirm over BLE returned `user_declined` in about a second
> with nobody near the device — reproducible in that order, while the same
> prompt on a fresh boot timed out correctly at 31s, and the approval loop's
> own record showed both buttons reading released at the end. Consistent with
> what `board_t_display.c` warns about for GPIO35: input-only on the classic
> ESP32, no internal pull resistor, dependent on the board's external one, and
> high-impedance next to a transmitting radio.
>
> The cancel debounce was raised from 30ms to 250ms. Re-run afterwards on the
> same board: **25 passed, 0 failed, 5 skipped** including the BLE path.

## 8. The approval gesture — NOT YET BENCH-RUN

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

## 9. Display and orientation

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

## 10. On-device browsing and QR

Tap to cycle CONFIRMED notes, chord to unveil one as a QR, and scan it with a
phone.

Pass: the code scans. Note that "does it scan" collapses the encoder, the
renderer, the optics and the phone's decoder into a single bit — if it fails,
the QR density ladder (`-DLNURLVAULT_QR_SELFTEST`) is what separates them.

Pass now also means the code **opens something**, not just that it decodes.
The payload is an https claim link by default (`docs/PROTOCOL.md`), so scan it
with a stock iPhone camera and a stock Android camera and check each lands in
the wallet with the note claimable.

> **Bench record — 2026-08-17.** Classic T-Display. Codes render and a phone
> camera decodes them. **But** nothing opened them: the URL was a
> `lnurlw://` (LUD-17) scheme with no handler on the phones tried — issue
> #26. Addressed since by switching the default payload to an https claim
> link; this record stands as the evidence that rendering and decoding were
> never the problem.
>
> Do not enable `-DLNURLVAULT_QR_SELFTEST` in a shipping build: `app_main()`
> runs the diagnostics before `ui_task_start()`, and the ladder waits for a
> press per code, so while it runs `ui_task` does not exist and no approval
> can be serviced at all. Leaving that flag set was enough to make
> `export_secret` fail on hardware.

> **Bench record — 2026-08-20, real sats, fw `0.0.7-6-g6cda81e`.** Classic
> T-Display. **The offline handoff worked end to end for the first time.** A
> 50,000 msat note from moneyer.dev, held on a secret only this board had
> seen, was browsed to, unveiled with the chord, scanned off the panel with a
> phone camera, and claimed into a wallet on that phone. No cable, no pairing,
> nothing plugged into the device. The sats are on the phone.
>
> It had never worked before, and could not have, for reasons in three
> separate repositories — all found by trying it rather than by reading
> anything:
>
> 1. **lnurl-wallet** stored a note's mint as a bare hostname
>    (`serverOf(callback)`), dropping the withdraw path. So the QR encoded
>    `lnurlw://mint.example?k1=...`, which resolves to the mint's landing page.
>    Fixed in dni/lnurl-wallet#26.
> 2. **This harness** reproduced the same bug, having copied the wallet rather
>    than the protocol — so even the real-sats note in section 17 carried an
>    unclaimable endpoint.
> 3. **notecase** threw `Invalid URL` on the schemeless `u` the vault writes,
>    into a `catch` that remembered nothing. A scan showed no error at all.
>    Fixed in forgesworn/notecase#3.
>
> `PROTOCOL.md`'s own examples had shown the bare form throughout, which is
> the likeliest reason the wallet was written that way. Also fixed here.
>
> **What this run exposes, and does not fix:** `unveil()` does not mark the
> note spent, so the board still lists the handed-over note as `CONFIRMED`
> and now overstates its holdings by exactly the note it gave away. Defensible
> — the device cannot know whether anybody scanned the screen, and marking it
> spent on display would be wrong for a QR nobody took — but the same note can
> be unveiled again and handed to a second person, who would find it already
> spent. Whether that wants an explicit "I handed this over" step is an open
> design question, not a bug, and this is the first run where it is more than
> theoretical.
>
> Only an iPhone camera was used. **Android is still NOT BENCH-RUN.**

## 11. Buttons

Pass: no spurious events at rest, and both buttons register.

Also check what the device says about its own inputs. `get_info` now carries
an `inputs` object (`docs/PROTOCOL.md`), and on a healthy board it must read
`{"confirm":"ok","cancel":"ok"}` within a few seconds of boot — `ok` means the
pin has been seen released, which is the only thing that can be proven without
a finger on the board. A board that reports `stuck` has a pin wedged low; see
section 7a. A board that reports `unknown` for more than five seconds after
boot is reporting a pin it has never once read released, which is the same
thing arriving slowly.

Note what this does **not** check: a disconnected button reads released
forever and reports `ok`. Proving a button works still needs a person to press
it, which is the row above.

> **Bench record — 2026-08-17.** Classic T-Display. `button_fsm` produced
> zero spurious events across 31s at rest. GPIO35 is input-only with no
> internal pull, and the board's external pull-up was confirmed present.

## 12. Persistence across power cycles

Mint and confirm notes, power-cycle the board, and confirm they are still
there with the right states.

> **Bench record — 2026-08-17.** Classic T-Display. Notes survived repeated
> reboots (software resets and EN-pin resets) with counts and states intact.
> A full unplug/replug cycle was not specifically isolated in this pass.

## 13. Storage exhaustion, and wipe

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

## 14. Crash reporting and the watchdog

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

## 15. OTA

Sign an image, push it with `tools/ota_push.py`, approve on the device, and
confirm it boots the new firmware and reports the new version.

> **Bench record — 2026-08-17.** Classic T-Display, `0.0.4-1-g23cd436`, using
> the genuinely signed `firmware.bin` from release v0.0.4. Only `ota_begin`
> was sent, never a chunk, so nothing was written to the OTA partition — and
> `ota_begin` verifies the signature *before* the owner is asked anything,
> which is precisely the half that can be checked without a press.
>
> | Sent | Result |
> |---|---|
> | tampered signature | `bad_signature` in 0.2s, no prompt |
> | genuine signature over a different digest | `bad_signature` in 0.2s |
> | the real v0.0.4 signature | accepted, prompt raised, timed out at 31.2s |
>
> The third line is the one that matters: the device's own ed25519
> verification, against the key compiled into it, accepted a signature made by
> the CI seed. The two ends of the release chain meet on hardware. The first
> two matter for a different reason — a bad image is refused *before* the
> owner is bothered, so it cannot train anyone into dismissing prompts.
>
> Note the image was an ESP32-S3 build offered to a classic ESP32. That is
> harmless here and deliberate: `ota_begin` checks the signature over the
> claimed digest and nothing else, so it exercises verification without the
> image ever being written. Do not extend this into a real transfer on the
> wrong chip.
>
> **Still NOT YET BENCH-RUN:** an approved transfer — `ota_chunk` streaming,
> `ota_finish` re-verifying the digest actually written, the partition switch,
> the reboot into the new image, and rollback. Everything past the owner's
> press is still only covered by `test/native/test_ota_dispatch.c` against a
> fake in-memory flash.

## 16. Release artefacts

Cut a tag, then check the published release carries `firmware.bin`,
`firmware.bin.sig`, `SHA256SUMS` and `manifest.json`; that the checksums
match; that `firmware.bin.sig` verifies against the committed public key; and
that the web installer flashes `merged-firmware.bin` onto a blank board which
then boots and reports the tag as its `fw_version`.

> **Bench record — 2026-08-17, release v0.0.4.** The first release cut since
> the pipeline was pinned and given checksums, and the first with a signing
> seed configured. Verified independently of CI, from the published artefacts:
>
> - all seven assets present, including `SHA256SUMS`, which v0.0.3 and earlier
>   do not carry
> - `shasum -a 256 -c SHA256SUMS` passes for all six files
> - `firmware.bin.sig` verifies against the public key compiled into the
>   firmware (`283761ec…62ee`), over the domain-separated message
>   `"lnurlvault-ota-v1" || 0x00 || sha256(firmware.bin)` that
>   `ota_signing_message()` builds — **not** the bare digest, which is what a
>   first attempt at this check verified against and wrongly reported as a
>   failure. Verify the message the firmware actually verifies.
>
> That the signature checks out against the committed key is the end-to-end
> proof that the signing seed and the shipped key are the same key — the thing
> `tools/check_release_key.py` asserts at build time, confirmed here against
> the artefact a device would actually be offered.
>
> **Still not bench-run:** flashing `merged-firmware.bin` onto a blank board
> via the web installer, and an actual OTA transfer of `firmware.bin` to a
> device. The signature is proven acceptable; nothing has yet accepted it.

## 17. End to end with a real mint

The gap nothing else covers. `lnurl-wallet`'s `deviceOrchestration.ts`
implements every LUD-25 operation against a paired vault, and every one of
them is verified only against mocks. The two-phase commit the whole design
rests on — device stages a secret and discloses only its hash, mint burns
against that hash, device commits — had never run with a real mint and a real
board in the loop.

`test/hardware/e2e_mint.py` now drives that chain directly, doing what the
browser would, so it can be run without a wallet build in the loop:

```
python3 test/hardware/fake_cln.py --port 9737 &
LNURL_MINT_ENV_FILE=/dev/null DATABASE_PATH=/tmp/refmint.db \
  FUNDINGSOURCE_BACKEND=cln FUNDINGSOURCE_URL=http://127.0.0.1:9737 \
  FUNDINGSOURCE_RUNE=fake BASE_URL=http://127.0.0.1:8111 \
  BASE_FEE_MSAT=0 MIN_MINT_MSAT=0 \
  uvicorn lnurl_mint.server:app --port 8111 &

python3 test/hardware/e2e_mint.py --port /dev/ttyUSB0 \
    --mint http://127.0.0.1:8111 --fake-cln http://127.0.0.1:9737
```

`fake_cln.py` exists because `lnurl-mint` will not mint or melt without a
funding source and supports only real cln and real lnd. It serves the seven
RPCs that mint actually calls. A mint whose invoices nothing can pay cannot
demonstrate a mint or a melt, which is most of this section.

The chain is one continuous lineage from a single note, because that is the
only arrangement where each step's output is proven spendable: every
operation consumes what the one before it produced.

| Operation | Expect |
|---|---|
| mint | a note from a paid `payRequest`, then `import_secret` + immediate rotate onto the device |
| rotate | old note `SPENT`, new note `CONFIRMED`, same value |
| split | two outputs confirmed, every input spent |
| merge | one output worth the sum, every input spent |
| melt | invoice paid, note `SPENT` |
| receive | an imported note rotated before it is trusted |

Pass: the plaintext secret leaves the vault only on a press, `list_notes`
agrees with the mint after each step, and nothing is left `PENDING`.

Do this on the **classic T-Display**, which is the board whose confirm gate is
known to work. Section 7a is why.

> **Bench record — 2026-08-20, fw `0.0.7` / `0.0.7-dirty`.** Classic
> T-Display, `/dev/cu.usbserial-5B310132921`. The two-phase commit ran with a
> real mint and a real board for the first time, against **two independent
> mints**, and the full chain is green on both.
>
> **dni's `lnurl-mint`** (branch `tests/bearer-threat-suite`, server code
> identical to main) backed by `fake_cln.py`: **mint** (paid a `payRequest`,
> took the preimage over LUD-21 `verify`), **import_secret**, **rotate after
> import**, **split** (21000 → 7000 + 14000, input spent), **merge** (both
> inputs spent, one output worth 21000 — value conserved), **rotate**,
> **melt** (invoice paid, note `SPENT`).
>
> **moneyer** `--dev` on `:3737`, from its startup seed note: 8 passed, 0
> failed — **import_secret**, **rotate after import**, **split**, **merge**
> (value conserved), **rotate**, **melt**. Its `mint` row is skipped rather
> than passed: its fake funding source mints deliberately unpayable invoices,
> so no fresh note can settle. Starting from a secret held outside the device
> and rotating it immediately is the **receive** row, so that is covered here
> even though `mint` is not.
>
> On both mints the device's `list_notes` lineage agreed with the mint at
> every step and nothing was left `PENDING`.
>
> The whole session cost far more approvals than it should have, for a reason
> worth recording: the approval is a **two-second hold**, nobody driving it
> knew that, and the screen did not say. Nine or so prompts died as timeouts
> that were read as a dead device. See section 19.

> **Bench record — 2026-08-20, real sats, fw `0.0.7-1-gf4ac981-dirty`.**
> Classic T-Display against **moneyer.dev**, a real mint on the public
> internet backed by a real node — no fake funding source anywhere in this
> run. The whole round trip, with money:
>
> 1. A 55,055 msat invoice from `moneyer.dev` paid from Wallet of Satoshi.
>    The mint fee (5,000 msat + 1,000 ppm) left a **50,000 msat note**.
> 2. The preimage taken over LUD-21 `verify` — the payer's wallet never
>    revealed it, which is the whole reason `verify` exists — and
>    `import_secret`ed onto the device.
> 3. **Rotated**, burning the mint-known preimage. `/w?k1=<preimage>` then
>    answered `Note already spent.`, so the burn is confirmed mint-side, and
>    the note now lives on a secret only this device has ever held.
> 4. **Melted** to a Wallet of Satoshi invoice for exactly 50,000 msat.
>    moneyer routed a real payment; **50 sats arrived in WoS**. Both notes
>    `spent` on the device, `pending_count` 0.
>
> Two things this run established that no local test could. The **melt
> settle discipline works against a real node**: `OK` came back when the note
> was reserved, and the device did not mark it spent until the mint confirmed
> the payment had actually landed. And both mints require the melt invoice to
> be for the note's **exact** value — the routing fee comes from the mint's
> own budget, funded by the mint fee, not out of the note. An invoice for the
> note minus headroom is refused by both.
>
> Cost of the exercise: ~6 sats, all of it mint fee and routing.

**A second mint.** `--mint http://127.0.0.1:3737 --seed-note <k1>` runs the
same chain against [moneyer](https://github.com/forgesworn/moneyer). It needs
`--seed-note` because its `--dev` funding source mints deliberately unpayable
invoices, so nothing can settle a fresh one; the 21 sat note it prints at
startup is the way in, and it is one note per start — a chain that ends in a
melt needs moneyer restarted before the next run.

**On the approvals.** Every `export_secret` is a **two-second hold of button
1**, not a tap (`approval.h`). This cost most of a bench session to learn the
hard way, from the wrong end: a tap leaves the progress bar empty and the
device looks like it is not listening. The confirm card now says
`HOLD BTN1 2s` on it for exactly that reason — see section 19.

## 18. Device identity — NOT YET BENCH-RUN

`identify` (issue #69) is what lets a wallet notice a swapped vault. Three
things need a board, and none of them are provable in a native test:

| Check | Expect |
|---|---|
| Two challenges, same boot | different signatures, **same** `pubkey` |
| Power-cycle, then challenge again | the same `pubkey` as before. A key that changes at every boot warns about a swap every time and trains people to dismiss it |
| A second board | a different `pubkey`. Two devices sharing one identity would be worse than having none |
| After an approved `wipe` | a different `pubkey`, deliberately: a wiped vault is a different vault to any wallet that pinned it |
| First boot on a blank device | a key is generated and stored, and `identify` answers rather than reporting `unsupported` |

Also worth one check with the wallet: pair, then present the other board, and
confirm the wallet says it is not the vault it paired with.

**NOT YET BENCH-RUN.**

## 19. The confirm card — PARTIALLY BENCH-RUN

What the screen says while it is asking. Every row here needs a person to
look at a panel, so none of it is reachable from `bench.py`.

The card used to show an amount, a label and a note id, and nothing about
either the verb or the gesture. Both gaps were found the same way, on a
bench run rather than by reading the code:

- **No verb.** An owner was asked to approve "21000 sats" with no statement
  of what was about to happen to it. `export_secret` and `wipe` — disclose
  one note, or erase every note on the device — presented as the same flat
  amber card, since the wipe prompt had no note to show and so drew no text
  at all. `ui_task_request_action_confirm` had taken an `action` string since
  before this and discarded it.
- **No gesture.** Approving is a two-second hold of button 1
  (`APPROVAL_HOLD_US`), deliberately, since a tap "is one accidental brush of
  a pocket button away". Nothing on screen said so. The observed behaviour of
  a person who taps is to conclude the device is not listening and stop — which
  is what happened, repeatedly, before anyone thought to re-read `approval.h`.

| Check | Expect |
|---|---|
| `export_secret` | `SHOW SECRET` above the amount, `HOLD BTN1 2s` below it |
| A gated destructive command | its own verb — `MARK SPENT`, `DISCARD`, `RENAME`, `DELETE` — not the disclosure card |
| `wipe` | `WIPE ALL`, on a card that is no longer bare colour |
| OTA | `NEW FIRMWARE`, likewise |
| Holding button 1 | the bar fills over 2s and the card resolves green |
| Tapping button 1 | the bar visibly starts and drops back, rather than nothing happening |
| Browsing a note | unchanged: no verb, no hint, note id still shown |
| A seven-digit amount | still readable; the digits shrink to make room, no line falls off the bottom |

The note id is deliberately **not** on the confirm card any more. Eight hex
characters identify a note to a wallet, not to the person holding the device,
and the row it occupied buys the gesture hint instead. It is still on the
browse card, where choosing a specific note is the whole point.

Both panels need checking: the 240x135 classic is the tighter fit, and the
verbs are capped at 12 characters because that is what fits across it at
`FONT5X7_MIN_READABLE_SCALE`. A longer verb clips rather than shrinking.

> **Bench record — 2026-08-20, fw `0.0.7-dirty`.** Classic T-Display,
> photographed. `export_secret` renders four lines and the bar:
> `SHOW SECRET` / `2100` / `msat  card-c` / `HOLD BTN1 2s`. A gated
> destructive command renders `RENAME` in the same place, so the verbs do
> differentiate. Holding button 1 approves; the `RENAME` card was approved by
> hold during the same session.
>
> **The hint did not fit on the first attempt**, and the first build of this
> card shipped without it — the one line the whole change existed to add. Four
> lines at the readable minimum put it at y=103 against a `usable_h` of 102,
> because the reservation budgeted a 4px gap per line while the amount
> advanced by 5. One pixel. It was found by photographing the panel, not by
> reading the code, and not by the build, which was perfectly happy. There is
> no slack on a 135px panel to absorb a mismatch like that, so the gap is now
> a single named constant used by both the reservation and the advance.
>
> The label also used to clip mid-glyph (`card-check` drew as `card-ch` with
> the h sliced down the middle, which reads as a different label rather than a
> shortened one). It is now cut to what the width holds.
>
> **Still unverified:** the bar visibly filling and dropping back on a tap
> rather than a hold; the `WIPE ALL` and `NEW FIRMWARE` cards, neither of
> which has been put on a panel; a seven-digit amount; and **the whole of this
> on the S3**, whose larger panel takes a different branch of the fitting
> logic (the amount grows to scale 5-6 there rather than staying at 3).
>
> **That record is stale, and the commit after it broke the card again.**
> Reserving room for the unit/label and id lines was what made all four fit
> above, and it did so by holding the digits to 21 pixels — the height a
> person on this same bench had already called too small to read. Removing the
> reservation gave the digits their room back and handed the hint's row to the
> unit/label line, so `HOLD BTN1 2s` was once more computed, budgeted for and
> never drawn: on the 240x135 panel, on every card that has a label, which is
> every real one. The S3 was unaffected, which is why nothing on the bench
> caught it either.
>
> Nobody found this on a board. It was found by `make preview` (see
> [Verification](../README.md#verification)) on the very first run, and the
> assertion that now holds it down is in `test/native/test_card_render.c` —
> which draws with the firmware's own `display.c` at the real panel geometry
> and counts pixels, rather than re-deriving the arithmetic and agreeing with
> it. Three display faults have now shipped in this project; all three were
> layout, none was reachable from the build, and this is the first one caught
> before anyone had to look at hardware.
>
> The underlying problem was that 102 usable rows do not hold four readable
> lines and their gaps, so the two fixes traded the same pixels back and
> forth. The bar was taking a fifth of the panel — an eighth of the height,
> plus a twelfth again as bottom margin. It is now `h/16` (8 rows on the
> classic, 10 on the S3), which is still an obvious bar at 180px wide, and the
> card has 118 rows to work with. All four lines fit at once, with the digits
> at scale 4-5 rather than 3. The hint is also pinned to the bottom of the
> card now rather than laid out after whatever precedes it: it is the one line
> that must never be the one that falls off.
>
> **Wants a bench run:** the slimmer bar on real glass, both panels — the
> geometry is only checked against a framebuffer.

## 20. What the screen says the rest of the time — NOT YET BENCH-RUN

Section 19 is the card that asks. This is everything else, and until now all
of it was a flat colour: boot, rest, and the answer.

An outcome was a green, red or grey rectangle held for 800ms. Those are only
meaningful to somebody who already knows the scheme, and the one that matters
most — a prompt that timed out — is a grey nobody has ever been taught. The
idle screen was a dark rectangle, which is the screen a person looks at for
hours and which says nothing about whether the device is alive, paired, or
holding anything; the observed response to it was pressing buttons to find
out, on a device where a press starts browsing bearer secrets.

| Check | Expect |
|---|---|
| Power on | `LNURL VAULT`, the firmware version, and the board name, before anything else draws |
| At rest, notes on the device | `3 NOTES` (or `1 NOTE`) over `TAP TO VIEW`, white on dark grey |
| At rest, empty vault | `NO NOTES` over `PAIR TO ADD` |
| Approve a disclosure | `APPROVED` over `SHOW SECRET`, green |
| Cancel with button 2 | `DECLINED` / `SHOW SECRET` / `NOTHING DONE`, white on red |
| Let a prompt time out | `NO ANSWER` / the verb / `NOTHING DONE`, on grey |
| Outcome timing | the wallet gets its answer FIRST; the card stays up ~1.8s afterwards |
| Hold button 1 before the prompt appears | the card says `LET GO FIRST`; releasing switches it to `HOLD BTN1 2s` |
| Unveil a note that cannot be shown | `FAILED` over `NOT SHOWN`, not a bare red flash |
| Idle after browsing times out | back to the note count, not a blank screen |

The last two rows are the ones worth being awkward about. The `LET GO FIRST`
path needs a button held down at the moment the host sends the command, which
takes two people, or a scripted `export_secret` and a finger already on the
button. The unveil failure needs a note deleted over the wire between
selecting it and chording — or, more easily, a URL too long for the QR
versions this panel can draw.

The idle screen shows how many notes and **not** what they are worth. A vault
sitting on a desk announcing its balance to the room is a different device
from one that makes you ask; the amounts are one deliberate press away.

Every one of these screens can be looked at without a board:

```sh
cd test/native && make preview
```

That is not a substitute for the bench. A framebuffer cannot tell you whether
white on `#7800F8` is readable in daylight, or whether 1.8 seconds is long
enough to read three lines while your finger is still coming off a button. It
is a substitute for guessing.

> **Not yet bench-run.** Every row above comes from the preview renderer and
> the pixel assertions in `test/native/test_card_render.c`. None of it has
> been on glass.
