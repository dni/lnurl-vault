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

    def __init__(self, port, baud=115200, settle=30.0):
        self.port = serial.Serial()
        self.port.port = port
        self.port.baudrate = baud
        self.port.timeout = 45
        self.port.dtr = False
        self.port.rts = False
        self.port.open()
        # Lines that arrived looking like a reply but would not parse. Kept
        # rather than raised: see cmd(). Set before _wait_ready(), which goes
        # through cmd() and so can append to it.
        self.malformed = []
        self.ready = self._wait_ready(settle)

    def _wait_ready(self, settle):
        """Waits for the board to finish rebooting, rather than guessing at it.

        Opening the port drives DTR/RTS through a USB-UART bridge's auto-reset
        circuit, so for the first moment the board is booting, not listening,
        and a command written to it is simply gone. This used to be a flat
        `time.sleep(3.5)` -- a constant standing in for something that is a
        property of whichever board is plugged in and its reset circuit. On a
        classic T-Display it is too short, so the first command after open was
        dropped on every run (issue #120). bench.py got away with it because
        its opening get_info is a check allowed to fail; e2e_mint.py treated
        the same silence as fatal and died before its first step.

        Probing costs nothing when the device is already up, since the first
        probe answers, and it adapts to a slower board instead of making every
        run pay a worst-case constant.
        """
        # The reset itself needs a moment: probing into a board still in its
        # bootloader only burns a probe.
        time.sleep(1.0)
        prev = self.port.timeout
        # Short reads, so an unanswered probe costs about a second rather than
        # the full command timeout before the next attempt.
        self.port.timeout = 1.0
        try:
            deadline = time.monotonic() + settle
            while time.monotonic() < deadline:
                self.port.reset_input_buffer()
                r = self.cmd({"cmd": "get_info"}, wait=2)
                if r and r.get("ok"):
                    return True
            return False
        finally:
            self.port.timeout = prev
            # A half-written line caught while the board was still coming up is
            # boot noise, not a torn reply, and must not be reported as one at
            # the end of the run.
            self.malformed.clear()

    def cmd(self, payload, wait=45):
        self.port.reset_input_buffer()
        self.port.write((json.dumps(payload) + "\n").encode())
        deadline = time.monotonic() + wait
        base = self.port.timeout
        try:
            return self._read_reply(deadline, base)
        finally:
            self.port.timeout = base

    def _read_reply(self, deadline, base):
        while True:
            # Cap each blocking read to the time actually left. readline()
            # blocks for the whole port timeout however close the deadline is,
            # so a read starting just under it ran on for nearly another full
            # timeout -- which is how a 30s confirm window was measured, and
            # reported, as 75.1s. A timing check has to measure the device,
            # not the harness.
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.port.timeout = min(base, remaining) if base else remaining
            line = self.port.readline().decode(errors="replace").strip()
            if line.startswith("{"):
                try:
                    return json.loads(line)
                except json.JSONDecodeError as e:
                    # A reply that arrived but did not survive the wire, which
                    # is a different fault from silence and must be reported as
                    # such. This used to raise straight out of the harness and
                    # abandon every remaining check, so the one bug the bench
                    # exists to characterise was the one bug that stopped it
                    # running.
                    #
                    # The length is the diagnostic. A run missing that is a
                    # multiple of 64 means whole USB packets went astray; the
                    # first real capture was short by 18, which is what ruled
                    # that explanation out and pointed at the TX ring being
                    # cleared mid-reply instead. The raw bytes say where the
                    # seam is, which is what identifies the missing run.
                    self.malformed.append(line)
                    print(
                        f"  ....  malformed reply, {len(line)} bytes "
                        f"({len(line) % 64} past a 64-byte boundary): {e}"
                    )
                    print(f"        {line!r}")
                    # A torn line may be followed by a good one; keep reading
                    # until the deadline rather than giving up on the command.
                    continue
            # Diagnostics and boot banners share this UART; keep looking.

    def close(self):
        self.port.close()


def note_count(dev):
    """The device's note_count, or None if it did not answer.

    A reply from cmd() must never be dereferenced straight: it is None when
    nothing came back. `dev.cmd({"cmd": "get_info"}).get("note_count")` raised
    AttributeError on the first unanswered get_info and abandoned the rest of
    the run -- the same failure, for the same reason, as the JSONDecodeError
    that used to escape cmd(). A reply that never arrives is a result to
    record, not a crash: on a link being tested precisely because it drops
    replies, the harness must outlive the fault it is measuring.
    """
    r = dev.cmd({"cmd": "get_info"})
    return r.get("note_count") if r else None


def confirmed_note(dev, label):
    """Mints a note and confirms it, so export_secret reaches the UI gate.

    Both halves are checked. This used to return new_secret's id while
    ignoring what confirm answered, so a confirm that timed out or came back
    torn still reported "new_secret + confirm" as a PASS, and the run carried
    on believing there was a CONFIRMED note behind that id. Every check after
    it then failed for reasons that had nothing to do with what it was
    testing, which is a worse outcome than failing here.
    """
    r = dev.cmd({"cmd": "new_secret", "label": label})
    if not r or not r.get("ok"):
        return None
    c = dev.cmd(
        {"cmd": "confirm", "id": r["id"], "amount_msat": 2100, "host": "example.com"}
    )
    if not c or not c.get("ok"):
        return None
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
    count_before = note_count(dev)

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

    count_after = note_count(dev)
    b.check(
        "no note was lost to any of that",
        count_before is not None and count_before == count_after,
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
        torn = list(dev.malformed)
        dev.close()
    rc = b.report()
    # Said after the pass/fail tally because it reframes it: a run whose
    # failures are all "no reply" against a link that also tore N replies is
    # reporting one transport fault, not N unrelated protocol bugs.
    if torn:
        print(f"\n{len(torn)} reply/replies arrived malformed -- the link tore, "
              f"the firmware did not necessarily misbehave:")
        for line in torn:
            print(f"  {len(line)} bytes: {line!r}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
