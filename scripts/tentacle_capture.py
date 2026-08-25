#!/usr/bin/env python3
"""
octomancer -- record raw Tentacle BLE adverts to a file for offline analysis.

Reverse engineering wants the same capture examined several ways, and re-running
the radio for every new idea is slow and not reproducible: the boxes move, the
timecode advances, RSSI wanders. So capture once to JSONL and analyse the file
as many times as needed.

Each line is one advert:

    {"t": 12.345, "wall": "21:10:38.123", "addr": "...", "name": "BMPCC",
     "rssi": -40, "hex": "22 7d 58 15 00 3a 0a a3 92"}

`t` is monotonic seconds since capture start, which is what any rate or drift
fit should use -- wall clock can step under NTP, monotonic cannot.
"""

import argparse
import asyncio
import json
import sys
import time
from datetime import datetime

from bleak import BleakScanner

FDAC = "0000fdac-0000-1000-8000-00805f9b34fb"


async def capture(args):
    out = open(args.output, "w")
    t0 = time.monotonic()
    counts = {}

    def cb(dev, adv):
        for uuid, data in (adv.service_data or {}).items():
            if uuid.lower() != FDAC:
                continue
            data = bytes(data)
            now = datetime.now()
            rec = {
                "t": round(time.monotonic() - t0, 6),
                "wall": now.strftime("%H:%M:%S.%f")[:-3],
                "wall_sod": (now.hour * 3600 + now.minute * 60 + now.second
                             + now.microsecond / 1e6),
                "addr": dev.address,
                "name": adv.local_name or dev.name or "?",
                "rssi": adv.rssi,
                "hex": data.hex(" "),
            }
            out.write(json.dumps(rec) + "\n")
            # Flush as we go: a long capture is worth watching, and an
            # interrupted one should still leave usable data behind.
            out.flush()
            counts[rec["name"]] = counts.get(rec["name"], 0) + 1

    print("capturing %.0fs to %s ..." % (args.seconds, args.output))
    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()

    # Progress, so a multi-minute capture doesn't look like a hang.
    step = max(1.0, args.seconds / 10.0)
    elapsed = 0.0
    while elapsed < args.seconds:
        await asyncio.sleep(min(step, args.seconds - elapsed))
        elapsed = time.monotonic() - t0
        print("  %5.0fs  %d adverts from %d box(es)"
              % (elapsed, sum(counts.values()), len(counts)), flush=True)
    await scanner.stop()
    out.close()

    print("\nwrote %s" % args.output)
    for name, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print("  %-22s %d" % (name, n))
    return 0 if counts else 1


def main():
    p = argparse.ArgumentParser(
        description="Record raw Tentacle FDAC adverts to JSONL")
    p.add_argument("seconds", nargs="?", type=float, default=120.0,
                   help="capture duration (default 120)")
    p.add_argument("-o", "--output", default="tentacle-capture.jsonl",
                   help="output file (default tentacle-capture.jsonl)")
    args = p.parse_args()
    try:
        return asyncio.run(capture(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
