# lnurl-vault wire protocol

The device is a command server; a paired browser (or any other host) is the
client. Every command gets exactly one response. The protocol is identical
over both transports — only the framing around a JSON message differs.

This protocol is deliberately narrow: the device does no networking and
knows nothing about LNURL/mint HTTP calls. It only ever generates secrets,
discloses their SHA-256 hash, and tracks note state. See "Orchestration"
below for how a browser client (e.g. a future `lnurl-wallet` integration)
composes these commands with calls to a mint to implement rotate/split/
merge/melt, per [LUD-25](../../luds/25.md).

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
`invalid_state`, `user_declined`, `timeout`, `storage_full`, `bad_request`.

### `get_info`

```json
{"cmd":"get_info"}
→ {"ok":true,"fw_version":"0.1.0","note_count":3,"pending_count":1}
```

### `list_notes`

Metadata only — a note's secret is never included in any command's response
except `export_secret`.

```json
{"cmd":"list_notes"}
→ {"ok":true,"notes":[
    {"id":"a1b2c3d4","state":"confirmed","amount_msat":21000,"label":"",
     "host":"mint.example","parent_ids":[],"created_at":1234,"updated_at":1234}
  ]}
```

`state` is one of `pending`, `confirmed`, `spent`. `sig` is present only if
the note carries an offline-verification signature ([LUD-25](../../luds/25.md)).

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

## Orchestration

How a browser client composes these commands with mint HTTP calls to
implement each [LUD-25](../../luds/25.md) operation. The device never talks
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
...)`, then immediately **rotate** it (per [LUD-25](../../luds/25.md)'s
security considerations — a payment preimage was also seen by the mint
itself as a prior holder).

**Receiving a note from someone else** (offline handoff): `import_secret`
with the received `k1`, then immediately **rotate** it, closing the window
in which the previous holder could also redeem it.
