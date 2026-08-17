#!/usr/bin/env python3
"""Checks that the signing seed and the shipped public key are the same key.

    python3 tools/check_release_key.py --seed seed.bin

Nothing connected these two before. The seed lives as a CI secret and the
public key is compiled into the firmware from src/ota/release_key.c, and they
were only ever related by somebody having pasted one after generating the
other. If they drift -- a key rotated on one side, a seed regenerated, a paste
that lost a character -- every release still builds, still signs, and still
publishes. The failure appears later, on somebody's device, as an OTA that is
refused with bad_signature, by which point the release is public and the
devices are in pockets.

This turns that into a build failure at release time, which is the only moment
it can still be fixed for free. Run by .github/workflows/release.yml before it
signs anything.

Exits 0 only when the seed derives exactly the committed key.
"""

import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
RELEASE_KEY_C = REPO / "src" / "ota" / "release_key.c"

BYTE = re.compile(r"0x([0-9A-Fa-f]{2})")


def committed_pubkey(path=RELEASE_KEY_C):
    """The 32 bytes of OTA_RELEASE_PUBKEY, as hex."""
    text = path.read_text()
    start = text.find("OTA_RELEASE_PUBKEY")
    if start < 0:
        raise SystemExit(f"OTA_RELEASE_PUBKEY not found in {path}")
    body_start = text.index("{", start)
    body_end = text.index("}", body_start)
    values = BYTE.findall(text[body_start:body_end])
    if len(values) != 32:
        raise SystemExit(
            f"OTA_RELEASE_PUBKEY has {len(values)} bytes, expected 32 -- refusing to guess"
        )
    return "".join(v.lower() for v in values)


def derive_pubkey(seed_path):
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    except ImportError:
        raise SystemExit("the cryptography package is required: pip install cryptography")
    seed = pathlib.Path(seed_path).read_bytes()
    if len(seed) != 32:
        raise SystemExit(f"seed is {len(seed)} bytes, expected 32")
    return Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes_raw().hex()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seed", required=True, help="raw 32-byte ed25519 seed file")
    args = ap.parse_args()

    shipped = committed_pubkey()

    # An all-zero key is the fail-closed placeholder: the firmware accepts no
    # image at all against it. Shipping a release signed for it would produce
    # binaries nothing can ever install.
    if shipped == "00" * 32:
        print(
            "ERROR: src/ota/release_key.c still holds the all-zero placeholder key.\n"
            "       Firmware built from it refuses every OTA image. Generate a key\n"
            "       (python3 tools/ota_push.py keygen --out seed.bin), commit the\n"
            "       printed public key, and set the seed as the OTA_SIGNING_SEED\n"
            "       repository secret.",
            file=sys.stderr,
        )
        return 1

    derived = derive_pubkey(args.seed)

    if derived != shipped:
        print(
            "ERROR: the signing seed does not match the public key this firmware ships.\n"
            f"  OTA_SIGNING_SEED derives : {derived}\n"
            f"  src/ota/release_key.c has: {shipped}\n"
            "\n"
            "Every device built from this commit would refuse an image signed by that\n"
            "seed, with bad_signature. Either the secret or the committed key is stale.\n"
            "Fix whichever is wrong BEFORE releasing -- a published release signed by\n"
            "the wrong key cannot be repaired over the air, because the devices that\n"
            "would have to accept the repair are the ones rejecting it.",
            file=sys.stderr,
        )
        return 1

    print(f"signing seed matches the shipped public key: {shipped}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
