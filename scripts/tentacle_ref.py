#!/usr/bin/env python3
"""
octomancer -- pick a Tentacle box to use as the timecode reference.

Listens to every box in range and chooses one to sync against, then reports how
far this Mac's clock is from it. That offset is the number the camera side
needs: write `mac_utc + offset` to the RTC and the camera lands on Tentacle
time instead of Mac time.

**Every** box gives millisecond-grade time, not just the Track E: the 0x22
payload carries microseconds-within-frame in its last two bytes, so a box that
looks frame-limited is not. Measured across a bench of five, the offsets agree
to within 2 ms. So selection is not about which box is precise enough -- they
all are -- but about which is heard often and reliably enough to trust.

Preference order is therefore: a box named on the command line, then a
microsecond box (type 0x32, simplest to decode and highest advert rate here),
then whichever box was heard most often.

Precision is not the same as being right, and that is the trap this tool exists
to catch. A box can have a beautiful microsecond clock and still be jammed to a
different source than everything else on set -- the Track E here was doing
exactly that earlier, sitting 77 s away from the other four until it was
re-jammed. So the reference is always reported together with whether the rest
of the bench agrees, and a reference that disagrees with the majority is called
out rather than silently used.
"""

import argparse
import asyncio
import json
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from bleak import BleakScanner  # noqa: E402

from tentacle_scan import (  # noqa: E402
    FDAC,
    TYPE_MICROS,
    Box,
    wrap_delta,
)

# Frame-resolution boxes quantise to the second, so two of them can legitimately
# differ by this much without either being wrong.
CLUSTER_TOLERANCE = 1.5


def median(values):
    ordered = sorted(values)
    return ordered[len(ordered) // 2]


def cluster(boxes, tolerance=CLUSTER_TOLERANCE):
    """Group boxes whose offsets agree, largest group first.

    A simple single-pass grouping is enough here: real benches are either all
    jammed together or split into a couple of obvious camps.
    """
    groups = []
    for b in sorted(boxes, key=lambda x: x.offset):
        for g in groups:
            if abs(b.offset - g[0].offset) <= tolerance:
                g.append(b)
                break
        else:
            groups.append([b])
    return sorted(groups, key=len, reverse=True)


def choose(boxes, args):
    """Pick the reference box, and say why.

    Returns (box, reason, warnings).
    """
    warnings = []
    usable = [b for b in boxes if len(b.deltas) >= args.min_adverts]
    if not usable:
        return None, "no box produced %d decodable adverts" % args.min_adverts, warnings

    if args.prefer:
        named = [b for b in usable if args.prefer.lower() in b.name.lower()]
        if named:
            return (named[0], "named on the command line (--prefer %s)"
                    % args.prefer, warnings)
        warnings.append("--prefer %s matched no box; falling back to automatic"
                        % args.prefer)

    micros = [b for b in usable if TYPE_MICROS in b.types]
    if micros:
        # Among microsecond boxes, the one heard most often is the most solid.
        best = max(micros, key=lambda b: len(b.deltas))
        return best, "direct microsecond clock (packet type 0x32)", warnings

    best = max(usable, key=lambda b: len(b.deltas))
    if best.resolution == "frame":
        warnings.append("this box was only heard in the short form, without"
                        " microseconds, so its offset is frame-limited")
        return best, "most adverts heard, frame resolution only", warnings
    return best, "most adverts heard (carries sub-frame microseconds)", warnings


async def run(args):
    boxes = {}

    def cb(dev, adv):
        for uuid, data in (adv.service_data or {}).items():
            if uuid.lower() != FDAC:
                continue
            addr = dev.address
            if addr not in boxes:
                boxes[addr] = Box(addr, adv.local_name or dev.name or "?")
            elif adv.local_name:
                boxes[addr].name = adv.local_name
            boxes[addr].add(bytes(data), adv.rssi, datetime.now())

    if not args.json:
        print("listening %.0fs for Tentacle boxes ..." % args.seconds)
    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    await asyncio.sleep(args.seconds)
    await scanner.stop()

    heard = [b for b in boxes.values() if b.deltas]
    for b in heard:
        b.offset = median(b.deltas)

    if not heard:
        msg = "no Tentacle box produced a readable clock"
        print(json.dumps({"ok": False, "error": msg}) if args.json else msg)
        return 1

    ref, reason, warnings = choose(heard, args)
    groups = cluster(heard)

    consensus = groups[0]
    in_consensus = ref is not None and any(b is ref for b in consensus)
    if ref is not None and not in_consensus and len(heard) > 1:
        warnings.append(
            "the chosen reference disagrees with the %d-box majority by %+.2fs"
            % (len(consensus), wrap_delta(ref.offset - consensus[0].offset)))

    if args.json:
        print(json.dumps({
            "ok": ref is not None,
            "reference": None if ref is None else {
                "name": ref.name,
                "address": ref.address,
                "resolution": ref.resolution,
                "offset_from_mac_s": round(ref.offset, 4),
                "adverts": len(ref.deltas),
                "reason": reason,
            },
            "boxes": [{"name": b.name, "address": b.address,
                       "resolution": b.resolution,
                       "offset_from_mac_s": round(b.offset, 4),
                       "adverts": len(b.deltas)} for b in heard],
            "groups": [[b.name for b in g] for g in groups],
            "warnings": warnings,
        }, indent=2))
        return 0 if ref is not None else 1

    print("\n%d box(es) with a readable clock\n" % len(heard))
    print("  %-20s %-12s %-11s %-8s %s"
          % ("name", "resolution", "offset", "adverts", "spread"))
    for b in sorted(heard, key=lambda x: x.offset):
        spread = max(b.deltas) - min(b.deltas)
        print("  %-20s %-12s %+-11.3f %-8d %.3fs"
              % (b.name[:20], b.resolution, b.offset, len(b.deltas), spread))

    if len(groups) > 1:
        print("\nthe bench is NOT in agreement -- %d separate camps:"
              % len(groups))
        for g in groups:
            print("  %+8.2fs  %s" % (g[0].offset,
                                     ", ".join(b.name[:18] for b in g)))
    else:
        spread = max(b.offset for b in heard) - min(b.offset for b in heard)
        print("\nall boxes agree to within %.3fs" % spread)

    if ref is None:
        print("\nNo usable reference: %s" % reason)
        return 1

    print("\n--- reference ---")
    print("  %s (%s)" % (ref.name, ref.address))
    print("  chosen because: %s" % reason)
    print("  resolution:     %s" % ref.resolution)
    print("  this Mac is %+.3fs from it" % (-ref.offset))
    spread = max(ref.deltas) - min(ref.deltas)
    print("  measured over %d adverts, spread %.3fs" % (len(ref.deltas), spread))
    if ref.resolution == "frame":
        print("  NOTE: heard without microseconds, so this offset is only")
        print("        good to about a frame")

    for w in warnings:
        print("\n  WARNING: %s" % w)
    if warnings and not in_consensus:
        print("  Higher resolution does not mean more correct. If the rest of")
        print("  the bench is jammed together and this box isn't, syncing to")
        print("  it puts the camera 'accurately' on the wrong time.")

    print("\nTo put the camera on this box's time rather than this Mac's, add")
    print("%+.3fs to what set_rtc.py writes:" % ref.offset)
    print("\n    scripts/set_rtc.py --name <camera> --bias %d"
          % round(ref.offset + args.rtc_bias))
    print("\n(that folds in the camera's own %+ds RTC offset; re-measure it with"
          % args.rtc_bias)
    print("set_rtc.py's calibration pass if you haven't confirmed it lately)")
    return 0


def main():
    p = argparse.ArgumentParser(
        description="Choose a Tentacle box as the timecode reference")
    p.add_argument("seconds", nargs="?", type=float, default=30.0,
                   help="how long to listen (default 30)")
    p.add_argument("--prefer", metavar="NAME",
                   help="force a particular box by (sub)name")
    p.add_argument("--min-adverts", type=int, default=3, metavar="N",
                   help="adverts a box needs before it can be the reference")
    p.add_argument("--rtc-bias", type=int, default=75, metavar="SECONDS",
                   help="the camera's own RTC offset, folded into the suggested"
                        " --bias (default 75, as measured on the 6K Pro)")
    p.add_argument("--json", action="store_true",
                   help="machine-readable output, for wiring into other tools")
    args = p.parse_args()
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
