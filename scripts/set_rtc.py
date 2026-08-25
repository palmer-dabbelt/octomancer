#!/usr/bin/env python3
"""
octomancer -- set the camera's Real Time Clock from this Mac.

The RTC (group 7, parameter 0) *is* writable over BLE, and on a body running
time-of-day timecode the timecode follows it immediately. So this is a working
way to put a known wall-clock time on the camera with no cable.

Two things get between "the write was accepted" and "the clock is right", and
this script handles both:

1. **Frame of reference.** The documentation specifies the RTC in UTC, and the
   camera adds its own Timezone parameter before displaying. So write UTC and
   let the camera convert -- writing local time gets the offset applied twice.
   `--local` exists to demonstrate that, not to be used.

2. **A fixed offset in how the camera applies the write.** On the 6K Pro tested
   here the clock lands ~75 s behind what was sent, repeatably, with zero
   spread across readings. That is a constant, not drift, so it cancels
   exactly -- but rather than hardcode a number measured on one body on one
   day, the default is to measure it and correct it in a second pass.

Either way the error is always reported against this Mac's own clock at the
end, because a GATT ack only proves the characteristic took the bytes.
"""

import argparse
import asyncio
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from bleak import BleakClient  # noqa: E402

from timecode_probe import (  # noqa: E402
    CH_OUTGOING_CTRL,
    CH_TIMECODE,
    describe_timecode,
    find_camera,
    hexdump,
    rtc_packet,
    tc_seconds,
    wrap_delta,
)


def secs_of_day(when):
    return when.hour * 3600 + when.minute * 60 + when.second


def hhmmss(secs):
    return "%02d:%02d:%02d" % (secs // 3600, secs % 3600 // 60, secs % 60)


async def write_and_measure(client, seen, args, bias, label):
    """Write the clock with `bias` applied, then measure what actually landed.

    Returns the settled error in seconds (camera minus this Mac), or None if
    the camera never produced a readable timecode.
    """
    # Sample the time immediately before writing: scanning and connecting can
    # take 30 seconds, and a clock sampled that long ago would bake the delay
    # straight into the result.
    if args.local:
        target = datetime.now() + timedelta(seconds=bias)
    else:
        target = datetime.now(timezone.utc) + timedelta(seconds=bias)
    pkt = rtc_packet(target, 0)

    before = seen[-1].split()[0] if seen else "(none)"
    print("\n%s: writing RTC = %s %s%s"
          % (label, target.strftime("%Y-%m-%d %H:%M:%S"),
             "local" if args.local else "UTC",
             "  (%+ds bias)" % bias if bias else ""))
    print("  bytes: %s" % hexdump(pkt))
    await client.write_gatt_char(CH_OUTGOING_CTRL, pkt, response=True)
    print("  timecode was %s -- watching whether it moves" % before)

    # Compare against this Mac's wall clock, never against the value we sent.
    # Measuring against our own biased target moves the goalposts with the shot
    # and could never reveal a bias.
    print("\n  %-6s %-12s %-12s %s" % ("t", "camera tc", "this Mac", "error"))
    errors = []
    for i in range(args.watch):
        await asyncio.sleep(1.0)
        tc = tc_seconds(seen[-1]) if seen else None
        if tc is None:
            print("  +%-5ds <no reading>" % (i + 1))
            continue
        truth = secs_of_day(datetime.now())
        diff = wrap_delta(tc - truth)
        errors.append(diff)
        print("  +%-5ds %-12s %-12s %+ds"
              % (i + 1, hhmmss(tc), hhmmss(truth), diff))

    if not errors:
        return None

    settled = errors[-3:]
    spread = max(settled) - min(settled)
    mean = int(round(sum(settled) / float(len(settled))))
    print("  -> settled at %+ds (spread %ds)" % (mean, spread))
    return mean


async def main_async(args):
    now_utc = datetime.now(timezone.utc)
    now_local = datetime.now()
    print("this Mac:  %s local  /  %s UTC"
          % (now_local.strftime("%Y-%m-%d %H:%M:%S"),
             now_utc.strftime("%H:%M:%S")))
    print("writing:   %s" % ("this Mac's LOCAL time (--local)" if args.local
                             else "UTC, letting the camera apply its Timezone"))

    if args.dry_run:
        print("packet:    %s" % hexdump(rtc_packet(
            now_local if args.local else now_utc, 0)))
        print("\n--dry-run: not touching Bluetooth.")
        return 0

    dev = await find_camera(args.name, args.scan_timeout)
    if dev is None:
        return 1

    print("\nconnecting to %s ..." % dev.address)
    async with BleakClient(dev) as client:
        print("connected.")

        seen = []

        def on_timecode(_char, data):
            seen.append(describe_timecode(data))

        await client.start_notify(CH_TIMECODE, on_timecode)
        # The first notification can take several seconds on a weak link, so
        # wait for one rather than assuming a fixed delay is long enough.
        for _ in range(int(args.first_tc_wait * 2)):
            if seen:
                break
            await asyncio.sleep(0.5)
        if not seen:
            print("\nno timecode notifications in %.0fs -- is the camera's"
                  " timecode running?" % args.first_tc_wait)
            print("This needs the camera set to Time of Day; in Record Run the")
            print("timecode doesn't follow the clock and there is nothing to see.")
            return 4

        bias = args.bias
        err = await write_and_measure(client, seen, args, bias, "pass 1")
        if err is None:
            print("\nnever got a clean reading.")
            return 4

        passes = 1
        while (args.calibrate and abs(err) > args.tolerance
               and abs(err) < 3600 and passes < args.max_passes):
            # Fold the measured error back into the bias. The offset is a
            # constant on this body, so one correction pass is normally enough.
            bias -= err
            passes += 1
            err = await write_and_measure(client, seen, args, bias,
                                          "pass %d" % passes)
            if err is None:
                print("\nlost the timecode stream mid-calibration.")
                return 4

        try:
            await client.stop_notify(CH_TIMECODE)
        except Exception:
            pass

        print("\n--- result ---")
        if abs(err) <= args.tolerance:
            print("The camera's timecode reads this Mac's time of day to within")
            print("%ds. The RTC write landed and the clock is right -- good"
                  % abs(err))
            print("enough to place a timeline against.")
            if bias != args.bias:
                print("\nIt took a %+ds correction to get there, measured on this"
                      % bias)
                print("body just now. Pass --bias %d to skip straight to it next"
                      % bias)
                print("time, or leave it to re-measure on every run.")
            return 0

        if abs(err) >= 3600:
            hours = int(round(err / 3600.0))
            print("Off by about %+d whole hours: the camera's timezone and this"
                  % hours)
            print("Mac's disagree. The camera adds its Timezone to the RTC before")
            print("displaying it, so this is a settings mismatch, not a failed write.")
            if args.local:
                print("\nThat is exactly what --local does -- the offset gets")
                print("applied twice. Re-run without it.")
            else:
                print("\nSet the camera's timezone to match this Mac, or pass")
                print("--bias %d to paper over it." % (bias - err))
            return 5

        print("Still %+ds out after %d pass(es). That is not a timezone and it"
              % (err, passes))
        print("didn't cancel, so it isn't behaving like the fixed offset seen on")
        print("this body -- worth another run to see whether it is drifting.")
        return 5


def main():
    p = argparse.ArgumentParser(
        description="Set a Blackmagic camera's Real Time Clock from this Mac",
        epilog="By default this writes UTC, measures the error against this "
               "Mac's clock, and corrects it in a second pass.")
    p.add_argument("--name", help="camera name or BLE address (default: first found)")
    p.add_argument("--bias", type=int, default=0, metavar="SECONDS",
                   help="seconds to add to the time written; the camera applies "
                        "the clock with a fixed offset (~75 s on the 6K Pro tested)")
    p.add_argument("--calibrate", action=argparse.BooleanOptionalAction, default=True,
                   help="measure the residual error and correct it in a further "
                        "pass (default: on)")
    p.add_argument("--max-passes", type=int, default=3, metavar="N",
                   help="most write/measure passes to make (default 3)")
    p.add_argument("--tolerance", type=int, default=1, metavar="SECONDS",
                   help="error to accept as correct (default 1)")
    p.add_argument("--local", action="store_true",
                   help="write this Mac's local time instead of UTC; wrong on "
                        "purpose, to show the timezone being applied twice")
    p.add_argument("--watch", type=int, default=8, metavar="SECONDS",
                   help="seconds to watch the timecode after each write (default 8)")
    p.add_argument("--first-tc-wait", type=float, default=15.0, metavar="SECONDS",
                   help="how long to wait for the first timecode notification "
                        "(default 15)")
    p.add_argument("--scan-timeout", type=float, default=20.0,
                   help="BLE scan duration (default 20)")
    p.add_argument("--dry-run", action="store_true",
                   help="show the packet and exit, no Bluetooth")
    args = p.parse_args()
    try:
        return asyncio.run(main_async(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
