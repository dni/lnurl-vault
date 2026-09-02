#!/usr/bin/env python3
"""Every hardware check that does not need a finger on the board.

    python3 test/hardware/bench.py --port /dev/ttyUSB0
    python3 test/hardware/bench.py --port /dev/ttyUSB0 --ble

Prints a pass/fail table and exits non-zero if anything failed, so it can
gate a release. What it deliberately does NOT do is anything that needs a
physical press, and in particular it never grants a wipe -- an automated test
one press away from erasing a device is not a test worth having. Those rows
stay human-run; see docs/HARDWARE-TEST-CHECKLIST.md.

Two things about the serial port, both learned the hard way:

  - It is opened ONCE and held for the whole run. Opening it drives DTR/RTS
    into the board's auto-reset circuit, so a script that opens per command
    reboots the device between every check -- which silently invalidates
    anything measuring uptime or persistence.
  - Boot chatter and any enabled diagnostics write plain text to the same
    UART that carries the protocol, so responses are matched by looking for a
    line that starts with '{' rather than by taking the next line.

BLE needs `pip install bleak`, and is skipped unless --ble is passed.
"""

import argparse
import json
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("needs pyserial: pip install pyserial")

# docs/PROTOCOL.md, and src/transport/ble_gatt.c for the byte order.
BLE_NAME = "lnurl-vault"
BLE_RX = "407e0f1b-2c3d-118e-d64b-1b534e601a9c"
BLE_TX = "407e0f1c-2c3d-118e-d64b-1b534e601a9c"

CONFIRM_WINDOW_S = 30


class Bench:
    def __init__(self):
        self.rows = []

    def check(self, name, ok, detail=""):
        self.rows.append((name, bool(ok), detail))
        print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail else ""))
        return ok

    def skip(self, name, why):
        self.rows.append((name, None, why))
        print(f"  SKIP  {name}  -- {why}")

    def report(self):
        passed = sum(1 for _, ok, _ in self.rows if ok is True)
        failed = [n for n, ok, _ in self.rows if ok is False]
        skipped = sum(1 for _, ok, _ in self.rows if ok is None)
        print(f"\n{passed} passed, {len(failed)} failed, {skipped} skipped")
        for n in failed:
            print(f"  FAILED: {n}")
        return 1 if failed else 0


class Device:
    """One held-open serial connection to the vault."""

    def __init__(self, port, baud=115200):
        self.port = serial.Serial()
        self.port.port = port
        self.port.baudrate = baud
        self.port.timeout = 45
        self.port.dtr = False
        self.port.rts = False
        self.port.open()
        # Opening reset the board. Wait it out and drop the boot chatter.
        time.sleep(3.5)
        self.port.reset_input_buffer()

    def cmd(self, payload, wait=45):
        self.port.reset_input_buffer()
        self.port.write((json.dumps(payload) + "\n").encode())
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            line = self.port.readline().decode(errors="replace").strip()
            if line.startswith("{"):
                return json.loads(line)
            # Diagnostics and boot banners share this UART; keep looking.
        return None

    def close(self):
        self.port.close()


def confirmed_note(dev, label):
    """Mints a note and confirms it, so export_secret reaches the UI gate."""
    r = dev.cmd({"cmd": "new_secret", "label": label})
    if not r or not r.get("ok"):
        return None
    dev.cmd(
        {"cmd": "confirm", "id": r["id"], "amount_msat": 2100, "host": "example.com"}
    )
    return r["id"]


def run_serial(dev, b):
    print("\n-- transport and protocol --")
    info = dev.cmd({"cmd": "get_info"})
    b.check("get_info round-trips", info is not None and info.get("ok"))
    tagged = dev.cmd({"cmd": "get_info", "tag": "bench"})
    b.check("a tagged command's reply carries the tag",
            tagged is not None and tagged.get("tag") == "bench")
    if not info:
        return

    b.check(
        "reports a real firmware version",
        info.get("fw_version") not in (None, "", "0.1.0"),
        f"fw_version={info.get('fw_version')!r}",
    )
    b.check("reports a board identifier", bool(info.get("board")), f"board={info.get('board')!r}")

    # Anything but "ok" means note_count is not a statement about how many
    # notes exist -- see docs/PROTOCOL.md's get_info. Absent entirely on a
    # firmware that predates the field, which is a skip rather than a failure:
    # this driver is meant to be usable against an older build to find out
    # what it does, not only against the newest.
    storage = info.get("storage")
    if storage is None:
        b.skip("storage state is reported", "no `storage` field in this firmware")
    else:
        b.check("storage is healthy", storage == "ok", f"storage={storage!r}")

    if info.get("last_reset_reason") is None:
        b.skip("boot report is present", "no boot report in this firmware")
    else:
        b.check(
            "boot report is present",
            True,
            f"reason={info.get('last_reset_reason')!r} boot={info.get('boot_count')!r}",
        )

    print("\n-- note lifecycle --")
    before = info.get("note_count", 0)
    nid = confirmed_note(dev, "bench")
    b.check("new_secret + confirm", nid is not None)
    after = dev.cmd({"cmd": "get_info"})
    b.check(
        "the new note is counted",
        after is not None and after.get("note_count", 0) == before + 1,
        f"{before} -> {after.get('note_count') if after else '?'}",
    )

    # Page through, rather than assuming one response holds everything. A
    # vault with more notes than fit returns a page plus next_offset, so a
    # client that reads only the first page and concludes a note is missing is
    # simply wrong -- which is what this check did before paging landed, and it
    # failed against a device holding 49 notes with the new one on page 2.
    ids, offset, pages, total = [], 0, 0, None
    while True:
        page = dev.cmd({"cmd": "list_notes", "offset": offset})
        if not page or not page.get("ok"):
            b.check(
                "list_notes answers",
                False,
                f"got {page.get('error') if page else None} at offset {offset}",
            )
            break
        pages += 1
        total = page.get("total")
        ids += [n["id"] for n in page["notes"]]
        if "next_offset" not in page or pages > 50:
            break
        offset = page["next_offset"]

    if total is not None:
        b.check("list_notes includes the new note", nid in ids,
                f"{len(ids)} notes across {pages} page(s), device reports {total}")
        b.check("paging reaches every note", len(ids) == total, f"{len(ids)} of {total}")
        b.check("no note appears twice", len(set(ids)) == len(ids),
                f"{len(set(ids))} distinct of {len(ids)}")

    print("\n-- the disclosure gate --")
    nid2 = confirmed_note(dev, "bench-timeout")
    t0 = time.monotonic()
    r = dev.cmd({"cmd": "export_secret", "id": nid2})
    dt = time.monotonic() - t0
    b.check(
        "an unanswered export_secret times out",
        r is not None and r.get("error") == "timeout",
        f"{dt:.1f}s",
    )
    b.check(
        "and takes about the confirm window, not instantly",
        CONFIRM_WINDOW_S - 2 < dt < CONFIRM_WINDOW_S + 15,
        f"{dt:.1f}s vs a {CONFIRM_WINDOW_S}s window",
    )
    b.check("the device still answers afterwards", dev.cmd({"cmd": "get_info"}) is not None)

    print("\n-- wipe refuses everything it should --")
    count_before = dev.cmd({"cmd": "get_info"}).get("note_count")

    t0 = time.monotonic()
    r = dev.cmd({"cmd": "wipe"}, wait=10)
    dt = time.monotonic() - t0
    if r is not None and r.get("message") == "unknown cmd":
        b.skip("wipe refuses a bare request", "no `wipe` command in this firmware")
        b.skip("wipe refuses a wrong phrase", "no `wipe` command in this firmware")
        b.skip("wipe times out unanswered", "no `wipe` command in this firmware")
        b.skip("granting a wipe", "needs a physical press; never automated on purpose")
        return
    b.check(
        "a bare wipe is refused, with no prompt",
        r is not None and r.get("error") == "bad_request" and dt < 5,
        f"{dt:.1f}s",
    )

    r = dev.cmd({"cmd": "wipe", "confirm": "yes"}, wait=10)
    b.check("a wrong confirmation phrase is refused", r is not None and r.get("error") == "bad_request")

    r = dev.cmd({"cmd": "wipe", "confirm": "WIPE"})
    b.check("an unanswered wipe times out", r is not None and r.get("error") == "timeout")

    count_after = dev.cmd({"cmd": "get_info"}).get("note_count")
    b.check(
        "no note was lost to any of that",
        count_before == count_after,
        f"{count_before} -> {count_after}",
    )
    b.skip("granting a wipe", "needs a physical press; never automated on purpose")

    print("\n-- gestures --")
    b.skip("hold-to-approve grants an export", "needs a physical press")
    b.skip("a tap does not approve", "needs a physical press")
    b.skip("button 2 declines", "needs a physical press")
    b.skip("browse and unveil a QR on screen", "needs a physical press and eyes")


class Frame:
    """Mirror of src/proto/ble_frame.c, for the notify direction."""

    def __init__(self):
        self.buf = bytearray()
        self.want = None
        self.msgs = []

    def feed(self, data):
        self.buf += data
        while True:
            if self.want is None:
                if len(self.buf) < 2:
                    return
                self.want = struct.unpack("<H", self.buf[:2])[0]
                del self.buf[:2]
            if len(self.buf) < self.want:
                return
            self.msgs.append(bytes(self.buf[: self.want]).decode())
            del self.buf[: self.want]
            self.want = None


def run_ble(dev, b):
    import asyncio

    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        b.skip("BLE transport", "needs bleak: pip install bleak")
        return

    async def main():
        print("\n-- BLE transport --")
        found = await BleakScanner.find_device_by_name(BLE_NAME, timeout=20.0)
        if not found:
            b.check("the device advertises", False, f"no {BLE_NAME!r} found")
            return
        b.check("the device advertises", True, found.address)

        async def send(client, frame, payload, wait):
            frame.msgs.clear()
            body = json.dumps(payload).encode()
            wire = struct.pack("<H", len(body)) + body
            chunk = client.mtu_size - 3
            for off in range(0, len(wire), chunk):
                await client.write_gatt_char(BLE_RX, wire[off : off + chunk], response=False)
            t0 = time.monotonic()
            while time.monotonic() - t0 < wait:
                if frame.msgs:
                    return json.loads(frame.msgs.pop(0)), time.monotonic() - t0
                await asyncio.sleep(0.05)
            return None, time.monotonic() - t0

        frame = Frame()
        dropped = asyncio.Event()
        async with BleakClient(found, disconnected_callback=lambda _: dropped.set()) as client:
            await client.start_notify(BLE_TX, lambda _, d: frame.feed(d))
            b.check("MTU was negotiated above the 23-byte default", client.mtu_size > 23,
                    f"mtu={client.mtu_size}")

            r, dt = await send(client, frame, {"cmd": "get_info"}, 10)
            b.check("get_info over BLE", r is not None and r.get("ok"), f"{dt:.2f}s")

            # A command that writes to NVS. This is the one that could not be
            # issued over BLE at all before #29, because dispatcher_handle()
            # ran on the NimBLE host task: the link dropped and the note was
            # never created.
            r, dt = await send(client, frame, {"cmd": "new_secret", "label": "bench-ble"}, 15)
            b.check("a command that writes to flash, over BLE", r is not None and r.get("ok"),
                    f"{dt:.2f}s")
            b.check("the link survived it", not dropped.is_set())

            nid = r["id"] if r and r.get("ok") else None
            if nid:
                await send(client, frame,
                           {"cmd": "confirm", "id": nid, "amount_msat": 2100,
                            "host": "example.com"}, 15)
                # The confirm window blocks a transport task for 30s. The link
                # must stay up and the answer must arrive as a response.
                r, dt = await send(client, frame, {"cmd": "export_secret", "id": nid}, 60)
                b.check("an unanswered export over BLE times out",
                        r is not None and r.get("error") == "timeout", f"{dt:.1f}s")
                b.check("the link survived the whole confirm window", not dropped.is_set())

            r, _ = await send(client, frame, {"cmd": "get_info"}, 10)
            b.check("BLE still works afterwards", r is not None)

    asyncio.run(main())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True, help="e.g. /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--ble", action="store_true", help="also exercise the BLE transport")
    args = ap.parse_args()

    b = Bench()
    dev = Device(args.port, args.baud)
    try:
        run_serial(dev, b)
        if args.ble:
            run_ble(dev, b)
        else:
            b.skip("BLE transport", "pass --ble to include it")
    finally:
        dev.close()
    return b.report()


if __name__ == "__main__":
    sys.exit(main())
