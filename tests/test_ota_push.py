"""Tests for tools/ota_push.py -- the host tool that drives a real OTA.

Two things it must get right that had no coverage:

  1. `signing_message()` must byte-for-byte match src/ota/ota_sign.c's
     `ota_signing_message()`. A drift there produces releases every device
     refuses, and the same "verify the message, not the bare digest" mistake
     is already recorded in docs/HARDWARE-TEST-CHECKLIST.md.
  2. The chunk transfer must survive a lost ACK. Given this device's
     documented serial flakiness, a chunk applied whose response never arrives
     is the likeliest real-world failure: the retry resends the same offset,
     which the device now rejects as out-of-order. That must be recognised as
     "already applied, advance", not looped until it gives up.

Run: pytest -q tests/
"""
import hashlib
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import ota_push  # noqa: E402  (path set up above; cryptography is imported lazily)


def test_signing_message_matches_the_c_wire_format():
    digest = bytes(range(32))
    msg = ota_push.signing_message(digest)
    # "lnurlvault-ota-v1" || 0x00 || sha256(image) -- exactly ota_sign.c.
    assert msg == b"lnurlvault-ota-v1" + b"\x00" + digest
    assert len(msg) == 17 + 1 + 32


def test_sign_image_produces_a_signature_over_that_message():
    ed = pytest.importorskip("cryptography.hazmat.primitives.asymmetric.ed25519")
    seed = bytes([0x42]) * 32
    image = b"firmware-bytes" * 100
    sig_hex = ota_push.sign_image(seed, image)
    pub = ed.Ed25519PrivateKey.from_private_bytes(seed).public_key()
    digest = hashlib.sha256(image).digest()
    # Raises InvalidSignature if the signed message is not exactly this.
    pub.verify(bytes.fromhex(sig_hex), ota_push.signing_message(digest))


class FakeDevice:
    """Stands in for the serial Device: records what was sent and replays a
    scripted list of responses (a dict, or None for a lost/timed-out reply)."""

    def __init__(self, responses):
        self.responses = list(responses)
        self.sent = []

    def send(self, obj, wait=None):
        self.sent.append(obj)
        return self.responses.pop(0)


OK = {"ok": True}
OFFSET_ERR = {"ok": False, "error": "bad_request",
              "message": "offset does not match the next expected byte"}


def test_push_completes_when_a_chunk_ack_is_lost():
    image = b"x" * (ota_push.CHUNK_SIZE + 100)  # exactly two chunks
    digest = hashlib.sha256(image).digest()
    dev = FakeDevice([
        OK,          # ota_begin
        None,        # chunk 0, attempt 1: applied on-device but ACK lost
        OFFSET_ERR,  # chunk 0, attempt 2: device already advanced past it
        OK,          # chunk 1
        OK,          # ota_finish
    ])
    ota_push.push_image(dev, image, "ab" * 64, digest)  # must not raise

    chunk_offsets = [m["offset"] for m in dev.sent if m.get("cmd") == "ota_chunk"]
    assert chunk_offsets == [0, 0, ota_push.CHUNK_SIZE], "offset 0 retried once, then advances"


def test_push_raises_when_a_chunk_fails_for_real():
    image = b"x" * 10
    digest = hashlib.sha256(image).digest()
    real_err = {"ok": False, "error": "ota_failed", "message": "flash write failed"}
    dev = FakeDevice([OK] + [real_err] * ota_push.CHUNK_RETRIES)
    with pytest.raises(RuntimeError):
        ota_push.push_image(dev, image, "ab" * 64, digest)


def test_a_genuine_offset_error_is_not_mistaken_for_a_real_failure():
    assert ota_push._chunk_already_applied(OFFSET_ERR) is True
    assert ota_push._chunk_already_applied({"ok": False, "error": "ota_failed"}) is False
    assert ota_push._chunk_already_applied(None) is False
    assert ota_push._chunk_already_applied(OK) is False
