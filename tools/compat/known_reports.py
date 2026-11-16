"""The reports a stock client understands, and the layouts this server sends for them.

A report is `[len][id][payload]` with `len` counting the id and the payload. The ids come from the pinned
telemetrix-esp32 client's dispatch table (`dispatched_ids`); the layouts come from the stock protocol (Global
Constraints of the plan, upstream Telemetrix4Esp32 3.2.0). The client swallows reports it cannot handle (`except
TypeError: continue`), so the proxy is the only place an unknown id or a wrong length can be seen.

A report the client dispatches but this server never sends (7 I2C too few, 12 touch, 13 SPI, 15-19 steppers, 99 debug
print) is not known here: seeing one on the wire is a bug.
"""

I2C_COUNT_MAX = 59  # upstream clamps an I2C read to 59 bytes
ONE_WIRE_SUBTYPES_3 = (28, 32, 35)  # reset, read, crc8: one result byte
ONE_WIRE_SEARCH = 34  # eight ROM bytes


def check_i2c_read(payload):
    """[count, addr, reg, data x count]: the count byte must match the data that follows."""
    if len(payload) < 3 or payload[0] > I2C_COUNT_MAX or len(payload) != 3 + payload[0]:
        return "I2C read report: count does not match the length"
    return None


def check_dht(payload):
    """[0, pin, hum f32, temp f32] (data) or [1, pin, error] (error)."""
    if payload[:1] == b"\x00" and len(payload) == 10:
        return None
    if payload[:1] == b"\x01" and len(payload) == 3:
        return None
    return "DHT report: length does not match its data/error flag"


def check_one_wire(payload):
    """[subtype, result]: subtypes 28, 32, 35 carry one byte, 34 (search) eight."""
    sub = payload[0] if payload else None
    if sub in ONE_WIRE_SUBTYPES_3 and len(payload) == 2:
        return None
    if sub == ONE_WIRE_SEARCH and len(payload) == 9:
        return None
    return "1-wire report: unknown subtype or wrong length"


def fixed(payload_len):
    def check(payload):
        return None if len(payload) == payload_len else f"payload is {len(payload)} bytes, expected {payload_len}"

    return check


# report id -> (name, check(payload bytes) -> error text or None)
LAYOUTS = {
    0: ("loop", fixed(1)),  # [2,0,c]
    2: ("digital", fixed(2)),  # [3,2,pin,v]
    3: ("analog", fixed(3)),  # [4,3,pin,v_hi,v_lo]
    5: ("firmware", fixed(3)),  # [4,5,3,2,100]
    6: ("servo unavailable", fixed(1)),  # [2,6,pin]
    8: ("I2C error", fixed(2)),  # [3,8,1,addr]
    9: ("I2C read", check_i2c_read),  # [count+4,9,count,addr,reg,data...]
    10: ("sonar", fixed(3)),  # [4,10,trig,cm_hi,cm_lo]
    11: ("DHT", check_dht),
    14: ("1-wire", check_one_wire),
}

KNOWN_IDS = frozenset(LAYOUTS)


def dispatched_ids():
    """The report ids the pinned stock client has a handler for (read from a client built without connecting)."""
    from telemetrix_esp32 import telemetrix_esp32

    client = telemetrix_esp32.TelemetrixEsp32("0.0.0.0", autostart=False)
    return frozenset(client.report_dispatch)


def check_report(report_id, payload):
    """None when a server-to-client report is one the stock client knows, else the reason it is not.

    `payload` is the bytes after the id; the length byte is len(payload) + 1.
    """
    if report_id not in LAYOUTS:
        return f"report id {report_id} is not one the stock client dispatches and this server sends"
    name, check = LAYOUTS[report_id]
    error = check(bytes(payload))
    return f"{name} report (id {report_id}): {error}" if error else None
