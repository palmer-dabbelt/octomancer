#!/usr/bin/env python3
"""Check the packet encoder against the worked examples on p105 of
doc/BlackmagicCameraControl.pdf ("Example Protocol Packets"), plus the
Real Time Clock encoding from p102.

Run: .venv/bin/python scripts/test_packets.py
"""

import struct
import sys
from datetime import datetime, timezone

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from timecode_probe import (  # noqa: E402
    build_packet, encode_date, encode_time, decode_bcd_timecode, rtc_packet,
)

# (description, args to build_packet, expected bytes) -- straight from p105.
CASES = [
    (
        "trigger instantaneous auto focus on camera 4",
        dict(category=0, parameter=1, data_type=0, operation=0, payload=b"", dest=4),
        [4, 4, 0, 0, 0, 1, 0, 0],
    ),
    (
        "turn on OIS on all cameras",
        dict(category=0, parameter=6, data_type=0, operation=0, payload=b"\x01", dest=255),
        [255, 5, 0, 0, 0, 6, 0, 0, 1, 0, 0, 0],
    ),
    (
        "set exposure to 10 ms on camera 4",
        dict(category=1, parameter=5, data_type=3, operation=0,
             payload=struct.pack("<i", 10000), dest=4),
        [4, 8, 0, 0, 1, 5, 3, 0, 0x10, 0x27, 0x00, 0x00],
    ),
    (
        "add 15% to zebra level",
        dict(category=4, parameter=2, data_type=128, operation=1,
             payload=b"\x33\x01", dest=4),
        [4, 6, 0, 0, 4, 2, 128, 1, 0x33, 0x01, 0, 0],
    ),
    (
        "select 1080p 23.98 mode on all cameras",
        dict(category=1, parameter=0, data_type=1, operation=0,
             payload=bytes([24, 1, 3, 0, 0]), dest=255),
        [255, 9, 0, 0, 1, 0, 1, 0, 24, 1, 3, 0, 0, 0, 0, 0],
    ),
    (
        "subtract 0.3 from gamma adjust for green & blue",
        dict(category=8, parameter=1, data_type=128, operation=1,
             payload=bytes([0, 0, 0x9A, 0xFD, 0x9A, 0xFD, 0, 0]), dest=4),
        [4, 12, 0, 0, 8, 1, 128, 1, 0, 0, 0x9A, 0xFD, 0x9A, 0xFD, 0, 0],
    ),
]


def hexs(b):
    return " ".join("%02x" % x for x in b)


def main():
    failures = 0

    for desc, kwargs, expected in CASES:
        got = build_packet(**kwargs)
        want = bytes(expected)
        ok = got == want
        failures += not ok
        print("%s  %s" % ("PASS" if ok else "FAIL", desc))
        if not ok:
            print("     want: %s" % hexs(want))
            print("     got:  %s" % hexs(got))

    # --- BCD encoding, p102 / p109 ---
    checks = [
        ("time 09:12:53:10 -> 0x09125310", encode_time(9, 12, 53, 10), 0x09125310),
        ("date 2026-08-24 -> 0x20260824", encode_date(2026, 8, 24), 0x20260824),
        ("time 23:59:59:29 -> 0x23595929", encode_time(23, 59, 59, 29), 0x23595929),
        ("date 1999-12-31 -> 0x19991231", encode_date(1999, 12, 31), 0x19991231),
    ]
    for desc, got, want in checks:
        ok = got == want
        failures += not ok
        print("%s  %s" % ("PASS" if ok else "FAIL", desc))
        if not ok:
            print("     want: 0x%08x  got: 0x%08x" % (want, got))

    # round trip
    rt = decode_bcd_timecode(encode_time(9, 12, 53, 10))
    ok = rt == "09:12:53:10"
    failures += not ok
    print("%s  BCD round trip -> %s" % ("PASS" if ok else "FAIL", rt))

    # non-BCD nibbles must be rejected, not silently mangled
    ok = decode_bcd_timecode(0xAABBCCDD) is None
    failures += not ok
    print("%s  rejects non-BCD nibbles" % ("PASS" if ok else "FAIL"))

    # --- the actual packet we care about ---
    when = datetime(2026, 8, 24, 9, 12, 53, tzinfo=timezone.utc)
    pkt = rtc_packet(when, 10, dest=255)
    want = bytes([255, 12, 0, 0, 7, 0, 3, 0,
                  0x10, 0x53, 0x12, 0x09,     # time  0x09125310 little-endian
                  0x24, 0x08, 0x26, 0x20])    # date  0x20260824 little-endian
    ok = pkt == want
    failures += not ok
    print("%s  Real Time Clock packet (2026-08-24 09:12:53:10 UTC)" % ("PASS" if ok else "FAIL"))
    print("     %s" % hexs(pkt))
    if not ok:
        print("     want: %s" % hexs(want))

    # length field must exclude header and padding
    ok = pkt[1] == 12 and len(pkt) == 16 and len(pkt) % 4 == 0
    failures += not ok
    print("%s  length field excludes header, packet is 32-bit aligned" % ("PASS" if ok else "FAIL"))

    print("\n%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
