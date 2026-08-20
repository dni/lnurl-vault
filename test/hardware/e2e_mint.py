#!/usr/bin/env python3
"""Section 17: a real mint, a real board, and the two-phase commit between.

Everything else in this directory tests the device against itself. This
walks the whole LUD-25 chain with a live mint on one side and a flashed
vault on the other, doing what lnurl-wallet's deviceOrchestration.ts does --
which until now had only ever run against mocks.

    python3 test/hardware/e2e_mint.py --port /dev/ttyUSB0 --mint http://127.0.0.1:8111
    python3 test/hardware/e2e_mint.py --port /dev/ttyUSB0 --mint http://127.0.0.1:3737 \
        --seed-note <k1>

The chain is deliberately one continuous lineage from a single starting
note, because that is the only way each step's output is proven spendable:
every operation consumes what the previous one produced.

    mint (or seed) -> rotate -> split -> merge -> rotate -> melt

**This needs a finger on the board.** Every `export_secret` is gated by a
physical confirm, by design -- it is the only command that ever discloses a
plaintext secret. Six presses for the full chain. The script says when.

Two ways in, because not every mint can settle its own invoices:

  --seed-note K1   start from a note you already hold. moneyer --dev prints
                   one at startup; its fake funding source mints unpayable
                   invoices on purpose, so nothing can settle a fresh one.
  (default)        mint a new note over LUD-21 verify, which needs a mint
                   whose funding source actually settles -- lnurl-mint in
                   front of test/hardware/fake_cln.py does.

--fake-cln points at that fake node's control API, so a melt has an invoice
it can genuinely pay. Without it the melt row is skipped rather than
reported as passing: a mint that cannot pay is not evidence of anything.
"""

import argparse
import json
import sys
import time
from hashlib import sha256
from os import urandom
from pathlib import Path
from urllib.parse import urlencode

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    import httpx
except ImportError:
    sys.exit("needs httpx: pip install httpx")

from bench import Bench, Device  # noqa: E402

# The chain, in the only order that proves anything: each step spends what
# the one before it produced.
STEPS_DEFAULT = "rotate1,split,merge,rotate2,melt"


def new_secret_pair():
    """A (k1, h) the WALLET generated -- what the vault does on-device, done
    here only for the melt invoice, never for a note the vault holds."""
    k1 = urandom(32)
    return k1.hex(), sha256(k1).hexdigest()


class Mint:
    """The mint HTTP a browser wallet would be doing."""

    def __init__(self, base, fake_cln=None):
        self.base = base.rstrip("/")
        self.fake_cln = fake_cln.rstrip("/") if fake_cln else None
        self.http = httpx.Client(timeout=30.0)
        self.host = self.base.split("//", 1)[-1]

    def note(self, k1):
        """LUD-03 withdrawRequest for a note -- its value and its callback.

        A spent or unknown note comes back as a 200 carrying
        `{"status":"ERROR"}`, not an HTTP error, so callers check the body.
        """
        r = self.http.get(f"{self.base}/w", params={"k1": k1})
        try:
            return r.json()
        except ValueError:
            return {"status": "ERROR", "reason": f"HTTP {r.status_code}"}

    @staticmethod
    def spendable(info):
        return info.get("status") != "ERROR" and info.get("maxWithdrawable", 0) > 0

    def callback(self, params):
        """The one mutating call. A list of (key, value) pairs, not a dict:
        merge repeats `k1`, which a dict cannot express."""
        r = self.http.get(f"{self.base}/w/cb?{urlencode(params)}")
        try:
            return r.json()
        except ValueError:
            return {"status": "ERROR", "reason": f"HTTP {r.status_code}: {r.text[:200]}"}

    def mint_note(self, amount_msat, timeout=20):
        """Pay a payRequest and come back with the preimage, which for
        LUD-25 IS the note's spend secret. Returns None if this mint's
        funding source never settles (moneyer --dev, deliberately)."""
        r = self.http.get(f"{self.base}/p/cb", params={"amount": amount_msat})
        r.raise_for_status()
        body = r.json()
        verify = body.get("verify")
        if not verify:
            return None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            v = self.http.get(verify).json()
            if v.get("settled") and v.get("preimage"):
                return v["preimage"]
            time.sleep(0.5)
        return None

    def payable_invoice(self, amount_msat):
        """A BOLT-11 the mint's funding source can actually settle, for the
        melt row. Only possible against the fake node, which is told the
        preimage up front -- lnurl-mint verifies what comes back against the
        invoice's own payment hash, so an invented preimage fails there."""
        if not self.fake_cln:
            return None
        preimage = urandom(32).hex()
        inv = httpx.post(
            f"{self.fake_cln}/v1/invoice",
            headers={"Rune": "harness"},
            json={
                "amount_msat": amount_msat,
                "label": urandom(8).hex(),
                "description": "e2e melt target",
                "preimage": preimage,
            },
            timeout=15.0,
        )
        inv.raise_for_status()
        pr = inv.json()["bolt11"]
        # Teach the node the preimage, so its xpay can settle rather than
        # reporting no route.
        httpx.post(
            f"{self.fake_cln}/control/payable",
            json={"bolt11": pr, "preimage": preimage},
            timeout=15.0,
        ).raise_for_status()
        return pr


class Vault:
    """The vault side of deviceOrchestration.ts, over a held-open port."""

    def __init__(self, dev):
        self.dev = dev

    def notes(self):
        """Every note, following the paging -- a device that has been
        benched before will have more than one page of them."""
        out, offset = [], 0
        while True:
            r = self.dev.cmd({"cmd": "list_notes", "offset": offset, "limit": 20})
            if not r or not r.get("ok"):
                return out
            page = r.get("notes") or []
            out.extend(page)
            offset += len(page)
            if not page or offset >= r.get("total", 0):
                return out

    def note(self, note_id):
        for n in self.notes():
            if n["id"] == note_id:
                return n
        return None

    def export_secret(self, note_id, why, attempts=3):
        """The gated one. Prompt first, then send: the device only puts its
        confirm screen up once the command has arrived.

        A timeout is re-asked rather than failed. Nobody driving this can see
        the script's own output and the board at the same time, so a missed
        30s window is a missed glance, not a defect -- and failing the whole
        chain on one costs every press already given. A `user_declined` is
        NOT retried: that was a deliberate answer, and asking again until
        someone gives in is how a confirmation gate becomes a formality.
        """
        for attempt in range(1, attempts + 1):
            suffix = "" if attempt == 1 else f"  [asking again, {attempt}/{attempts}]"
            print(f"\n    >>> APPROVE ON THE BOARD: {why}{suffix}", flush=True)
            r = self.dev.cmd({"cmd": "export_secret", "id": note_id}, wait=45)
            if r and r.get("ok"):
                print("    <<< approved")
                return r["k1"]
            error = (r or {}).get("error", "no response")
            print(f"    <<< {error}")
            if error != "timeout":
                return None
        return None


def assert_states(b, vault, expected, label):
    """expected: {note_id: state}. One row, so a wrong state anywhere in the
    lineage fails loudly rather than being noticed three steps later."""
    notes = {n["id"]: n for n in vault.notes()}
    wrong = []
    for note_id, state in expected.items():
        actual = notes.get(note_id, {}).get("state")
        if actual != state:
            wrong.append(f"{note_id} is {actual}, expected {state}")
    return b.check(label, not wrong, "; ".join(wrong))


def rotate(b, vault, mint, note_id, label):
    """Burn one note, mint a fresh one of the same value.

    The order is the whole point: the device stages a secret and discloses
    only its hash, the mint burns the input against that hash, and only a
    confirmed OK commits the output. An error discards it, so a refused
    rotate cannot leave a note the mint never issued looking spendable.
    """
    held = vault.note(note_id)
    if not held:
        return b.check(label, False, f"{note_id} not on device") and None

    k1 = vault.export_secret(note_id, f"rotate {note_id}")
    if not k1:
        b.check(label, False, "export declined or timed out")
        return None

    r = vault.dev.cmd({"cmd": "new_secret", "parent_ids": [note_id], "label": "rotated"})
    if not r or not r.get("ok"):
        b.check(label, False, f"new_secret failed: {r}")
        return None
    new_id, h = r["id"], r["h"]

    resp = mint.callback([("k1", k1), ("h", h)])
    if resp.get("status") != "OK":
        vault.dev.cmd({"cmd": "discard", "id": new_id})
        b.check(label, False, f"mint refused: {resp.get('reason')}")
        return None

    confirm = {
        "cmd": "confirm",
        "id": new_id,
        "amount_msat": held["amount_msat"],
        "host": mint.host,
    }
    if resp.get("sig"):
        confirm["sig"] = resp["sig"]
    vault.dev.cmd(confirm)
    vault.dev.cmd({"cmd": "mark_spent", "id": note_id})

    ok = assert_states(b, vault, {note_id: "spent", new_id: "confirmed"}, label)
    return new_id if ok else None


def split(b, vault, mint, note_id, amount_msat, label):
    """Burn one note, mint two: `amount` and the remainder."""
    held = vault.note(note_id)
    if not held:
        b.check(label, False, f"{note_id} not on device")
        return None

    k1 = vault.export_secret(note_id, f"split {note_id}")
    if not k1:
        b.check(label, False, "export declined or timed out")
        return None

    r = vault.dev.cmd({"cmd": "new_secret_pair", "parent_ids": [note_id]})
    if not r or not r.get("ok"):
        b.check(label, False, f"new_secret_pair failed: {r}")
        return None
    id_a, h_a, id_b, h_b = r["id"], r["h"], r["id2"], r["h2"]

    resp = mint.callback([("k1", k1), ("amount", amount_msat), ("h", h_a), ("h2", h_b)])
    if resp.get("status") != "OK":
        vault.dev.cmd({"cmd": "discard", "id": id_a})
        vault.dev.cmd({"cmd": "discard", "id": id_b})
        b.check(label, False, f"mint refused: {resp.get('reason')}")
        return None

    remainder = held["amount_msat"] - amount_msat
    for note, value in ((id_a, amount_msat), (id_b, remainder)):
        vault.dev.cmd(
            {"cmd": "confirm", "id": note, "amount_msat": value, "host": mint.host}
        )
    vault.dev.cmd({"cmd": "mark_spent", "id": note_id})

    ok = assert_states(
        b, vault, {note_id: "spent", id_a: "confirmed", id_b: "confirmed"}, label
    )
    return (id_a, id_b) if ok else None


def merge(b, vault, mint, note_ids, label):
    """Burn many notes, mint one worth their sum."""
    held = [vault.note(n) for n in note_ids]
    if any(h is None for h in held):
        b.check(label, False, "an input is not on device")
        return None
    total = sum(h["amount_msat"] for h in held)

    secrets = []
    for note_id in note_ids:
        k1 = vault.export_secret(note_id, f"merge input {note_id}")
        if not k1:
            b.check(label, False, "export declined or timed out")
            return None
        secrets.append(k1)

    r = vault.dev.cmd({"cmd": "new_secret", "parent_ids": list(note_ids), "label": "merged"})
    if not r or not r.get("ok"):
        b.check(label, False, f"new_secret failed: {r}")
        return None
    new_id, h = r["id"], r["h"]

    params = [("k1", k) for k in secrets] + [("h", h)]
    resp = mint.callback(params)
    if resp.get("status") != "OK":
        vault.dev.cmd({"cmd": "discard", "id": new_id})
        b.check(label, False, f"mint refused: {resp.get('reason')}")
        return None

    confirm = {"cmd": "confirm", "id": new_id, "amount_msat": total, "host": mint.host}
    if resp.get("sig"):
        confirm["sig"] = resp["sig"]
    vault.dev.cmd(confirm)
    for note_id in note_ids:
        vault.dev.cmd({"cmd": "mark_spent", "id": note_id})

    expected = {n: "spent" for n in note_ids}
    expected[new_id] = "confirmed"
    ok = assert_states(b, vault, expected, label)
    return new_id if ok else None


def melt(b, vault, mint, note_id, label):
    """Redeem to a BOLT-11. No new secret: nothing is minted, so there is
    nothing to stage -- but the note is only marked spent once the mint
    reports the payment settled."""
    held = vault.note(note_id)
    if not held:
        b.check(label, False, f"{note_id} not on device")
        return False

    # The invoice must be for the note's exact value: lnurl-mint rejects
    # anything else outright ("Invoice must be for exactly N msat"), and
    # takes the routing fee from its own side rather than from the note.
    invoice = mint.payable_invoice(held["amount_msat"])
    if not invoice:
        b.skip(label, "no payable invoice (needs --fake-cln)")
        return None

    k1 = vault.export_secret(note_id, f"melt {note_id}")
    if not k1:
        b.check(label, False, "export declined or timed out")
        return False

    resp = mint.callback([("k1", k1), ("pr", invoice)])
    if resp.get("status") != "OK":
        b.check(label, False, f"mint refused: {resp.get('reason')}")
        return False

    # OK means in-flight, not settled -- the note stays CONFIRMED on the
    # device until the mint says the payment landed.
    settled = False
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        # Burned notes drop out of the mint's withdraw view entirely.
        if not Mint.spendable(mint.note(k1)):
            settled = True
            break
        time.sleep(1.0)

    if not settled:
        b.check(label, False, "mint never reported the melt settled")
        return False

    vault.dev.cmd({"cmd": "mark_spent", "id": note_id})
    return assert_states(b, vault, {note_id: "spent"}, label)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="serial port the vault is on")
    ap.add_argument("--mint", required=True, help="base URL of the mint")
    ap.add_argument("--seed-note", help="k1 of a note you already hold, instead of minting")
    ap.add_argument("--fake-cln", help="control API of test/hardware/fake_cln.py, for the melt row")
    ap.add_argument("--amount-msat", type=int, default=21000, help="value to mint, if minting")
    ap.add_argument(
        "--resume-note-id",
        help="carry on from a confirmed note already on the device, instead of minting "
        "or importing -- so a run interrupted by a missed press does not cost the "
        "presses it already had",
    )
    ap.add_argument(
        "--steps",
        default=STEPS_DEFAULT,
        help=f"comma-separated subset of: {STEPS_DEFAULT}",
    )
    args = ap.parse_args()

    steps = [s.strip() for s in args.steps.split(",") if s.strip()]
    unknown = [s for s in steps if s not in STEPS_DEFAULT.split(",")]
    if unknown:
        sys.exit(f"unknown step(s): {', '.join(unknown)}")

    mint = Mint(args.mint, args.fake_cln)
    b = Bench()

    print(f"\nmint: {args.mint}")
    dev = Device(args.port)
    info = dev.cmd({"cmd": "get_info"})
    if not info or not info.get("ok"):
        sys.exit("the vault did not answer get_info")
    print(f"vault: fw {info['fw_version']} on {info['board']}, {info['note_count']} notes held")
    vault = Vault(dev)

    press_count = sum(2 if s == "merge" else 1 for s in steps)
    print(f"steps: {', '.join(steps)}  ({press_count} approvals expected)")

    try:
        if args.resume_note_id:
            note = vault.note(args.resume_note_id)
            if not note or note["state"] != "confirmed":
                sys.exit(f"{args.resume_note_id} is not a confirmed note on this device")
            current, value = note["id"], note["amount_msat"]
            b.skip("mint", "resumed from a note already held")
            print(f"\nresuming from {current}, worth {value} msat")
        else:
            print("\n-- mint --")
            if args.seed_note:
                k1 = args.seed_note
                b.skip("mint", "started from --seed-note")
            else:
                k1 = mint.mint_note(args.amount_msat)
                b.check(
                    "mint",
                    bool(k1),
                    "paid a payRequest, took the preimage" if k1 else "never settled",
                )
                if not k1:
                    sys.exit(b.report())

            note_info = mint.note(k1)
            if not Mint.spendable(note_info):
                sys.exit(f"the starting note is not spendable at this mint: {note_info.get('reason')}")
            value = note_info["maxWithdrawable"]
            print(f"    note worth {value} msat")

            r = dev.cmd(
                {
                    "cmd": "import_secret",
                    "k1": k1,
                    "host": mint.host,
                    "amount_msat": value,
                    "label": "e2e seed",
                }
            )
            current = r.get("id") if r and r.get("ok") else None
            b.check("import_secret", bool(current), f"id {current}")
            if not current:
                sys.exit(b.report())

        for step in steps:
            if step == "rotate1":
                # LUD-25's security consideration: whoever handed this secret
                # over -- the mint's own node, on a fresh mint -- is a prior
                # holder of it, so it is not trusted until it has been rotated
                # onto a secret only this device has ever seen.
                print("\n-- rotate (the untrusted import) --")
                current = rotate(b, vault, mint, current, "rotate after import")
            elif step == "split":
                print("\n-- split --")
                current = split(b, vault, mint, current, value // 3, "split")
            elif step == "merge":
                print("\n-- merge --")
                current = merge(b, vault, mint, current, "merge")
                if current:
                    merged = vault.note(current)
                    b.check(
                        "merge conserves value",
                        merged["amount_msat"] == value,
                        f"{merged['amount_msat']} msat vs {value} in",
                    )
            elif step == "rotate2":
                print("\n-- rotate --")
                current = rotate(b, vault, mint, current, "rotate")
            elif step == "melt":
                print("\n-- melt --")
                melt(b, vault, mint, current, "melt")
                current = None
            if current is None and step != "melt":
                print(f"\nstopped after {step}; nothing to carry forward")
                break

        print("\n-- final state --")
        notes = vault.notes()
        pending = [n["id"] for n in notes if n["state"] == "pending"]
        b.check("nothing left pending", not pending, ", ".join(pending))

    finally:
        dev.close()

    sys.exit(b.report())


if __name__ == "__main__":
    main()
