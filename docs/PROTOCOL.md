# lnurl-vault wire protocol

The device is a command server; a paired browser (or any other host) is the
client. Every command gets exactly one response. The protocol is identical
over both transports — only the framing around a JSON message differs.

This protocol is deliberately narrow: the device does no networking and
knows nothing about LNURL/mint HTTP calls. It only ever generates secrets,
discloses their SHA-256 hash, and tracks note state. See "Orchestration"
below for how a browser client (e.g. a future `lnurl-wallet` integration)
composes these commands with calls to a mint to implement rotate/split/
merge/melt, per [LUD-25](https://github.com/lnurl/luds/pull/301).

## Transports

### WebSerial (USB-CDC)

Newline-delimited JSON: one command object per line in, one response object
per line out (`\n`-terminated). The device's native USB presents as a
standard CDC-ACM serial device — no drivers, `navigator.serial` opens it
directly.

### BLE (NimBLE GATT)

One custom service, two characteristics (see `src/transport/ble_gatt.c` for
the exact 128-bit UUIDs):

| Characteristic | Properties | Direction |
|---|---|---|
| RX | write, write-without-response | browser → device: command chunks |
| TX | notify | device → browser: response chunks |

GATT's negotiated MTU is small and JSON messages can exceed it, so both
directions wrap a complete message in a trivial framing:

```
[2-byte little-endian total length][message bytes...]
```

split across as many writes/notifications as needed; the device (and the
browser client) reassemble by accumulating bytes until the declared length
is reached. The length header only appears on the *first* chunk of a
message, not every chunk.

## Commands

Every response has a boolean `ok`. On failure: `{"ok":false,"error":"<code>","message":"..."}`
(`message` is optional, human-readable). Error codes: `not_found`,
`invalid_state`, `user_declined`, `timeout`, `storage_full`, `bad_request`,
`response_too_large`.

`storage_full` has two causes, and they want opposite responses from the
owner. Either the vault is genuinely out of room, or storage is degraded and
the device is refusing to write rather than risk what is already there. Read
`get_info`'s `storage` field to tell them apart: `full` means spend or delete
notes; `index_unreadable` means reboot, and specifically do **not** wipe.

`response_too_large` means the reply did not fit the transport's response
buffer. Today only `list_notes` can produce it (every other command's reply
is a fixed set of fields). A client should treat it as "ask for less", not as
a device fault — but note there is currently no way to ask for less, so a
vault holding more notes than fit cannot be listed at all. Paging is the
outstanding fix.
plus two OTA-specific codes (see `ota_begin`/`ota_finish` below):
`bad_signature`, `ota_failed`.

### `get_info`

```json
{"cmd":"get_info"}
→ {"ok":true,"fw_version":"0.1.0","board":"t-display-s3","note_count":3,"pending_count":1}
```

`board` identifies the hardware and therefore the pin map -- useful in a bug
report, and for a client that wants to warn about a build it does not
recognise. Absent on a build with no board identifier compiled in.

`storage` says whether the device can actually read its own notes. **A client
must check it before concluding a vault is empty**, because these are not the
same situation:

| Value | Meaning |
|---|---|
| `ok` | storage is working |
| `full` | the NVS partition is out of free pages. Every note is still on flash and none is readable. Not corruption -- ordinary churn reaches this, since a note blob is 448 bytes and every `confirm`, `rename` or `mark_spent` rewrites one |
| `version_unsupported` | flash was written by a newer NVS format than this firmware understands. A downgrade, not damage; a correct firmware could still read it |
| `unavailable` | storage could not be brought up at all |
| `index_unreadable` | NVS came up fine, but the note index itself could not be read this boot. The notes are still on flash; the device just cannot say which ones exist. It refuses to create notes or to rewrite the index until a boot that can read it, so `new_secret` and `import_secret` answer `storage_full`. Recovery is a **reboot** -- never a `wipe`, which would destroy exactly what this state exists to protect |

Anything other than `ok` means `note_count` is **not** a statement about how
many notes exist -- it is how many the device could load. The firmware never
erases to recover from any of these; see `wipe`.

The field is absent on a build with no persistent storage.

### `list_notes`

```json
{"cmd":"list_notes"}
→ {"ok":true,"total":3,"offset":0,"notes":[ ... ]}
```

`state` is one of `pending`, `confirmed`, `spent`. `sig` is present only if
the note carries an offline-verification signature ([LUD-25](https://github.com/lnurl/luds/pull/301)).

`total` is how many notes the device holds. `offset` is where this page
started. **A client must not treat the length of `notes` as the number of
notes that exist** — that is what `total` is for.

`next_offset` is present only when there are more notes past this page:

```json
{"cmd":"list_notes","offset":14,"limit":10}
→ {"ok":true,"total":40,"offset":14,"notes":[ ... ],"next_offset":24}
```

Page by feeding `next_offset` back as `offset` until it stops appearing.

Both arguments are optional. Omitting them returns **as many notes as fit in
one response**, plus a `next_offset` if that is not all of them. This is the
only response in the protocol whose size depends on stored data, and it is a
single fixed buffer, so how many fit depends on the notes: measured against a
128-note vault, 27 per page when notes carry no `sig`, 14 when they all carry
a full-length one.

An explicit `limit` is honoured or refused, never silently reduced:

| Request | Reply |
|---|---|
| no `limit` | as many as fit, with `next_offset` if more remain |
| `limit` that fits | exactly that many |
| `limit` too large to fit | `response_too_large` — ask for fewer |
| `limit: 0` | no notes, but `total` still reported |
| `offset` equal to `total` | a valid empty page, no `next_offset` |
| `offset` past `total` | `bad_request` |

A refused `limit` is deliberately not shrunk for you. A client that asked for
fifty and silently received fourteen would build a wrong picture of the vault,
which is the same class of failure as the truncation this replaced.

### `new_secret`

Generates one fresh secret on-device, stores it `PENDING`, and discloses
only its hash. Used for **rotate** (one parent) and **merge** (many
parents).

```json
{"cmd":"new_secret","parent_ids":["a1b2c3d4"],"label":"optional"}
→ {"ok":true,"id":"e5f6a7b8","h":"<64-hex sha256>"}
```

`parent_ids` and `label` are both optional.

### `new_secret_pair`

Generates two fresh secrets sharing the same parent lineage, for **split**.

```json
{"cmd":"new_secret_pair","parent_ids":["a1b2c3d4"]}
→ {"ok":true,"id":"...","h":"...","id2":"...","h2":"..."}
```

### `confirm`

Commits a `PENDING` note to `CONFIRMED` once the mint replied
`{"status":"OK"}`.

```json
{"cmd":"confirm","id":"e5f6a7b8","amount_msat":21000,"host":"mint.example","sig":"optional hex"}
→ {"ok":true}
```

### `discard`

Drops a `PENDING` note the mint rejected.

```json
{"cmd":"discard","id":"e5f6a7b8"}
→ {"ok":true}
```

### `export_secret`

Reveals a `CONFIRMED` note's raw secret as hex — **gated by a physical
confirm/cancel on the device** (30s timeout). This is the only command that
ever discloses a plaintext secret.

```json
{"cmd":"export_secret","id":"e5f6a7b8"}
→ {"ok":true,"k1":"<64-hex secret>"}
→ {"ok":false,"error":"user_declined"}
→ {"ok":false,"error":"timeout"}
```

### `import_secret`

Registers an externally-known secret directly as `CONFIRMED` — for a note
received from someone else, or a fresh Lightning payment preimage from
minting a new note.

```json
{"cmd":"import_secret","k1":"<64-hex secret>","host":"mint.example","amount_msat":21000,"label":"optional"}
→ {"ok":true,"id":"c9d0e1f2"}
```

### `mark_spent`

Transitions `CONFIRMED` → `SPENT`, once the browser confirms a melt settled,
or as the burn step of a rotate/split/merge.

```json
{"cmd":"mark_spent","id":"e5f6a7b8"}
→ {"ok":true}
```

### `rename` / `delete`

```json
{"cmd":"rename","id":"e5f6a7b8","label":"new label"}
→ {"ok":true}

{"cmd":"delete","id":"e5f6a7b8"}
→ {"ok":true}
```

`delete` only succeeds on a `SPENT` note (housekeeping) — a `PENDING` note is
dropped via `discard`, and a `CONFIRMED` one must be spent first.

### `reset`

Reboots the device. Not part of note lifecycle management — a recovery
tool. This device has a real, still-unresolved issue where individual
serial responses occasionally arrive late or not at all, and it's observed
to get *worse* the longer a boot runs without a power cycle (see the
firmware repo's README.md Status section); `reset` gives a remote client a
way to force that power cycle without physical access to the board.

```json
{"cmd":"reset"}
→ {"ok":true}
```

The device responds *before* rebooting, on a short delay (currently 10s in
`main.c`) rather than immediately — long enough that the response has a
real chance to actually leave the TX buffer first, even under this
device's worst documented latency. Don't rely on the exact delay; treat
the connection as gone once you've gotten the response, and expect to
reconnect (USB re-enumerates; a BLE central needs to reconnect) after a few
seconds. No note state changes and nothing is disclosed — this is a
software reboot, not a factory reset.

### `wipe`

Erases every note, irreversibly. The only command that destroys value, and
the only thing in the firmware that erases storage at all.

```json
{"cmd":"wipe","confirm":"WIPE"}
→ {"ok":true,"wiped":true}
```

`"confirm":"WIPE"` is required verbatim. A bare `{"cmd":"wipe"}` returns
`bad_request` and does nothing. That is not the security control -- the
physical confirmation below is -- it is there because `wipe` sits in the same
command namespace as `get_info`, reachable by anything already paired, and a
bare command is far too easy to emit by accident from a retry loop or a
mistyped script.

The device then asks its owner to confirm physically, on-screen, using the same
on-device confirmation `export_secret` uses. Possible replies:

| Reply | Meaning |
|---|---|
| `{"ok":true,"wiped":true}` | erased **and verified** empty; a reboot follows on the usual delay |
| `user_declined` | the owner refused. Nothing erased |
| `timeout` | nobody answered. Nothing erased |
| `wipe_failed` | the erase, or the verification after it, failed. **Nothing has been reported as gone** -- the device keeps whatever survived and does not reboot |
| `unsupported` | this build has no storage to wipe, or no way to confirm on-device. A wipe that cannot be confirmed is refused rather than granted |

`wipe_failed` is the reply to take seriously. Erasing is not the hard part;
being able to prove it worked is. The device re-initialises and re-reads after
erasing, and reports failure rather than success if anything is still
readable -- because the owner acts on a success claim, by selling or handing on
the device. Treat `wipe_failed` as "this device still holds secrets".

Note state and secrets are gone from RAM as well as flash, and only in that
order: flash is erased and verified first, because until the verification
passes RAM holds the only intact copy.

### `ota_begin` / `ota_chunk` / `ota_finish`

Firmware updates over this same JSON-over-serial protocol — no WiFi, no
separate flasher tool once a device is out in the field. Adapted from
[forgesworn/heartwood-esp32](https://github.com/forgesworn/heartwood-esp32)'s
OTA design (see the firmware repo's README.md OTA section for the full
story): every image must carry an ed25519 signature from this project's
release key, checked twice — once at `ota_begin` over the *claimed* digest,
before the owner is ever bothered, and again at `ota_finish` over the
digest actually written to flash. Use
[`tools/ota_push.py`](../tools/ota_push.py) rather than hand-rolling this —
it handles signing, base64 chunking, and retries.

```json
{"cmd":"ota_begin","size":581513,"sha256":"<64-hex>","signature":"<128-hex>"}
→ {"ok":true}
→ {"ok":false,"error":"bad_signature"}          // signature doesn't verify — rejected before the owner is asked anything
→ {"ok":false,"error":"user_declined"}          // physical confirm/cancel, same as export_secret
→ {"ok":false,"error":"timeout"}                // no button press within 30s
→ {"ok":false,"error":"bad_request"}            // size missing/out of range, or sha256/signature aren't 64/128 hex chars
→ {"ok":false,"error":"ota_failed"}             // could not open the OTA partition
```

A new `ota_begin` implicitly discards any abandoned prior session (a host
that crashed or gave up mid-transfer) rather than requiring a device reset
just to retry.

```json
{"cmd":"ota_chunk","offset":0,"data":"<base64, up to 1024 raw bytes>"}
→ {"ok":true}
→ {"ok":false,"error":"invalid_state"}   // no active session — call ota_begin first
→ {"ok":false,"error":"bad_request"}     // wrong offset, malformed base64, chunk too large, or it would exceed the declared size
```

Chunks must arrive strictly in order — `offset` must exactly equal the
number of bytes already received. There's no reassembly buffer and no
random access; a host retrying a stalled chunk just resends the same
(already-next-expected) offset. A wrong offset is rejected but does *not*
abort the session — the correct next chunk still succeeds. A chunk that
would push past the declared `size`, or a flash write failure, does abort
it.

```json
{"cmd":"ota_finish"}
→ {"ok":true}
→ {"ok":false,"error":"invalid_state"}    // no active session
→ {"ok":false,"error":"bad_request"}      // fewer bytes received than declared size
→ {"ok":false,"error":"bad_signature"}    // the digest of what was actually written doesn't match the signed one — a torn or corrupted transfer
→ {"ok":false,"error":"ota_failed"}       // esp_ota_end / esp_ota_set_boot_partition failed
```

Any `ota_finish` failure aborts the session — the currently running
firmware is never affected until every check above passes. On success, the
device responds `{"ok":true}` and reboots into the new image on the same
short delay `reset` uses (see above), for the same reason: so the response
has a real chance to leave the TX buffer first.

## Orchestration

How a browser client composes these commands with mint HTTP calls to
implement each [LUD-25](https://github.com/lnurl/luds/pull/301) operation. The device never talks
to the mint itself.

**Rotate** (burn one note, mint a fresh one of the same value):
1. `export_secret(old)` → `k1`
2. `new_secret(parent_ids=[old])` → `id, h`
3. `GET callback?k1=<k1>&h=<h>` on the mint
4. On `{"status":"OK"}`: `confirm(id, amount_msat, host, sig?)`, then `mark_spent(old)`
5. On error: `discard(id)`

**Split** (burn one or more notes, mint two: `amount` and the remainder):
1. `export_secret` each input note
2. `new_secret_pair(parent_ids=[inputs])` → `id, h, id2, h2`
3. `GET callback?k1=<k1>&...&amount=<msat>&h=<h>&h2=<h2>`
4. On OK: `confirm` both outputs, `mark_spent` each input
5. On error: `discard` both outputs

**Merge** (burn many notes, mint one worth their sum):
1. `export_secret` each input note
2. `new_secret(parent_ids=[inputs])` → `id, h`
3. `GET callback?k1=<k1>&k1=<k1>...&h=<h>`
4. On OK: `confirm(id, ...)`, then `mark_spent` each input
5. On error: `discard(id)`

**Melt** (redeem to a BOLT-11 invoice, no new secret):
1. `export_secret(old)` → `k1`
2. `GET callback?k1=<k1>&pr=<bolt11>`
3. Once the mint's payment settles (poll `verify` if offered): `mark_spent(old)`

**Minting** (paying a `payRequest` to create a brand-new note): happens
entirely off-device — the browser pays the invoice and gets a payment
preimage `P`. To bring `P` under this device's custody: `import_secret(P,
...)`, then immediately **rotate** it (per [LUD-25](https://github.com/lnurl/luds/pull/301)'s
security considerations — a payment preimage was also seen by the mint
itself as a prior holder).

**Receiving a note from someone else** (offline handoff): `import_secret`
with the received `k1`, then immediately **rotate** it, closing the window
in which the previous holder could also redeem it.

## On-device note browsing

Independent of the commands above and not exposed over serial/BLE at all —
this is a purely physical, local interaction (`src/ui/ui_task.c`) for
handing a note to someone else in person, the "offline circulation" case
[LUD-25](https://github.com/lnurl/luds/pull/301) itself describes. No browser or paired host is
involved in this flow.

| Gesture | Effect |
|---|---|
| Tap either button (idle) | Enter browse mode at the first `CONFIRMED` note |
| Tap button 1 / button 2 (browsing) | Next / previous `CONFIRMED` note (wraps around) |
| Hold both buttons together for ~200ms ("the chord") | **Unveil**: exports the selected note's secret and shows its `lnurlw://` URL as a QR code on-screen |
| Any tap while a QR is shown | Dismiss it, back to browsing |
| ~15s with no input while browsing | Back to idle |

Only `CONFIRMED` notes are browsable (a `PENDING` note has no settled value
yet; `SPENT` notes have nothing left to show). There's no on-screen text yet
(see README.md's "Known limitations"), so the display blinks the selected
note's 1-based position among `CONFIRMED` notes instead of printing a
number.

The chord *is* the confirmation — unlike `export_secret` over serial/BLE,
there's no separate confirm/cancel step, because reaching this point already
required physically holding the device and deliberately pressing both
buttons together. Once shown, the QR **is** the bearer secret in the clear;
anyone who can see the screen can scan and redeem it, exactly like handing
over a banknote. Dismissing it overwrites the screen (and the URL buffer
holding the secret is explicitly zeroed in RAM) but there is, deliberately,
no way to re-display the same QR without repeating the gesture.

This and a remote `export_secret` request share the same physical
buttons/display, arbitrated by a single owning task — see `ui_task.c` and
`vault_lock.h`'s header comments for how the two are kept from ever reading
the buttons concurrently.
