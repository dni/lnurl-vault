#!/usr/bin/env python3
"""A fake clnrest node, just enough of it to run lnurl-mint locally.

lnurl-mint needs a funding source before it will mint or melt anything, and
the only two it supports are real cln and real lnd. That makes the one thing
section 17 of the hardware checklist asks for -- a real mint, a real board,
and the two-phase commit running between them -- awkward to stage on a
laptop. This serves the seven RPCs lnurl-mint actually calls (see its
node.py: invoice, xpay, signmessage, listinvoices, listpays, getinfo,
listchannels) and nothing else.

It is a test double, not a node. No channels, no gossip, no money. Invoices
it issues are structurally valid BOLT-11 and signed, but nothing on the
network can pay them; they settle because this process says so.

    python3 test/hardware/fake_cln.py --port 9737

Two control endpoints exist for the harness driving it, both outside cln's
own API:

    POST /control/payable  {"bolt11": ..., "preimage": ...}
        Teaches it the preimage for an invoice it did not issue, so a melt
        can succeed. Without this, xpay reports "no route" -- lnurl-mint
        verifies the returned preimage against the invoice's payment hash
        (node._verify_preimage), so a melt can only be faked for an invoice
        whose preimage the fake already knows.

    POST /control/settle  {"payment_hash": ...}
        Marks a mint invoice paid, when --no-autosettle is in play.

By default an invoice this node issues is paid the moment it is issued,
which is what makes the mint flow deterministic to drive.
"""

import argparse
import json
import time
from hashlib import sha256
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from os import urandom

import bolt11
from bolt11.models.tags import TagChar, Tags
from bolt11.types import Bolt11
from coincurve import PrivateKey

# Fixed, so the mint's advertised mintPubkey does not change every time this
# is restarted -- a wallet that pinned it would otherwise see a new mint
# identity on each run, and offline verification of a note minted before the
# restart would fail against the newly advertised key.
NODE_KEY_HEX = "b0f6d3e8a1c25947e3d1b8046f2a9c5e7813d40a6b2e95c1f7a3084d2c6b5e91"

_LIGHTNING_SIGNED_MESSAGE_PREFIX = b"Lightning Signed Message:"


class FakeNode:
    def __init__(self, autosettle=True):
        self.key = PrivateKey(bytes.fromhex(NODE_KEY_HEX))
        self.pubkey = self.key.public_key.format(compressed=True).hex()
        self.autosettle = autosettle
        # payment_hash -> {preimage, amount_msat, status, bolt11}
        self.invoices = {}
        # payment_hash -> {status, preimage, amount_msat, amount_sent_msat}
        self.pays = {}
        # payment_hash -> preimage, for invoices this node did not issue
        self.payable = {}

    def invoice(self, amount_msat, label, description, preimage_hex):
        preimage = bytes.fromhex(preimage_hex) if preimage_hex else urandom(32)
        payment_hash = sha256(preimage).hexdigest()
        tags = Tags()
        tags.add(TagChar.payment_hash, payment_hash)
        tags.add(TagChar.payment_secret, urandom(32).hex())
        tags.add(TagChar.description, description or "")
        pr = bolt11.encode(
            Bolt11(
                currency="bc",
                amount_msat=amount_msat,
                date=int(time.time()),
                tags=tags,
            ),
            private_key=NODE_KEY_HEX,
        )
        self.invoices[payment_hash] = {
            "preimage": preimage.hex(),
            "amount_msat": amount_msat,
            "label": label,
            # An invoice nobody can pay would leave the mint polling forever,
            # so unless asked otherwise it is paid on arrival.
            "status": "paid" if self.autosettle else "unpaid",
            "bolt11": pr,
        }
        return pr, payment_hash

    def xpay(self, invstring, maxfee):
        """Pays only what this node has been taught a preimage for.

        lnurl-mint checks the returned preimage against the invoice's own
        payment hash, so inventing one is not an option -- an unknown
        invoice gets cln's "no route" (205) instead, which is also the
        honest answer for a node with no channels.
        """
        decoded = bolt11.decode(invstring)
        payment_hash = decoded.payment_hash
        preimage = self.payable.get(payment_hash)
        if preimage is None:
            return None
        amount_msat = decoded.amount_msat or 0
        # A plausible routing fee, bounded by whatever budget the mint set,
        # so the fee arithmetic on the melt path is exercised rather than
        # always seeing zero.
        fee = min(1000, maxfee if maxfee is not None else 1000)
        self.pays[payment_hash] = {
            "status": "complete",
            "preimage": preimage,
            "amount_msat": amount_msat,
            "amount_sent_msat": amount_msat + fee,
        }
        return self.pays[payment_hash]

    def signmessage(self, message):
        """cln's signmessage, including the wrapping it does internally.

        The digest is the double-SHA256 over the prefixed message that
        lnurl-mint's signing.verify_note reconstructs; anything else here
        produces signatures that verify against nothing.
        """
        digest = sha256(sha256(_LIGHTNING_SIGNED_MESSAGE_PREFIX + message.encode()).digest()).digest()
        sig = self.key.sign_recoverable(digest, hasher=None)
        # coincurve hands back r ‖ s ‖ recid; clnrest reports the recovery id
        # as its own field, and lnurl-mint parses it as hex.
        return sig[:64].hex(), f"{sig[64]:02x}"


class Handler(BaseHTTPRequestHandler):
    node = None

    def log_message(self, fmt, *args):
        pass  # the harness's own output is the interesting one

    def _send(self, status, body):
        payload = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        try:
            req = json.loads(raw or b"{}")
        except ValueError:
            req = {}
        node = Handler.node
        path = self.path.split("?")[0]

        if path == "/v1/getinfo":
            return self._send(
                200,
                {
                    "id": node.pubkey,
                    "alias": "fake-cln",
                    "color": "7f5af0",
                    "num_active_channels": 1,
                    "num_inactive_channels": 0,
                    "num_peers": 1,
                    "address": [{"address": "127.0.0.1", "port": 9735}],
                },
            )

        if path == "/v1/listchannels":
            return self._send(
                200,
                {"channels": [{"short_channel_id": "1x1x1", "amount_msat": 10_000_000}]},
            )

        if path == "/v1/invoice":
            pr, _ = node.invoice(
                req.get("amount_msat"),
                req.get("label"),
                req.get("description"),
                req.get("preimage"),
            )
            return self._send(200, {"bolt11": pr})

        if path == "/v1/listinvoices":
            inv = node.invoices.get(req.get("payment_hash"))
            if not inv:
                return self._send(200, {"invoices": []})
            entry = {
                "payment_hash": req.get("payment_hash"),
                "status": inv["status"],
                "amount_msat": inv["amount_msat"],
            }
            # cln only populates the preimage once the invoice is paid, and
            # lnurl-mint relies on exactly that to decide whether a mint has
            # settled -- so it must not leak early here either.
            if inv["status"] == "paid":
                entry["payment_preimage"] = inv["preimage"]
            return self._send(200, {"invoices": [entry]})

        if path == "/v1/xpay":
            result = node.xpay(req.get("invstring"), req.get("maxfee"))
            if result is None:
                return self._send(500, {"code": 205, "message": "Could not find a route"})
            return self._send(
                200,
                {
                    "payment_preimage": result["preimage"],
                    "amount_msat": result["amount_msat"],
                    "amount_sent_msat": result["amount_sent_msat"],
                },
            )

        if path == "/v1/listpays":
            pay = node.pays.get(req.get("payment_hash"))
            if not pay:
                return self._send(200, {"pays": []})
            return self._send(
                200,
                {
                    "pays": [
                        {
                            "payment_hash": req.get("payment_hash"),
                            "status": pay["status"],
                            "preimage": pay["preimage"],
                            "amount_msat": pay["amount_msat"],
                            "amount_sent_msat": pay["amount_sent_msat"],
                        }
                    ]
                },
            )

        if path == "/v1/signmessage":
            signature, recid = node.signmessage(req.get("message") or "")
            return self._send(200, {"signature": signature, "recid": recid, "zbase": ""})

        if path == "/control/payable":
            preimage = req.get("preimage")
            payment_hash = bolt11.decode(req["bolt11"]).payment_hash
            node.payable[payment_hash] = preimage
            return self._send(200, {"ok": True, "payment_hash": payment_hash})

        if path == "/control/settle":
            inv = node.invoices.get(req.get("payment_hash"))
            if not inv:
                return self._send(404, {"ok": False, "error": "unknown payment_hash"})
            inv["status"] = "paid"
            return self._send(200, {"ok": True})

        return self._send(404, {"code": 404, "message": f"no such method: {path}"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9737)
    ap.add_argument(
        "--no-autosettle",
        action="store_true",
        help="leave issued invoices unpaid until /control/settle names them",
    )
    args = ap.parse_args()

    Handler.node = FakeNode(autosettle=not args.no_autosettle)
    print(f"fake-cln listening on http://127.0.0.1:{args.port}")
    print(f"  node pubkey: {Handler.node.pubkey}")
    print(f"  autosettle:  {not args.no_autosettle}")
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
