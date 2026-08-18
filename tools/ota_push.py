#!/usr/bin/env python3
"""Signs and pushes lnurl-vault OTA firmware images over WebSerial's non-
browser cousin: a plain USB-CDC connection via pyserial, using the same
newline-delimited JSON protocol test/hardware/test_serial.py talks (see
docs/PROTOCOL.md's ota_begin/ota_chunk/ota_finish).

Mirrors forgesworn/heartwood-esp32's separate `ota-sign`/`ota` host tools
(see README.md's OTA section) as one script instead of two, since signing
and pushing are both small enough here to not need separate binaries — the
security property is the same either way: `push` never needs the private
key if you hand it a pre-computed --sig (the CI/release workflow); `sign`
and `push --seed` are for a solo dev iterating locally.

Requires: pip install cryptography pyserial

Subcommands:
    keygen --out SEED_FILE
        Generates a release keypair. Writes the 32-byte seed to SEED_FILE
        (mode 0600 — treat it like an nsec) and prints the public key hex
        to paste into src/ota/release_key.c's OTA_RELEASE_PUBKEY.

    pubkey --seed SEED_FILE
        Prints the public key hex for an existing seed file.

    sign --seed SEED_FILE --image FIRMWARE.BIN [--out SIG_FILE]
        Computes sha256(image) and signs it (see ota_signing_message()
        below — must exactly match src/ota/ota_sign.c's C implementation).
        Prints the 128-hex-char signature, and writes it to SIG_FILE too
        if given.

    push --port /dev/ttyACM0 --image FIRMWARE.BIN (--seed SEED_FILE | --sig HEX_OR_FILE)
        Sends ota_begin, then the image in 1024-byte ota_chunk frames
        (matching src/proto/dispatcher.c's OTA_CHUNK_MAX_RAW), then
        ota_finish. Retries a stalled chunk a few times before giving up —
        see this device's documented serial flakiness in README.md's
        Status section; a dropped response here is expected often enough
        to be worth retrying automatically, not a bug in this script.
"""
import argparse
import base64
import hashlib
import json
import os
import sys
import time

try:
    import serial
except ImportError:
    serial = None  # only push actually needs it; keygen/pubkey/sign don't touch a port


def _ed25519_key_cls():
    """Imported lazily so the module loads without `cryptography` installed:
    `push --sig` (the release workflow's path) never signs anything, and the
    tests exercise the serial/retry logic with no key material at all. Only the
    signing subcommands actually need it."""
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    except ImportError:
        print("the cryptography package is required for this: pip install cryptography",
              file=sys.stderr)
        sys.exit(1)
    return Ed25519PrivateKey

DOMAIN = b"lnurlvault-ota-v1"
CHUNK_SIZE = 1024  # must not exceed src/proto/dispatcher.c's OTA_CHUNK_MAX_RAW
CHUNK_RETRIES = 5
CHUNK_TIMEOUT_S = 10.0  # comfortably above this device's documented ~2s+ baseline latency


def signing_message(digest: bytes) -> bytes:
    """Must byte-for-byte match src/ota/ota_sign.c's ota_signing_message()."""
    return DOMAIN + b"\x00" + digest


def load_seed(path: str) -> bytes:
    with open(path, "rb") as f:
        seed = f.read()
    if len(seed) != 32:
        raise ValueError(f"{path}: expected a 32-byte seed, got {len(seed)} bytes")
    return seed


def cmd_keygen(args):
    seed = os.urandom(32)
    fd = os.open(args.out, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(seed)
    pubkey = _ed25519_key_cls().from_private_bytes(seed).public_key()
    pubkey_hex = pubkey.public_bytes_raw().hex()
    print(f"Wrote seed to {args.out} (mode 0600) — back this up offline; there is no rotation")
    print("path that doesn't involve re-flashing every device over USB. Losing it means")
    print("devices in the field can never accept another signed OTA image.")
    print()
    print(f"Public key: {pubkey_hex}")
    print()
    print("Paste that into src/ota/release_key.c's OTA_RELEASE_PUBKEY, replacing the")
    print("all-zero placeholder, as a C uint8_t[32] initializer.")
    print()
    print(f"Seed (hex): {seed.hex()}")
    print()
    print("For the CI signing secret, use the HEX form above, not the raw seed file —")
    print("GitHub Actions secrets are handled as text/env vars, and a raw 32-byte value")
    print("can contain bytes (NUL, newlines) that don't survive that round-trip intact.")
    print("release.yml decodes it back to bytes before signing. Set it with:")
    print()
    print(f"  gh secret set OTA_SIGNING_SEED --body {seed.hex()}")


def cmd_pubkey(args):
    seed = load_seed(args.seed)
    pubkey = _ed25519_key_cls().from_private_bytes(seed).public_key()
    print(pubkey.public_bytes_raw().hex())


def sign_image(seed: bytes, image: bytes) -> str:
    digest = hashlib.sha256(image).digest()
    sk = _ed25519_key_cls().from_private_bytes(seed)
    signature = sk.sign(signing_message(digest))
    return signature.hex()


def cmd_sign(args):
    with open(args.image, "rb") as f:
        image = f.read()
    seed = load_seed(args.seed)
    sig_hex = sign_image(seed, image)
    print(sig_hex)
    if args.out:
        with open(args.out, "w") as f:
            f.write(sig_hex + "\n")


class Device:
    """Deliberately minimal — mirrors test/hardware/test_serial.py's own
    Device class (same reasoning: this board has a real, documented
    ~2s+ baseline response latency, so reads keep going past a short wait
    instead of giving up early)."""

    def __init__(self, port):
        self.ser = serial.Serial(port, baudrate=115200, timeout=0.05)
        self.buf = b""
        time.sleep(1.5)
        self.ser.reset_input_buffer()

    def send(self, obj, wait=CHUNK_TIMEOUT_S):
        self.ser.write((json.dumps(obj) + "\n").encode())
        self.ser.flush()
        deadline = time.time() + wait
        while time.time() < deadline:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                line = line.strip()
                # Boot chatter and any enabled diagnostics share this UART with
                # the protocol on the classic board, so the reply is the line
                # that starts with '{' -- not simply the next line. bench.py and
                # test_serial.py match the same way.
                if not line.startswith(b"{"):
                    continue
                try:
                    return json.loads(line.decode())
                except ValueError:
                    continue
            chunk = self.ser.read(4096)
            if chunk:
                self.buf += chunk
        return None

    def close(self):
        self.ser.close()


def _chunk_already_applied(resp):
    """A resent chunk whose previous ACK was lost: the device applied it, so
    `bytes_received` advanced and it now expects the *next* offset -- it rejects
    this one with a bad_request naming the offset (see dispatcher.c's
    handle_ota_chunk, which does NOT abort the session for this). That is
    success, not failure: the bytes are already on flash. This is the likeliest
    real-world OTA failure mode given this device's documented serial flakiness,
    and blindly resending the same offset would otherwise loop until it gave up."""
    return (
        isinstance(resp, dict)
        and resp.get("ok") is False
        and resp.get("error") == "bad_request"
        and "offset" in (resp.get("message") or "")
    )


def send_chunk(dev, offset, data_b64):
    for attempt in range(1, CHUNK_RETRIES + 1):
        resp = dev.send({"cmd": "ota_chunk", "offset": offset, "data": data_b64})
        if isinstance(resp, dict) and resp.get("ok") is True:
            return
        if _chunk_already_applied(resp):
            return  # a lost ACK, not a rejection -- the device already has it
        print(f"  chunk at offset {offset}: attempt {attempt}/{CHUNK_RETRIES} got {resp!r}, retrying...",
              file=sys.stderr)
    raise RuntimeError(f"chunk at offset {offset} failed after {CHUNK_RETRIES} attempts")


def push_image(dev, image, sig_hex, digest):
    """Runs ota_begin -> ota_chunk* -> ota_finish over `dev` (anything with a
    .send(obj, wait=...) method). Extracted from cmd_push so the transfer and
    its retry behaviour can be tested against a fake device with no serial port."""
    resp = dev.send(
        {"cmd": "ota_begin", "size": len(image), "sha256": digest.hex(), "signature": sig_hex},
        wait=35.0,  # >30s device-side approval window
    )
    if not (isinstance(resp, dict) and resp.get("ok")):
        raise RuntimeError(f"ota_begin failed: {resp!r}")

    offset = 0
    while offset < len(image):
        chunk = image[offset : offset + CHUNK_SIZE]
        send_chunk(dev, offset, base64.b64encode(chunk).decode())
        offset += len(chunk)
        print(f"  {offset}/{len(image)} bytes ({100 * offset // len(image)}%)", end="\r", file=sys.stderr)
    print(file=sys.stderr)

    resp = dev.send({"cmd": "ota_finish"}, wait=10.0)
    if not (isinstance(resp, dict) and resp.get("ok")):
        raise RuntimeError(f"ota_finish failed: {resp!r}")


def cmd_push(args):
    if serial is None:
        print("pyserial is required for push: pip install pyserial", file=sys.stderr)
        sys.exit(1)
    with open(args.image, "rb") as f:
        image = f.read()
    digest = hashlib.sha256(image).digest()

    if args.seed:
        sig_hex = sign_image(load_seed(args.seed), image)
    elif os.path.isfile(args.sig):
        sig_hex = open(args.sig).read().strip()
    else:
        sig_hex = args.sig.strip()

    print(f"Image: {args.image} ({len(image)} bytes)")
    print(f"sha256: {digest.hex()}")
    print(f"signature: {sig_hex}")

    dev = Device(args.port)
    try:
        print("Sending ota_begin (approve on the device within 30s)...")
        push_image(dev, image, sig_hex, digest)
        print("Done — device is rebooting into the new image.")
    finally:
        dev.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("keygen")
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_keygen)

    p = sub.add_parser("pubkey")
    p.add_argument("--seed", required=True)
    p.set_defaults(func=cmd_pubkey)

    p = sub.add_parser("sign")
    p.add_argument("--seed", required=True)
    p.add_argument("--image", required=True)
    p.add_argument("--out")
    p.set_defaults(func=cmd_sign)

    p = sub.add_parser("push")
    p.add_argument("--port", required=True)
    p.add_argument("--image", required=True)
    p.add_argument("--seed", help="sign locally with this seed")
    p.add_argument("--sig", help="a pre-computed 128-hex-char signature, or a file containing one")
    p.set_defaults(func=cmd_push)

    args = parser.parse_args()
    if args.command == "push" and not args.seed and not args.sig:
        parser.error("push requires --seed or --sig")
    args.func(args)


if __name__ == "__main__":
    main()
