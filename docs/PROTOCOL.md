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

A command may carry a `tag`: a non-empty string of at most 32 characters,
which the device echoes verbatim as `tag` on the reply, whatever the reply is
(a success, an error, or each page of `list_notes`):

```json
{"cmd":"get_info","tag":"a1"}
{"ok":true,"fw_version":"0.0.12",...,"tag":"a1"}
```

The tag is what lets a client survive a lost or torn reply without tearing
the session down. Without one, a reply that never arrives is indistinguishable
from a slow one, and a late reply from the reply to the next command, so a
client's own timeout had to be fatal. With one, a client can keep the stream
open after a timeout, retire the straggler by its tag when it does show up,
and retry an idempotent command (`get_info`, `list_notes`, `identify`) whose
reply arrived unparseable. Commands are still answered one at a time, in order.

A `tag` that is not a string, is empty, or is longer than 32 characters is
refused with `bad_request`, and that refusal carries no tag: a tag echoed
truncated or coerced would match nothing the client sent. A line that could
not be parsed as JSON at all is answered without a tag for the same reason.

A client that sends no tags sees the wire exactly as before. For it the old
rule stands: one command in flight at a time, and after its own timeout it
must not send another on the same stream until the late line arrives or the
connection is reopened, because a late reply is then indistinguishable from
the new command's reply.

If USB-CDC stops accepting bytes after a response has partly left the device,
the firmware abandons that reply after a bounded wait and records
`drops.tx_stalled`. Before a later reply it emits a standalone newline to
restore framing, so the torn prefix and the later valid JSON cannot be glued
into one line. This repairs the stream; it does not make the abandoned reply
successful, and the original command remains indeterminate to its caller.

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
`response_too_large`, `display_unavailable`, `unsupported`, `wipe_failed`, and
two OTA-specific codes (see `ota_begin`/`ota_finish` below): `bad_signature`,
`ota_failed`.

**Physically-gated commands.** `export_secret`, `mark_spent`, `rename`,
`delete`, `discard`, `wipe` and `ota_begin` take effect only after an
on-device confirmation, so beyond their own success reply each can also return
`user_declined`, `timeout`, `display_unavailable`, or `unsupported` (no
confirmation hook available). A client must handle a refusal, not assume
`{"ok":true}`.

`unsupported` means the command cannot be honoured on this device as
configured — a gated command with no on-device confirmation wired, or `reset`
sent over BLE (it is serial-only).

`display_unavailable` means the device could not ask its owner: the panel
never came up, so there is nothing to show and no informed consent to be had.
It is deliberately distinct from `user_declined` — nobody declined, and a
client that cannot tell the difference sends its owner hunting for a button
they were never shown. Every physically-gated command can return it. Recovery
is a power cycle, or a device whose display is repaired; see `export_secret`.

`storage_full` has two causes, and they want opposite responses from the
owner. Either the vault is genuinely out of room, or storage is degraded and
the device is refusing to write rather than risk what is already there. Read
`get_info`'s `storage` field to tell them apart: `full` means spend or delete
notes; `index_unreadable` means reboot, and specifically do **not** wipe.

`unsupported` also covers `identify` on a build with no device identity.

`response_too_large` means the reply did not fit the transport's response
buffer. Today only `list_notes` can produce it (every other command's reply is
a fixed set of fields). Treat it as "ask for less": `list_notes` takes
`offset`/`limit` to page through a vault larger than one response (see below).

### `get_info`

```json
{"cmd":"get_info"}
→ {"ok":true,"fw_version":"0.0.6","board":"t-display-s3","note_count":3,"pending_count":1}
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

`capabilities` says what this device is physically able to do, so a client
does not have to guess:

```json
"capabilities":{"buttons":2,"touch":false,"gated":true,
                "display":{"width":320,"height":170},"transports":["serial","ble"]}
```

| Field | Meaning |
|---|---|
| `buttons` | buttons wired for confirm/cancel: `2`, `1` or `0`. Not buttons present — a board whose second button is unreachable reports `1`, because that is what the gesture has to work with |
| `touch` | the panel takes touch input |
| `gated` | this build has an on-device confirmation wired. **`false` means every physically-gated command will answer `unsupported`** — worth telling the owner before they try one, not after |
| `display` | usable pixels, after the board's own rotation. **Zero when the panel did not come up**, which is how a client knows a QR handoff is not available on this device right now |
| `transports` | which of `serial` / `ble` this build serves |

The point of the field is the sentence a client puts in front of a person.
"Hold the button on your vault to approve" is right for two buttons and wrong
for a touchscreen; "press cancel" is meaningless on a board that has one
button. A client that reads `capabilities` can say the true thing.

The object is absent on a build that cannot describe its own hardware — which
is not the same as a build with no hardware, so do not read a missing
`capabilities` as "no buttons and no screen".

`inputs` says whether this device's own buttons can be believed. Present
whenever the firmware can observe them at all — its **absence** means the
build cannot report on its inputs, not that they are healthy:

```json
{"ok":true,"fw_version":"0.0.7","board":"t-display-s3","inputs":{"confirm":"ok","cancel":"stuck"}}
```

| Value | Meaning |
|---|---|
| `ok` | the pin has been seen released at least once, so it is not wedged |
| `stuck` | pressed continuously since boot, past any plausible person holding it. Something is wrong with this pin |
| `unknown` | pressed since boot, but not yet for long enough to call |

A field is **absent** for a button this board has not got — a one-button
vault reports `confirm` and no `cancel`. That is not the same as `unknown`,
and a client must not tell someone to press a button that is not there.

A `stuck` input is **not** a security problem — a button that has not been
seen released since a prompt began cannot answer that prompt, so a wedged
cancel line can no longer refuse anything (and a wedged confirm line can no
longer approve anything). It is a **usability** problem, and a client should
say so plainly: a vault reporting `"cancel":"stuck"` has lost its cancel
button, so every gated command on it can now only be approved or left to time
out. Telling the owner that beats leaving them to press a dead button for
thirty seconds and conclude the firmware is broken.

Note what `ok` does not claim. Reading a pin released proves it is not wedged
low; nothing proves a button is *connected*, because a disconnected one reads
released forever and looks perfect. `ok` means "not stuck", not "works".

Two more groups of fields appear when their source is compiled in.
`free_heap_bytes` reports the current free heap (a health signal). And after an
unexpected reset the device reports how the previous boot ended, for diagnosing
a field reset over the wire — on a board whose console is disabled this is the
only channel: `last_reset_reason` (`poweron`, `panic`, `sw`, …), `boot_count`,
`last_boot_unexpected` (bool), and `last_cmd_in_flight` (the command in flight
at the crash, if any).

`drops` counts the messages **the link this command arrived on** has given up
on since boot, and exists because every one of those is otherwise invisible:

```json
"drops":{"rx":0,"tx":0,"tx_stalled":1}
```

| Field | Meaning |
|---|---|
| `rx` | commands dropped before the dispatcher ever saw them — the transport's queue was full, or a write was too large to reassemble |
| `tx` | responses dropped before a byte went out — the queue was full, or (BLE) nothing was subscribed to be notified |
| `tx_stalled` | responses abandoned part-written, after the transport stopped making progress. The client may hold a **torn** line rather than nothing |

All three present the same way at the far end: a command that never resolves.
The client's own timeout fires, and a client that treats that as fatal — as
`lnurl-wallet` does — tears the session down, leaving "the vault
disconnected" as the whole of what anyone can report. These numbers survive
the reconnect, so asking `get_info` afterwards says which path swallowed it.

That matters most over WebSerial on the T-Display-S3, where the firmware's own
warnings for these three go to UART0 and a host on the USB-C cable cannot see
them at all.

Counters are cumulative since boot and there is no way to reset them: one a
client could zero is one that disagrees with the next client.

The object is **absent** for a transport that keeps no tally of its own — the
classic board's UART has no drop paths — rather than reporting three zeroes
nothing measured. A missing `drops` means "this build cannot say", never "this
link has lost nothing".

`usb` reports what the native USB bus itself has done since boot, as distinct
from what the firmware gave up on:

```json
"usb":{"configured":1,"unconfigured":0,"suspends":0,"resumes":0,"tx_xfers":12}
```

| Field | Meaning |
|---|---|
| `configured` | the host finished enumerating the device and applied a configuration. Once per boot in a healthy session; **more than once means the link was torn down and rebuilt at USB level** (a bus reset, a re-plug, a driver reload), not merely closed by an app |
| `unconfigured` | the host withdrew the configuration, or VBUS was lost on a board that senses it |
| `suspends` | the bus went idle for 3 ms: the host suspended the port, or, on a board that senses no VBUS, the cable went |
| `resumes` | the bus came back from suspend |
| `tx_xfers` | CDC IN transfers the USB controller reported complete. A host that received fewer bytes than those transfers carried has a loss **below** the firmware, where no `drops` counter can see it |

This exists because "it keeps disconnecting" is indistinguishable, from the
far end, between a host that re-enumerated the vault and a wallet that closed
the port on its own timeout. Unlike `drops` it is **not** per link: a BLE
client is exactly who is still connected to ask why the serial link keeps
dying. Cumulative since boot, never reset.

The object is **absent** on a board with no native USB. The classic T-Display
sits behind a USB-UART bridge chip the firmware cannot observe, so it says
nothing rather than five zeroes.

### `identify`

Challenge-response over a per-device key, so a client can tell one vault from
another ([issue #69](https://github.com/dni/lnurl-vault/issues/69)).

```json
{"cmd":"identify","nonce":"<16-32 bytes of hex, chosen by the client>"}
→ {"ok":true,"pubkey":"<64-hex ed25519 public key>","sig":"<128-hex signature>"}
```

The signature is over `"lnurlvault-id-v1" || 0x00 || nonce`, domain-separated
the same way OTA images are, so an identity challenge can never be replayed as
a firmware approval.

**The client picks the nonce, every time.** A fixed one turns this into a
recording anything can replay. The device refuses a nonce shorter than 16
bytes (too little to stop precomputation) or longer than 32 (a challenge must
not become an oracle for signing something else), with `bad_request`.

What it proves: the thing answering now holds the same key as the thing that
answered before. That is enough for trust-on-first-use — pin the `pubkey` on
first pair and warn loudly if it ever changes.

What it does **not** prove: anything about what the device is, or who has it.
It is not a defence against someone holding the vault. Physical possession is
still the model.

The key is **not** a note secret: it never signs a spend and never leaves the
device. `wipe` destroys it along with everything else, so a wiped vault is
deliberately a *different* vault to any client that had pinned it — the right
answer for a device that has been sold or handed on.

`unsupported` means this build has no identity, or the device has one it could
not store. A device that cannot remember its key does not serve it, because a
client pinning a key that changes at every boot would be warned about a swap
every single time.

### `list_notes`

```json
{"cmd":"list_notes"}
→ {"ok":true,"total":3,"offset":0,"notes":[
    {"id":"a1b2c3d4","h":"<64-hex sha256>","state":"confirmed","amount_msat":21000,"label":"",
     "host":"mint.example","parent_ids":[],"created_at":1234,"updated_at":1234}
  ]}
```

`h` is always `sha256(k1)` and is safe to use for matching a staged output
without exporting its secret.

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
only its hash. Used for **rotate** (one parent), **merge** (many parents),
and device-bound minting (no parents).

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

Commits a `PENDING` note to `CONFIRMED` once the companion has authenticated
the mint's success response. For rotate/split/merge that is the successful
mutation response. For the additive device-bound mint extension it is the
settled receipt whose `h`, exact net `amount`, invoice and LUD-25 signature
all match the quote. A bare LUD-21 settlement response does not authenticate
which device hash received the value and is not enough to confirm a pending
vault note.

**`host` is the withdraw endpoint's base URL, path included** —
`mint.example/w`, not `mint.example`. Despite the field's name it is not a
hostname: the device stores it verbatim and rebuilds note URLs from it, so a
bare host produces `lnurlw://mint.example?k1=...`, which points at the mint's
root and is not a note. LUD-25 puts it plainly — "lnurlw://mint.example/w?k1=
<P>&amount=<msat> *is* the bearer note" — and a mint that serves withdraw
anywhere but `/` cannot be addressed without the path. Scheme and query are
not included; the device adds those.

This was worth spelling out: a client passing its display-side "server"
helper here produced notes whose on-screen QR could not be claimed by
anything, and nothing caught it until one was scanned off a real panel.

```json
{"cmd":"confirm","id":"e5f6a7b8","amount_msat":21000,"host":"mint.example/w","sig":"optional hex"}
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

The gesture is a **two-second hold of button 1**, not a tap, and button 2
cancels. Worth surfacing in a client's own UI: a person who taps sees the
on-screen bar fail to fill and reasonably concludes the device is not
listening, then keeps tapping — at the one screen where that is least wanted.
The device's own card says `HOLD BTN1 2s`, but a client that puts a modal in
front of the user should say it too.

```json
{"cmd":"export_secret","id":"e5f6a7b8"}
→ {"ok":true,"k1":"<64-hex secret>"}
→ {"ok":false,"error":"user_declined"}
→ {"ok":false,"error":"timeout"}
```

### `import_secret`

Registers an externally-known secret directly as `CONFIRMED` — normally for
a note received from someone else. It also supports recovery of historical
preimage-backed mint notes, but current mint creation must use the mandatory
comment commitment described below.

```json
{"cmd":"import_secret","k1":"<64-hex secret>","host":"mint.example/w","amount_msat":21000,"label":"optional"}
→ {"ok":true,"id":"c9d0e1f2"}
```

**Idempotent on `k1`.** A note is its secret, so the vault cannot hold the
same one twice — two entries backed by one secret would report double the
value actually held, and spending either would leave the other looking
spendable after the mint has already paid it out. Importing a secret the
device already holds returns `ok` with the **existing** note's id and
creates nothing.

That makes a retry safe, which matters because this command has no
idempotency key: if the response is lost after the note was committed (a
disconnect between the write and the reply), the client cannot otherwise
tell whether it landed. Retrying returns what the first call would have.

So treat `id` as *the note for this secret*, not *the note I just created*.
Read its `state` if that matters — re-importing an already-`SPENT` secret
returns that note, still `SPENT`.

A re-import never modifies the held note. `amount_msat`, `host` and `label`
from a second import are ignored, so this cannot be used to restate what the
device shows for a note it already has.

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

### `prune_spent`

```json
{"cmd":"prune_spent"}
→ {"ok":true,"removed":25,"remaining":14}
```

Forgets every note already in `SPENT` state, in one gated action. Gated like
any other destructive command, and the card carries the **count** where a
note's amount would go — because "forget some notes" is not something anyone
can sensibly approve, and the difference between 25 and 1 is the difference
between housekeeping and a host having been busy behind your back.

It exists because `delete` takes one id and every gated command costs a
physical two-second hold, so clearing a few dozen dead notes meant a few dozen
deliberate holds. That is housekeeping nobody does, so it does not get done,
and the device fills with dead weight until `list_notes` starts refusing pages
for it.

**It takes no parameters, deliberately** — there is nothing to aim it with. It
cannot touch a `CONFIRMED` note (that is money) or a `PENDING` one (that may
yet confirm), and it leaves alone any note the index named whose blob would
not load, since those are not known-spent, they are unreadable.

With nothing to do it answers `{"ok":true,"removed":0}` and **does not
prompt**: asking somebody to approve a no-op is how people learn to approve
without reading, on a device where the next prompt hands over a bearer secret.

**What it cannot do**, and does not try to. It has no idea whether a
`CONFIRMED` note is still outstanding at the mint. A note reaches `SPENT` only
because a host told this vault so — a melt it watched settle, or a
rotate/split/merge that burned the note as an input. A note redeemed by
somebody else's wallet, or rotated away by another client, still reads
`CONFIRMED` here forever. LUD-25 is explicit that this is inherent: "A spent
note keeps its valid signature forever, and no revocation is visible offline."

Reconciling that needs the mint, and asking the mint means presenting the
bearer secret at its withdraw endpoint — which puts a live secret in an access
log for every note checked. A vault that quietly dropped notes it merely
suspected were spent would be destroying money on a guess, so this one does
not guess.

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

`unsupported` is not specific to `wipe`. Every physically-gated command —
`export_secret`, `rename`, `delete`, `discard`, `mark_spent` — answers it on a
build with no on-device confirmation wired, rather than proceeding ungated. A
gate that disappears because a dependency is absent is not a gate, and the
command that would lose the most from that is the one that discloses a
plaintext secret.

`wipe_failed` is the reply to take seriously. Erasing is not the hard part;
being able to prove it worked is. The device re-initialises and re-reads after
erasing, and reports failure rather than success if anything is still
readable -- because the owner acts on a success claim, by selling or handing on
the device. Treat `wipe_failed` as "this device still holds secrets".

Note state and secrets are gone from RAM as well as flash, and only in that
order: flash is erased and verified first, because until the verification
passes RAM holds the only intact copy.

Both halves are verified, not just the flash one. After clearing RAM the
device reads it back and checks that no byte of any note secret is still set;
if any is, it answers `wipe_failed` naming RAM as the reason rather than
`ok`. That path reboots anyway, unlike a failed flash erase where a reboot
would help nothing — a reboot is precisely what clears RAM. Treat the reply
as "this device still holds secrets until it has restarted".

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

**Minting** (preferred bound-receipt flow):
1. Require a mint payRequest with `commentAllowed >= 64`.
2. Before requesting an invoice, `new_secret()` → `id, h`. The secret is
   durably `PENDING` on the vault and never enters the browser.
3. Request `GET payCallback?amount=<gross_msat>&comment=<h>&h=<h>`. The
   `comment` is the current LUD-25 requirement; the identical `h` negotiates
   the ForgeSworn/Moneyer receipt extension.
4. Before showing or paying it, require `mintToHash:true`, `verify`, and a
   `mint` commitment whose `h` matches and whose exact net `amount` is inside
   the mint's advertised fee band (msat-exact through whole-sat-rounded).
5. Poll `verify`. On settlement, require the same invoice, `h` and `amount`,
   plus a valid LUD-25 `sig` under the pinned mint key.
6. `confirm(id, amount, host, sig)`. No `export_secret`, preimage import, or
   rotate is involved.

If the payRequest or quote does not offer that additive receipt, discard the
unpaid staged output. A compatible current-LUD-25 fallback may create a fresh
secret in the companion, request a **new** invoice with
`comment=sha256(secret)`, pay it, import that bound secret, then immediately
**rotate** it under device custody. The payment preimage remains settlement
proof and is never imported as the note. A mint without `commentAllowed >= 64`
must be refused before any invoice is paid.

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
| Hold both buttons together for ~200ms ("the chord") | **Unveil**: exports the selected note's secret and shows it as a QR code on-screen |
| Any tap while a QR is shown | Dismiss it, back to browsing |
| ~15s with no input while browsing | Back to idle |
| ~60s with nothing new on screen | The screen goes dark: blanked *and* backlit off |
| Any press while dark | Wakes it, and does nothing else — that press is spent on the light |

The screen going dark is not the device sleeping. Nothing else stops: the
transports stay up, a paired host can still drive every command, and a
confirmation arriving while the screen is dark lights it back up and puts its
card on it — a confirmation nobody can see is not one, so this can never blank
a live prompt. Going dark also ends any browse, so waking always returns to
the resting card rather than to a note selected a minute ago. A vault lives
plugged in showing the same card in the same pixels; an IPS panel treated that
way ends up wearing a permanent copy of it.

Only `CONFIRMED` notes are browsable (a `PENDING` note has no settled value
yet; `SPENT` notes have nothing left to show). The browse card shows the
selected note's amount, unit, label and id, plus its 1-based position among
`CONFIRMED` notes — so unveiling the wrong one takes a deliberate misreading
rather than a miscount. At rest, before any of that, the screen shows how many
`CONFIRMED` notes the device holds and that a tap will show them; it does not
show what they are worth.

**What the QR encodes**, and how to change it without a rebuild. Three forms,
cycled with button 1 while the code is up; button 2 dismisses as before. The
strip under the code names the one showing.

| Caption | Form | Who takes it |
|---|---|---|
| `LNURL` | bech32 `LNURL1…`, uppercase | An ordinary Lightning wallet's scanner. The default |
| `LNURLW` | `lnurlw://<host>?k1=…&amount=…` ([LUD-17](https://github.com/lnurl/luds/blob/luds/17.md)) | LNURL-native wallets that implement the scheme |
| `LINK` | `<wallet>/#/claim?u=<host>&k1=<secret>&a=<msat>` | A stock phone camera, which opens it in a browser |

LUD-25 names the first two: "prefixed with the `lnurlw://` scheme (LUD-17) or
bech32-encoded as an ordinary LNURL, `<withdraw LNURL>?k1=<P or secret>&amount=<msat>`
*is* the bearer note". The third is not in the spec — it is this project's
answer to issue #26, because a stock camera opens `https://` and does nothing
at all with `lnurlw://`, and a note nobody can accept with the phone in their
pocket is not a bearer note.

None of the three works everywhere, which is why the device cycles rather than
picks. The claim link is an `https` URL, so a wallet expecting an invoice
rejects it outright — Wallet of Satoshi reports it as "not an ln invoice",
which is the spec's own backward-compatibility promise going unmet. `lnurlw://`
support is patchy. Bech32 is the broadest, and is what the unveil screen opens
on.

Cycling deliberately does **not** extend the 60-second window: the secret
leaves the screen a fixed time after the chord that unveiled it, however many
times it is redrawn.

The bech32 form costs nothing to carry despite being the longest string —
about 177 characters against the claim link's 138 — because it is uppercase,
and the QR encoder packs uppercase bech32 in alphanumeric mode at 5.5 bits per
character instead of byte mode's 8.

The secret sits in the **fragment**, never the query, so it is not in a
request line, a referrer or a server log.

It costs one QR version over LUD-17 — about 138 characters against 113, so
version 7 rather than 6 — which still renders at two pixels per module on the
smaller of the two supported panels. `LNURLVAULT_QR_FORMAT` selects
`NOTE_URL_LUD17` instead for an LNURL-native audience, and
`LNURLVAULT_CLAIM_BASE` points the link at a different wallet.

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
