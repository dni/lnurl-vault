# Security

This device holds bearer secrets. A note's `k1` **is** the money: whoever has
it can spend it, once, and there is no account, no password and no way to
reverse a spend. That shapes everything below.

## Reporting a vulnerability

Please report privately, not as a public issue, using GitHub's private
vulnerability reporting on this repository (**Security → Report a
vulnerability**). If that is not enabled, open an issue saying only that you
have a security report and asking for a private channel — no details.

There is no funded security team and no response-time commitment here; this is
an open-source project. What can be promised is that a report will be read and
answered rather than ignored.

## What this device is trying to protect against

**Someone with your paired browser session, or with a radio in range, taking
your money without you knowing.**

The control is physical: a plaintext secret is disclosed only after a hold on
the device's own button, which nothing over the wire can perform. A host that
is fully compromised can ask, repeatedly, and be refused every time as long as
nobody is holding the button.

## What it explicitly does NOT protect against

These are design decisions, written down so nobody discovers them the hard
way. None of them is a vulnerability report.

**Physical possession.** Note storage is **not encrypted at rest**. A flash
dump recovers every secret the device holds. NVS encryption is deliberately
off: on ESP32-S3 the HMAC key-protection scheme burns an eFuse on first boot,
irreversibly, as a silent side effect of powering the device on — see
`sdkconfig.defaults`, which sets out the whole call. Physical possession is the
protection model, exactly as it is for the on-device browse-and-unveil gesture.
Treat the device as you would treat cash in a wallet.

**A stolen device that is unlocked.** There is no PIN and no lockout. Anyone
holding the device can browse notes and unveil them on screen.

**BLE proximity.** There is no bonding and no passkey, so any central in radio
range can connect and issue commands. It cannot extract a secret without the
physical gesture -- but see "Known gaps" for what it can do without one.

**A malicious mint.** The device discloses `sha256(k1)` at mint time and only
reveals `k1` later, which is what stops a mint from spending your note behind
your back. It does not protect against a mint that simply refuses to honour a
withdrawal.

**Supply chain of the device itself.** A board flashed with modified firmware
before it reached you can do anything. Secure Boot and Flash Encryption are not
enabled; enabling them is a controlled production step with a written eFuse
map, not a default build.

## What is verified, and how

Claims about hardware behaviour live in
[`docs/HARDWARE-TEST-CHECKLIST.md`](docs/HARDWARE-TEST-CHECKLIST.md), which
records the date, board and firmware version for each area and marks anything
unrun as `NOT YET BENCH-RUN` in those words. Prefer it over prose elsewhere;
prose goes stale silently and that file is written not to.

The portable core — note state machine, protocol dispatch, framing, the
approval gesture, signature verification — is covered by `test/native/`, which
runs on every push and needs no hardware.

## Installing over an existing device destroys its notes

The web installer writes a single merged image from offset 0, and the padding
between its parts covers the storage region. A re-install is therefore not an
update in place: **every note on the device is erased**, whatever the erase
prompt appears to offer. Measured, not inferred — a board holding a full vault
came back with none.

Updating a device that already holds notes means a signed OTA image, which
only ever replaces the app partition and never touches storage.

That is not a vulnerability, but it is the kind of thing people discover the
expensive way, so it is written down here as well as on the installer page.

## Firmware updates

OTA images are ed25519-signed and verified **twice**: at `ota_begin` against
the claimed digest, before the owner is asked anything, and again at
`ota_finish` against the digest of the bytes actually written to flash. The
release public key is compiled in; an image signed by anything else is refused.
The bootloader is never touched, so a failed update leaves the running firmware
intact.

The signing seed is held as a CI secret. Losing it means devices in the field
can never accept another signed image — recovery is a cable and a person, per
device. There is no key-rotation path that avoids that.

## Known gaps

Tracked as issues rather than hidden here. At the time of writing:

- **BLE is unauthenticated** — no bonding, no passkey ([#16](../../issues/16)).
  Any central in radio range can connect and issue commands. The destructive
  commands (`mark_spent`, `delete`, `discard`, `rename`) are now gated behind
  the same physical confirmation as `export_secret`; bonding is not addressed.
- **No PIN or lockout** on the device itself.
- **No Secure Boot or Flash Encryption**, per the eFuse reasoning above.
- **OTA has never been run end to end on hardware** — the signature and
  sequencing logic is unit-tested against a fake flash, but no real transfer,
  partition switch or rollback has been exercised.

If you find something not on this list, please report it.
