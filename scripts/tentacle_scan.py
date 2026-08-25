#!/usr/bin/env python3
"""
octomancer -- read timecode from Tentacle Sync boxes over BLE.

Tentacles broadcast their timecode in the *advertising* payload, so this is
passive: no connection, no pairing, and no limit on how many boxes you listen
to at once. Point it at a room and it reports what every box in range thinks
the time is.

The payload arrives as BLE **service data** under the 16-bit UUID `FDAC`, not
as manufacturer data. Reverse-engineered against a bench of five boxes; see
doc/tentacle-notes.md for the evidence and for what is still unknown.

Two payload types carry a clock:

    0x22, 9 bytes -- timecode, good to a few milliseconds
        byte 0   type 0x22
        byte 1   flags         low 6 bits 0x3d; bit 0x40 changes over minutes
        byte 2   frame rate    low 6 bits = fps (0x18 = 24); bit 0x40 as above
        byte 3   hours         plain binary, NOT BCD
        byte 4   minutes
        byte 5   seconds
        byte 6   frames
        byte 7-8 microseconds within the frame, big-endian

    0x32, 8 bytes -- microsecond clock, seen on a Track E
        byte 0   type 0x32
        byte 1-2 as above
        byte 3-7 microseconds since midnight, 40-bit big-endian

    0x42, 9 bytes -- a static payload with no clock in it.

Note the contrast with Blackmagic, which sends timecode as packed BCD: here
0x15 means 21, not 15. Decoding these as BCD yields plausible-looking nonsense
rather than an obvious error, which is why it is worth stating twice.
"""

import argparse
import asyncio
import sys
from datetime import datetime

from bleak import BleakScanner

# Assigned to Tentacle Sync; the boxes put their clock here as service data.
FDAC = "0000fdac-0000-1000-8000-00805f9b34fb"

TYPE_TIMECODE = 0x22
TYPE_MICROS = 0x32
TYPE_STATIC = 0x42


def secs_of_day(when):
    return (when.hour * 3600 + when.minute * 60 + when.second
            + when.microsecond / 1e6)


def wrap_delta(seconds):
    """Shortest signed distance around a 24-hour clock."""
    return (seconds + 43200.0) % 86400.0 - 43200.0


def decode(data):
    """Decode one FDAC payload.

    Returns a dict with `sod` (seconds since midnight, float) or None for
    anything we cannot read as a clock -- better to say so than to print a
    plausible wrong number.
    """
    out = {"raw": data, "type": data[0] if data else None,
           "sod": None, "display": None, "frames": None, "note": ""}
    if not data:
        out["note"] = "empty"
        return out

    if data[0] == TYPE_TIMECODE and len(data) >= 7:
        h, m, s, f = data[3], data[4], data[5], data[6]
        # Range-check rather than trusting the layout: a box that violates
        # these bounds is telling us the format guess is wrong.
        if h > 23 or m > 59 or s > 59:
            out["note"] = "out of range: %d:%d:%d" % (h, m, s)
            return out
        fps = data[2] & 0x3F
        if not 1 <= fps <= 60:
            out["note"] = "implausible frame rate %d" % fps
            return out

        sod = float(h * 3600 + m * 60 + s) + f / float(fps)
        out["fps"] = fps
        out["flags"] = data[1]
        out["frames"] = f

        # Bytes 7-8 are microseconds within the frame. Including them takes
        # this from frame resolution (~42 ms) to a few ms: fitting the decoded
        # time against the host clock, residual drops 3-18x on every box
        # tested. There is an unexplained ~3.6 ms floor in the field (its
        # observed minimum is ~3600, not 0), so treat the absolute value as
        # carrying a small constant bias; the rate is what matters for sync.
        if len(data) >= 9:
            micros = int.from_bytes(data[7:9], "big")
            out["micros"] = micros
            sod += micros / 1e6
            out["display"] = ("%02d:%02d:%02d:%02d.%03d"
                              % (h, m, s, f, micros // 1000))
        else:
            out["display"] = "%02d:%02d:%02d:%02d" % (h, m, s, f)

        out["sod"] = sod
        return out

    if data[0] == TYPE_MICROS and len(data) >= 8:
        micros = int.from_bytes(data[3:8], "big")
        sod = micros / 1e6
        if sod >= 86400.0:
            out["note"] = "counter out of day range (%d)" % micros
            return out
        out["sod"] = sod
        out["display"] = ("%02d:%02d:%06.3f"
                          % (int(sod // 3600), int(sod % 3600 // 60), sod % 60))
        out["flags"] = data[1]
        return out

    if data[0] == TYPE_STATIC:
        out["note"] = "static/info packet"
        return out

    out["note"] = "unknown type 0x%02x (%dB)" % (data[0], len(data))
    return out


class Box:
    """One Tentacle, accumulating what we have heard from it."""

    def __init__(self, address, name):
        self.address = address
        self.name = name
        self.rssi = None
        self.adverts = 0
        self.decoded = 0
        self.last = None          # last decoded dict
        self.last_seen = None
        self.deltas = []
        self.types = set()
        self.flags = set()
        self.max_frame = -1
        self.reported_fps = None
        self.micros_seen = False
        self.notes = set()

    def add(self, data, rssi, when):
        self.adverts += 1
        self.rssi = rssi
        self.last_seen = when
        info = decode(data)
        if info["type"] is not None:
            self.types.add(info["type"])
        if info["sod"] is None:
            if info["note"]:
                self.notes.add(info["note"])
            return info
        self.decoded += 1
        self.last = info
        self.flags.add(info.get("flags"))
        if info.get("fps"):
            self.reported_fps = info["fps"]
        if info.get("micros") is not None:
            self.micros_seen = True
        if info["frames"] is not None:
            self.max_frame = max(self.max_frame, info["frames"])
        self.deltas.append(wrap_delta(info["sod"] - secs_of_day(when)))
        return info

    @property
    def resolution(self):
        if TYPE_MICROS in self.types:
            return "microsecond"
        if TYPE_TIMECODE in self.types:
            # Bytes 7-8 carry microseconds within the frame, so this is not
            # frame-limited despite the frame field.
            return "frame+us" if self.micros_seen else "frame"
        return "none"

    def fps(self):
        """Frame rate as the box reports it, in byte 2.

        Read it rather than inferring it from the largest frame number seen:
        a box heard only a few times may never show a high frame, which is how
        a 24 fps box gets misreported as 22 fps.
        """
        return self.reported_fps


async def scan(args):
    boxes = {}

    def cb(dev, adv):
        for uuid, data in (adv.service_data or {}).items():
            if uuid.lower() != FDAC:
                continue
            data = bytes(data)
            addr = dev.address
            if addr not in boxes:
                boxes[addr] = Box(addr, adv.local_name or dev.name or "?")
            elif adv.local_name:
                boxes[addr].name = adv.local_name
            info = boxes[addr].add(data, adv.rssi, datetime.now())
            if args.raw:
                print("%s  %-20s %-14s %s"
                      % (datetime.now().strftime("%H:%M:%S.%f")[:-3],
                         boxes[addr].name[:20],
                         info["display"] or info["note"][:14],
                         data.hex(" ")))

    print("listening %.0fs for Tentacle boxes (service data %s) ..."
          % (args.seconds, FDAC[:8]))
    if not args.raw:
        print("nothing is transmitted -- this is passive, receive only.\n")

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    await asyncio.sleep(args.seconds)
    await scanner.stop()

    if not boxes:
        print("\nNo Tentacle boxes seen.")
        print("They advertise under the name of the camera they are strapped")
        print("to, so don't look for one called 'Tentacle' -- match on the FDAC")
        print("service data, which is what this does.")
        return 1

    print("\n%d box(es) heard\n" % len(boxes))
    now = datetime.now()
    print("  %-20s %-6s %-14s %-9s %-8s %s"
          % ("name", "rssi", "clock", "vs Mac", "adverts", "address"))
    ordered = sorted(boxes.values(), key=lambda b: (b.rssi or -999),
                     reverse=True)
    for b in ordered:
        print("  %-20s %-6s %-14s %-9s %-8s %s"
              % (b.name[:20], b.rssi,
                 b.last["display"] if b.last else "--",
                 "%+.2fs" % b.deltas[-1] if b.deltas else "-",
                 "%d/%d" % (b.decoded, b.adverts), b.address))

    print("\n--- detail ---")
    for b in ordered:
        print("\n%s  (%s)" % (b.name, b.address))
        print("  rssi %s, %d adverts, %d carried a clock"
              % (b.rssi, b.adverts, b.decoded))
        print("  packet types: %s   resolution: %s"
              % (", ".join("0x%02x" % t for t in sorted(b.types)),
                 b.resolution))
        flags = [f for f in b.flags if f is not None]
        if flags:
            print("  flags byte:   %s" % ", ".join("0x%02x" % f
                                                   for f in sorted(flags)))
        if b.deltas:
            lo, hi = min(b.deltas), max(b.deltas)
            print("  clock:        %s, %+.2fs from this Mac (range %+.2f..%+.2f)"
                  % (b.last["display"], b.deltas[-1], lo, hi))
            if b.max_frame >= 0:
                print("  frames:       0..%d seen, box reports %s fps"
                      % (b.max_frame, b.fps() or "?"))
        if b.notes:
            print("  not decoded:  %s" % "; ".join(sorted(b.notes)))
        if b.last_seen:
            print("  last heard:   %.1fs ago"
                  % (now - b.last_seen).total_seconds())

    # Boxes jammed to each other should agree. Disagreement is the thing worth
    # knowing about on set, so check it explicitly rather than assuming.
    #
    # Compare each box's offset *from this Mac*, not its last raw reading.
    # Adverts from different boxes arrive at different instants, so comparing
    # last-seen values charges each box for how stale its last advert was --
    # a box heard 4s ago looks 4s out of sync when it is fine. Each delta was
    # taken against the Mac at that box's own moment of receipt, so the
    # staleness cancels.
    synced = [b for b in ordered if b.deltas]
    if len(synced) > 1:
        print("\n--- are they in sync with each other? ---")

        def offset(box):
            ds = sorted(box.deltas)
            return ds[len(ds) // 2]        # median, robust to a stray advert

        base = max(synced, key=lambda b: len(b.deltas))
        print("  measured as each box's offset from this Mac, so a box that")
        print("  hasn't been heard from recently isn't penalised for it\n")
        worst = 0.0
        for b in synced:
            gap = wrap_delta(offset(b) - offset(base))
            if b is not base:
                worst = max(worst, abs(gap))
            print("  %-20s %+7.2fs vs Mac   %+7.2fs vs %s%s"
                  % (b.name[:20], offset(b), gap, base.name[:12],
                     "   <- reference" if b is base else ""))

        # Every payload type carries sub-frame microseconds, so offsets are
        # good to milliseconds -- unless a box was heard only as the truncated
        # 7-byte form, which is frame-limited.
        quantised = any(b.resolution == "frame" for b in synced)
        limit = 1.5 if quantised else 0.05
        print()
        if worst <= limit:
            print("  All boxes agree to within %.3fs -- they are jammed"
                  % worst)
            print("  together.")
            if quantised:
                print("  One box was heard without microseconds, so about a")
                print("  frame of that spread is quantisation, not drift.")
        else:
            print("  Up to %.2fs apart -- these are NOT all jammed to the same"
                  % worst)
            print("  source. A gap far larger than a second is a real")
            print("  difference, not a sampling artefact.")
    return 0


def main():
    p = argparse.ArgumentParser(
        description="Passively read timecode from Tentacle Sync boxes over BLE")
    p.add_argument("seconds", nargs="?", type=float, default=20.0,
                   help="how long to listen (default 20)")
    p.add_argument("--raw", action="store_true",
                   help="print every advert as it arrives, with raw bytes")
    args = p.parse_args()
    try:
        return asyncio.run(scan(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
