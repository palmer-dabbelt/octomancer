#!/usr/bin/env python3
"""
octomancer -- summarise an octomancer_sync.py log.

The daemon logs every cycle, including the ones where it did nothing, so that
these questions can be answered from evidence rather than guessed at:

  * How fast does the camera actually drift away from Tentacle time?
  * Given a tolerance, how often does it therefore need correcting?
  * Do the Tentacle boxes stay agreed with each other?
  * Are writes landing, and did the learned RTC bias settle?

The drift figure is the one that matters for tuning: --poll and --tolerance
should be set from a measured drift rate, and until there are a few hours of
log there is nothing to set them from.

    scripts/sync_report.py octomancer-sync.jsonl
"""

import argparse
import json
import sys
from collections import Counter, defaultdict


def linfit(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    den = sum((x - mx) ** 2 for x in xs)
    if den == 0:
        return None
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den
    inter = my - slope * mx
    resid = max(abs(y - (slope * x + inter)) for x, y in zip(xs, ys))
    return slope, inter, resid


def parse_wall(s):
    """'2026-08-24T21:42:05.123' -> seconds since midnight."""
    try:
        t = s.split("T", 1)[1]
        h, m, rest = t.split(":")
        return int(h) * 3600 + int(m) * 60 + float(rest)
    except Exception:
        return None


def main():
    p = argparse.ArgumentParser(description="Summarise an octomancer sync log")
    p.add_argument("path", nargs="?", default="octomancer-sync.jsonl")
    p.add_argument("--segments", action="store_true",
                   help="show each free-running segment between writes")
    p.add_argument("--min-segment", type=float, default=1800.0,
                   metavar="SECONDS",
                   help="shortest free-running stretch that can measure drift "
                        "(default 1800; shorter ones are all quantisation)")
    args = p.parse_args()

    try:
        rows = [json.loads(l) for l in open(args.path) if l.strip()]
    except FileNotFoundError:
        print("no log at %s -- run octomancer_sync.py first" % args.path)
        return 1

    cycles = [r for r in rows if r.get("event") == "cycle"]
    if not cycles:
        print("%d records but no cycles yet" % len(rows))
        return 1

    t0 = parse_wall(cycles[0]["wall"])
    t1 = parse_wall(cycles[-1]["wall"])
    span = (t1 - t0) if (t0 is not None and t1 is not None) else 0
    print("%d cycles over %.1f minutes  (%s)"
          % (len(cycles), span / 60.0, args.path))

    print("\n--- what happened ---")
    for action, n in Counter(r.get("action", "?")
                             for r in cycles).most_common():
        print("  %-26s %4d  (%.0f%%)"
              % (action, n, 100.0 * n / len(cycles)))

    # --- Tentacle side ------------------------------------------------
    spreads = [r["tentacle_spread_s"] for r in cycles
               if "tentacle_spread_s" in r]
    if spreads:
        print("\n--- Tentacle bench ---")
        srt = sorted(spreads)
        print("  boxes heard:   %s"
              % ", ".join("%d boxes x%d" % (n, c) for n, c in sorted(
                  Counter(r.get("tentacles", 0) for r in cycles).items())))
        print("  spread between boxes: median %.4fs  worst %.4fs"
              % (srt[len(srt) // 2], srt[-1]))
        disagreed = sum(1 for r in cycles if r.get("bench_disagreement"))
        if disagreed:
            print("  WARNING: %d cycle(s) where the boxes did not agree"
                  % disagreed)

        per_box = defaultdict(list)
        for r in cycles:
            for name, b in (r.get("boxes") or {}).items():
                per_box[name].append(b["offset_s"])
        print("\n  per box, offset from this Mac:")
        for name, offs in sorted(per_box.items()):
            o = sorted(offs)
            print("    %-18s n=%-4d median %+.4fs  range %+.4f..%+.4f"
                  % (name, len(offs), o[len(o) // 2], o[0], o[-1]))

    # --- camera drift --------------------------------------------------
    obs = [(parse_wall(r["wall"]), r["error_s"]) for r in cycles
           if "error_s" in r and parse_wall(r["wall"]) is not None]
    if len(obs) < 2:
        print("\nnot enough camera observations yet to say anything about drift")
        return 0

    print("\n--- camera error vs Tentacle time ---")
    errs = sorted(e for _t, e in obs)
    print("  observations: %d   median %+.3fs   range %+.3f..%+.3f"
          % (len(errs), errs[len(errs) // 2], errs[0], errs[-1]))

    # Split into segments that were free-running: a write resets the clock, so
    # fitting across one would measure the correction, not the drift.
    segments, cur = [], []
    for r in cycles:
        t = parse_wall(r["wall"])
        if t is None or "error_s" not in r:
            continue
        cur.append((t, r["error_s"]))
        if str(r.get("action", "")).startswith("write"):
            if len(cur) >= 3:
                segments.append(cur)
            cur = []
    if len(cur) >= 3:
        segments.append(cur)

    # A short segment cannot measure drift, however good the fit looks. The
    # camera reports whole frames, so each reading is quantised to 1/fps -- at
    # 24 fps that is 42 ms, which over a 30-second stretch is +/-1400 ppm of
    # apparent drift. Fitting that produces a confident-looking number made
    # entirely of quantisation, so refuse to report it.
    long_enough = [g for g in segments if g[-1][0] - g[0][0] >= args.min_segment]
    dropped = len(segments) - len(long_enough)

    if not long_enough:
        print("\n--- drift ---")
        if dropped:
            worst = max((g[-1][0] - g[0][0]) for g in segments)
            print("  %d free-running stretch(es), longest %.1f min -- all below"
                  % (dropped, worst / 60.0))
            print("  the %.0f min needed to tell drift from frame quantisation."
                  % (args.min_segment / 60.0))
        else:
            print("  No free-running stretch of 3+ cycles between writes yet.")
        print("\n  Leave it running for an hour or so. Drift on these clocks is")
        print("  parts per million; measuring it needs a long lever arm, not")
        print("  more samples.")
        return 0

    segments = long_enough
    print("\n--- drift (free-running stretches only) ---")
    if dropped:
        print("  (ignoring %d stretch(es) shorter than %.0f min -- too short to"
              % (dropped, args.min_segment / 60.0))
        print("   separate drift from frame quantisation)")
    rates = []
    for i, seg in enumerate(segments, 1):
        xs = [t for t, _e in seg]
        ys = [e for _t, e in seg]
        fit = linfit(xs, ys)
        if not fit:
            continue
        slope, _inter, resid = fit
        rates.append(slope)
        if args.segments:
            print("  segment %d: %d cycles over %.1f min -> %+.3f s/hour "
                  "(%+0.1f ppm, resid %.3fs)"
                  % (i, len(seg), (xs[-1] - xs[0]) / 60.0, slope * 3600,
                     slope * 1e6, resid))
    if not rates:
        print("  nothing fittable yet")
        return 0

    rates.sort()
    med = rates[len(rates) // 2]
    longest = max(g[-1][0] - g[0][0] for g in segments)
    # Rough noise floor from the frame quantisation over the longest lever arm.
    floor = (1.0 / 24.0) / longest
    print("  %d segment(s), median drift %+.4f s/hour  (%+.1f ppm)"
          % (len(rates), med * 3600, med * 1e6))
    print("  measurement floor is about %.1f ppm over a %.1f min stretch"
          % (floor * 1e6, longest / 60.0))
    if abs(med) < floor:
        print("  -> the measured drift is below that floor: treat it as 'no")
        print("     drift detected yet' rather than as a value.")
        return 0

    if abs(med) > 1e-9:
        tol = 1.0
        hours = tol / abs(med * 3600)
        print("\n  At that rate the camera moves %.2fs per hour, so a %.1fs"
              % (abs(med * 3600), tol))
        print("  tolerance is reached about every %.1f hours." % hours)
        print("  Polling much faster than that only costs connections --")
        print("  though a short poll still catches a camera that was")
        print("  power-cycled or re-jammed out from under us.")

    # --- writes ---------------------------------------------------------
    writes = [r for r in cycles if str(r.get("action", "")).startswith("write")]
    if writes:
        print("\n--- writes ---")
        print("  %d write(s); %d verified"
              % (len(writes), sum(1 for r in writes if r.get("verified"))))
        lat = sorted(r["write_latency_s"] for r in writes
                     if "write_latency_s" in r)
        if lat:
            print("  BLE write latency: median %.0f ms  worst %.0f ms"
                  % (lat[len(lat) // 2] * 1000, lat[-1] * 1000))
        after = [r["error_after_s"] for r in writes if "error_after_s" in r]
        if after:
            a = sorted(after)
            print("  error just after a write: median %+.3fs  worst %+.3fs"
                  % (a[len(a) // 2], max(a, key=abs)))
        biases = [r["rtc_bias"] for r in writes if "rtc_bias" in r]
        if biases:
            seen = []
            for b in biases:
                if not seen or seen[-1] != b:
                    seen.append(b)
            print("  learned RTC bias: %s"
                  % " -> ".join("%+d" % b for b in seen))
            if len(seen) > 1 and seen[-1] == seen[-2]:
                print("    (settled)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
