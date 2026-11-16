"""Drive the stock telemetrix-esp32 client through the recording proxy, and compare two recordings.

    compat_test.py --host H [--port P] [--loopback OUT,IN] [--i2c-addr A] [--dht PIN] [--sonar TRIG,ECHO]
                   [--onewire PIN] [--log FILE] [--strict]
    compat_test.py --compare A.log B.log

The first form runs the same scripted session every time: LOOP echo, the digital loopback (OUT driven, IN read, default 32,33;
wire them together), analog input on IN when the stock client allows it, then I2C, DHT, sonar and 1-wire for the options
given. Run it against upstream Telemetrix4Esp32 and against this component, each with its own --log, then compare.

The stock client is awkward at exit: its constructor connects and takes about 0.6 s, shutdown() sends STOP_ALL_REPORTS
and RESET (upstream firmware reboots on RESET) but leaves the socket open, so the socket is shut down here.

--compare normalises what legitimately differs between two boards and diffs the rest: timestamps are dropped; the
firmware build byte (3.2.0 against 3.2.100) is zeroed; digital reports are reduced, per pin, to the sequence of distinct
consecutive values (a board that sends an initial or repeated report equal to the neighbouring one is not different),
and a digital report on an input pin that arrives after its SET_PIN_MODE and before the next DIGITAL_WRITE command (the
initial report, whose value is the input's before the loopback drives it) is left out;
a recording with no server packets is itself a difference; analog, sonar and DHT reports are periodic and carry sensor
values, so they are taken out of the sequence and compared as the set of (kind, pin, DHT data/error) seen. Everything
else (every command, every reply, I2C data, 1-wire results) must match byte for byte. The
differences are printed with context and the exit status is 1.
"""

import argparse
import difflib
import json
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import known_reports
from recording_proxy import RecordingProxy

WAIT = 5.0
FIRMWARE_ID = 5
SET_PIN_MODE_ID = 1
DIGITAL_WRITE_ID = 2
DIGITAL_INPUT_MODES = {0, 2, 9}  # input, pull-up, pull-down
PERIODIC = {3: "analog", 10: "sonar", 11: "dht"}


class Probe:
    """Callback target: keeps every report and lets a step wait for one."""

    def __init__(self):
        self.reports = []
        self.cond = threading.Condition()

    def __call__(self, report):
        with self.cond:
            self.reports.append(list(report))
            self.cond.notify_all()

    def wait(self, match, timeout=WAIT):
        """The first report match accepts, or None after timeout."""
        deadline = time.monotonic() + timeout
        seen = 0
        with self.cond:
            while True:
                for report in self.reports[seen:]:
                    if match(report):
                        return report
                seen = len(self.reports)
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self.cond.wait(left)


class Session:
    """Runs steps and keeps the failures; one failed step does not stop the others."""

    def __init__(self):
        self.failures = []

    def step(self, name, ok, detail=""):
        print(
            f"{'ok  ' if ok else 'FAIL'} {name}{': ' + detail if detail and not ok else ''}",
            flush=True,
        )
        if not ok:
            self.failures.append(name)


def run_steps(client, args, session):
    out, in_ = args.loopback
    probe = Probe()
    client.loop_back("B", probe)
    session.step(
        "loop echo", probe.wait(lambda r: r == [ord("B")]) is not None, "no echo of B"
    )

    digital = Probe()
    client.set_pin_mode_digital_output(out)
    client.digital_write(out, 0)
    client.set_pin_mode_digital_input(in_, digital)
    time.sleep(
        0.3
    )  # an initial report may arrive; a board that sends none is not wrong
    for level in (1, 0, 1, 0):
        client.digital_write(out, level)
        got = digital.wait(lambda r, level=level: r[1] == in_ and r[2] == level)
        session.step(
            f"digital loopback {out}->{in_} level {level}", got is not None, "no report"
        )
    client.disable_digital_reporting(in_)

    if in_ in client.valid_analog_in_pins:
        analog = Probe()
        client.set_pin_mode_analog_input(in_, 0, analog)
        first = analog.wait(lambda r: r[1] == in_)
        client.disable_analog_reporting(in_)
        session.step(f"analog input on {in_}", first is not None, "no report")
    else:
        print(f"skip analog input: {in_} is not an analog pin for the stock client")

    if args.i2c_addr is not None:
        i2c = Probe()
        client.set_pin_mode_i2c()
        client.i2c_read(args.i2c_addr, 0, 1, i2c)
        got = i2c.wait(lambda r: r[2] == args.i2c_addr)
        session.step(f"i2c read from {args.i2c_addr:#x}", got is not None, "no report")

    if args.onewire is not None:
        onewire = Probe()
        client.set_pin_mode_one_wire(args.onewire)
        client.onewire_reset(onewire)
        presence = onewire.wait(lambda r: r[1] == 28)
        session.step(
            "1-wire presence",
            presence is not None and presence[2] == 1,
            f"got {presence}",
        )
        client.onewire_reset_search()
        client.onewire_search(onewire)
        found = onewire.wait(lambda r: r[1] == 34)
        session.step(
            "1-wire search",
            found is not None and found[2:10] != [0xFF] * 8,
            f"got {found}",
        )
        if found:
            client.onewire_crc8(found[2:10], onewire)
            crc = onewire.wait(lambda r: r[1] == 35)
            session.step(
                "1-wire crc8", crc is not None and crc[2] == found[9], f"got {crc}"
            )

    if args.dht is not None:
        dht = Probe()
        client.set_pin_mode_dht(args.dht, dht)
        got = dht.wait(lambda r: r[2] == args.dht, timeout=8)
        session.step(f"dht on {args.dht}", got is not None, "no report")

    if args.sonar is not None:
        sonar = Probe()
        client.set_pin_mode_sonar(args.sonar[0], args.sonar[1], sonar)
        got = sonar.wait(lambda r: r[1] == args.sonar[0], timeout=3)
        session.step(
            f"sonar on {args.sonar[0]},{args.sonar[1]}", got is not None, "no report"
        )


def run(args):
    from telemetrix_esp32 import telemetrix_esp32

    proxy = RecordingProxy(
        0, (args.host, args.port), args.log, strict=args.strict
    ).start()
    session = Session()
    client = None
    try:
        client = telemetrix_esp32.TelemetrixEsp32(
            "127.0.0.1", ip_port=proxy.port, shutdown_on_exception=False
        )
        print(f"firmware {client.firmware_version}", flush=True)
        run_steps(client, args, session)
    except Exception as exc:  # noqa: BLE001 - the log up to here is still worth keeping
        session.step("session", False, repr(exc))
    finally:
        if client is not None:
            try:
                client.shutdown()  # STOP_ALL_REPORTS, then RESET: upstream reboots here
            except Exception as exc:  # noqa: BLE001
                session.step("client shutdown", False, repr(exc))
            try:
                client.sock.shutdown(
                    2
                )  # shutdown() leaves the socket open, its receiver thread in recv()
            except OSError:
                pass
            client.sock.close()
        time.sleep(0.2)
        violations = proxy.stop()
    if args.strict:
        session.step(
            "strict: every server report is one the stock client knows",
            not violations,
            "; ".join(violations),
        )
    print(f"log written to {args.log}")
    return 1 if session.failures else 0


def load_log(path):
    return [
        json.loads(line) for line in Path(path).read_text().splitlines() if line.strip()
    ]


def normalise(entries):
    """(sequence of text lines, set of periodic report keys) for one recording."""
    lines = []
    periodic = set()
    digital = {}  # pin -> last value seen: a repeated value is not a change, whether or not a board sends it
    settling = set()  # input pins set up since the last DIGITAL_WRITE: their initial report is not compared
    for e in entries:
        payload = bytes.fromhex(e["bytes"])
        rid = e["id"]
        if e["dir"] == "c2s" and rid == SET_PIN_MODE_ID and len(payload) >= 2:
            if payload[1] in DIGITAL_INPUT_MODES:
                settling.add(payload[0])
        elif e["dir"] == "c2s" and rid == DIGITAL_WRITE_ID:
            settling.clear()
        if e["dir"] == "s2c" and rid in PERIODIC:
            if rid == 11:
                key = (
                    PERIODIC[rid],
                    payload[1] if len(payload) > 1 else None,
                    "data" if payload[:1] == b"\0" else "error",
                )
            else:
                key = (PERIODIC[rid], payload[0] if payload else None)
            periodic.add(key)
            continue
        if (
            e["dir"] == "s2c"
            and rid == 2
            and len(payload) == 2
            and not e.get("partial")
        ):
            if payload[0] in settling:
                continue  # a board may or may not send an initial report, and its value is the floating input's
            if digital.get(payload[0]) == payload[1]:
                continue
            digital[payload[0]] = payload[1]
        if e["dir"] == "s2c" and rid == FIRMWARE_ID and payload:
            payload = payload[:-1] + b"\0"  # 3.2.0 against 3.2.100
        name = (
            known_reports.LAYOUTS[rid][0]
            if e["dir"] == "s2c" and rid in known_reports.LAYOUTS
            else ""
        )
        partial = " PARTIAL" if e.get("partial") else ""
        lines.append(
            f"{e['dir']} len={e['len']} id={rid} {payload.hex()} {name}{partial}".rstrip()
        )
    return lines, periodic


def compare(path_a, path_b):
    """The differences between two recordings after normalising, as a list of text lines (empty = same)."""
    entries_a, entries_b = load_log(path_a), load_log(path_b)
    diff = [
        f"{path} has no server reports: nothing to compare"
        for path, entries in ((path_a, entries_a), (path_b, entries_b))
        if not any(e["dir"] == "s2c" for e in entries)
    ]
    lines_a, periodic_a = normalise(entries_a)
    lines_b, periodic_b = normalise(entries_b)
    diff += list(
        difflib.unified_diff(
            lines_a, lines_b, str(path_a), str(path_b), n=3, lineterm=""
        )
    )
    for label, only in (
        ("only in " + str(path_a), periodic_a - periodic_b),
        ("only in " + str(path_b), periodic_b - periodic_a),
    ):
        for key in sorted(only, key=str):
            diff.append(f"periodic report {label}: {key}")
    return diff


def pair(text, name):
    try:
        a, b = (int(x, 0) for x in text.split(","))
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"{name} must be two pins like 32,33"
        ) from None
    return a, b


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--host")
    parser.add_argument("--port", type=int, default=31336)
    parser.add_argument(
        "--loopback",
        type=lambda t: pair(t, "--loopback"),
        default=(32, 33),
        metavar="OUT,IN",
    )
    parser.add_argument("--i2c-addr", type=lambda t: int(t, 0), metavar="A")
    parser.add_argument("--dht", type=int, metavar="PIN")
    parser.add_argument(
        "--sonar", type=lambda t: pair(t, "--sonar"), metavar="TRIG,ECHO"
    )
    parser.add_argument("--onewire", type=int, metavar="PIN")
    parser.add_argument("--log", default="compat.log", metavar="FILE")
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--compare", nargs=2, metavar=("A.log", "B.log"))
    args = parser.parse_args(argv)

    if args.compare:
        diff = compare(*args.compare)
        if diff:
            print("\n".join(diff))
            return 1
        print("recordings match after normalising")
        return 0
    if not args.host:
        parser.error("--host is required (or --compare A.log B.log)")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
