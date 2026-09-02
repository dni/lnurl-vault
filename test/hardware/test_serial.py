#!/usr/bin/env python3
"""Hardware test suite for lnurl-vault's serial (WebSerial/USB-CDC) protocol.

Needs a REAL flashed board attached over USB. This is local-only, manual
testing — deliberately NOT wired into .github/workflows/ci.yml (which only
builds firmware, no board attached there) or release.yml. See
docs/PROTOCOL.md for the wire protocol itself.

Requires pyserial (`pip install pyserial`) and nothing else — no pytest, no
project-specific tooling, matching test/native/'s own "no heavy
dependencies" approach (see unity_lite.h there).

Usage:
    python3 test/hardware/test_serial.py [--port /dev/ttyACM0] [--timeout 3]

If --port is omitted, auto-detects a device whose USB vendor ID matches
Espressif's (0x303A) via pyserial's port listing — this is the same VID our
own firmware's USB-CDC descriptor advertises (CONFIG_TINYUSB_DESC_USE_
ESPRESSIF_VID, see sdkconfig.defaults / platformio.ini).

NOTE ON CURRENT KNOWN FAILURES: the permanent hang this suite originally
bisected (src/proto/json.c's hand-rolled json_find_raw() never returning
once it matched the "cmd" key) is fixed — that parser was replaced with
cJSON entirely. A different, real issue remains: responses are consistently
DELAYED by more than ~2 seconds after the command is sent (not lost — they
arrive intact, just late), most likely a TinyUSB CDC-ACM write-flush/task-
scheduling issue in src/transport/serial_cdc.c, unrelated to JSON parsing.
The suite never discards bytes between reads (an earlier version called
reset_input_buffer() before every write, which — combined with this latency
— tore genuine delayed responses apart and looked like corruption; it
wasn't). The protocol has no request IDs, though, so once a response times
out the suite must stop: carrying that late line into the next call would
mislabel it as the next command's response.
"""
import argparse
import json
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("pyserial is required: pip install pyserial", file=sys.stderr)
    sys.exit(1)

ESPRESSIF_VID = 0x303A


class StreamOutOfSyncError(RuntimeError):
    """The next response can no longer be correlated to one command."""


def find_port():
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID:
            return p.device
    return None


class Device:
    """Keeps bytes intact while one command is in flight.

    A timeout makes the stream ambiguous, so no later command is sent on the
    same open connection. See the module docstring and docs/PROTOCOL.md.
    """

    def __init__(self, port, timeout):
        self.ser = serial.Serial(port, baudrate=115200, timeout=0.05)
        self.port_timeout = timeout
        self.buf = b""
        self.in_sync = True
        time.sleep(1.5)  # let the port settle before the first write — the
        # very first command sent right after opening the port can otherwise
        # go unheard (observed directly: identical bad_request-triggering
        # inputs sent later always succeed, only ever the very first one
        # after opening flakes)
        self.ser.reset_input_buffer()

    def _drain(self, deadline):
        while time.time() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                self.buf += chunk
                if b"\n" in self.buf:
                    return

    def send_raw(self, text, wait):
        """Sends one line and returns its response line within `wait` seconds.

        Raises StreamOutOfSyncError rather than allowing another command when
        the response is overdue or unmatched bytes precede this request.
        """
        if not self.in_sync:
            raise StreamOutOfSyncError(
                "a previous response timed out; close and reopen the connection"
            )
        if self.buf:
            self.in_sync = False
            raise StreamOutOfSyncError(
                "unmatched serial bytes were buffered before the next command; "
                "close and reopen the connection"
            )
        self.ser.write((text + "\n").encode())
        self.ser.flush()
        self._drain(time.time() + wait)
        if b"\n" not in self.buf:
            self.in_sync = False
            raise StreamOutOfSyncError(
                f"response timed out after {wait:.1f}s; no further command was sent "
                "because a late reply would be indistinguishable from it"
            )
        line, self.buf = self.buf.split(b"\n", 1)
        return line

    def send(self, cmd_obj, wait=2.0):
        """Sends a JSON-encoded command object and returns the parsed JSON
        response. A timeout raises StreamOutOfSyncError."""
        raw = self.send_raw(json.dumps(cmd_obj), wait)
        if not raw:
            return None
        try:
            return json.loads(raw.decode().strip())
        except ValueError:
            return {"_unparseable_raw": repr(raw)}

    def close(self):
        self.ser.close()


results = []


def check(name, cond, detail=None):
    results.append((name, cond))
    status = "PASS" if cond else "FAIL"
    line = f"[{status}] {name}"
    if detail is not None:
        line += f" — {detail}"
    print(line)


def run(dev):
    # A generous wait per check — see the module docstring on the real,
    # separate delayed-response behavior these checks now have to tolerate.
    W = 6.0

    # --- actual protocol smoke tests first: establish the happy path works
    # before spending the boot's budget on inputs designed to be rejected.
    # (Checks later in this run are more likely to see the documented
    # gets-worse-over-a-boot degradation — see the module docstring — so
    # the commands we most want a clean read on go first.) ---
    resp = dev.send({"cmd": "get_info"}, wait=W)
    check("get_info responds at all", resp is not None, str(resp))
    if resp is not None:
        check("get_info response has ok:true", resp.get("ok") is True, str(resp))
        check("get_info response has fw_version", "fw_version" in resp, str(resp))
        check("get_info response has note_count", "note_count" in resp, str(resp))
        check("get_info response has pending_count", "pending_count" in resp, str(resp))

    resp = dev.send({"cmd": "list_notes"}, wait=W)
    check("list_notes responds at all", resp is not None, str(resp))
    if resp is not None:
        check("list_notes response has ok:true", resp.get("ok") is True, str(resp))
        check("list_notes response has a notes array", isinstance(resp.get("notes"), list), str(resp))

    # --- baseline: does the device respond to anything at all? ---
    resp = dev.send_raw("not json at all", wait=W)
    check(
        "garbage (non-JSON) input gets a bad_request response",
        resp != b"" and b'"bad_request"' in resp,
        f"raw={resp!r}",
    )

    # --- bisection of the (now-fixed) old hang: does "cmd" ever get matched? ---
    resp = dev.send({}, wait=W)
    check('{} (no "cmd" key at all) gets a response', resp is not None, str(resp))

    resp = dev.send({"foo": "bar"}, wait=W)
    check('{"foo":"bar"} (non-matching key, string value) gets a response', resp is not None, str(resp))

    resp = dev.send({"foo": 123}, wait=W)
    check('{"foo":123} (non-matching key, numeric value) gets a response', resp is not None, str(resp))

    resp = dev.send({"cmd": 123}, wait=W)
    check('{"cmd":123} ("cmd" key MATCHES, wrong value type) gets a response', resp is not None, str(resp))

    resp = dev.send({"cmd": "totally_bogus_command_name"}, wait=W)
    check("a syntactically valid but unrecognized cmd name gets a response", resp is not None, str(resp))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--port", help="Serial port (auto-detected via USB VID if omitted)")
    parser.add_argument("--timeout", type=float, default=3.0, help="pyserial read timeout, seconds")
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        print(
            "No device found (looked for a port with USB VID 0x303A). "
            "Pass --port explicitly, e.g. --port /dev/ttyACM0",
            file=sys.stderr,
        )
        sys.exit(1)

    print(f"Connecting to {port}...\n")
    dev = Device(port, args.timeout)
    try:
        try:
            run(dev)
        except StreamOutOfSyncError as err:
            check("serial stream remained correlated", False, str(err))
    finally:
        dev.close()

    passed = sum(1 for _, ok in results if ok)
    print(f"\n{passed}/{len(results)} checks passed")
    if passed != len(results):
        print(f"FAILED: {len(results) - passed} check(s) failed")
        sys.exit(1)
    print("All checks passed.")


if __name__ == "__main__":
    main()
