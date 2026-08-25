#!/usr/bin/env python3
"""
octomancer -- keep a Blackmagic camera's clock on Tentacle time.

Runs until Ctrl-C. Each cycle it listens for Tentacle boxes, works out how far
this Mac is from them, then connects to the camera and corrects its clock if
that is both needed and allowed.

The camera is the expensive half of this: connecting takes seconds, and every
connection is a chance to disturb an operator mid-shot. So the Tentacle side is
sampled passively and often, the camera is touched rarely (once a minute by
default), and there are gates on every write.

Gates, all of which are logged with the reason:

  * **Recording.** Never touch the clock while transport mode (10.1) says
    Record. Jumping timecode mid-take corrupts the take.
  * **Already close enough.** Below --tolerance (default 1 s) leave it alone.
    The RTC is settable only in whole seconds, so chasing anything tighter
    than that just means writing constantly to no benefit.
  * **Externally jam-synced.** There is no protocol field that reports this, so
    it is detected behaviourally: if a write does not take, something else is
    driving the camera's timecode. After --max-failures consecutive misses the
    daemon backs off rather than fighting whatever that is.

Everything is logged to JSONL as well as the console, including cycles where
nothing happened, because the point is to build up a picture of how these
clocks drift so the tolerance and poll interval can be tuned from evidence.

    scripts/octomancer_sync.py --camera <addr>
    scripts/octomancer_sync.py --camera <addr> --dry-run --poll 20
"""

import argparse
import asyncio
import json
import math
import signal
import struct
import sys
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from bleak import BleakClient, BleakScanner  # noqa: E402

from tentacle_scan import FDAC, Box, wrap_delta  # noqa: E402
from timecode_probe import (  # noqa: E402
    CH_INCOMING_CTRL,
    CH_OUTGOING_CTRL,
    CH_TIMECODE,
    CMD_CHANGE_CONFIG,
    TYPE_INT32,
    decode_values,
    find_camera,
    parse_sdi_stream,
    rtc_packet,
)

# 10.1 [0]: 0 = Preview, 1 = Play, 2 = Record (documented, p103).
PARAM_TRANSPORT = (10, 1)
TRANSPORT_RECORD = 2
# 1.9 [0] is the sensor/recording frame rate on the bodies seen here.
PARAM_FRAMERATE = (1, 9)


def secs_of_day(when):
    return (when.hour * 3600 + when.minute * 60 + when.second
            + when.microsecond / 1e6)


def hhmmss(sod):
    sod %= 86400.0
    return "%02d:%02d:%06.3f" % (int(sod // 3600), int(sod % 3600 // 60),
                                 sod % 60)


def camera_tc(data):
    """Pull (h, m, s, f) out of a Timecode notification, or None.

    The camera wraps its timecode in a whole SDI message and packs the value
    as BCD -- the opposite of the Tentacles, which send plain binary.
    """
    for _dest, cmd, body in parse_sdi_stream(bytes(data)):
        if cmd == CMD_CHANGE_CONFIG and len(body) >= 8 and body[2] == TYPE_INT32:
            word = struct.unpack("<I", bytes(body[4:8]))[0]
            parts = []
            for shift in (24, 16, 8, 0):
                byte = (word >> shift) & 0xFF
                hi, lo = byte >> 4, byte & 0x0F
                if hi > 9 or lo > 9:
                    return None
                parts.append(hi * 10 + lo)
            return tuple(parts)
    return None


class Logger:
    """Console line plus a JSONL record, so cycles can be analysed later."""

    def __init__(self, path):
        self.path = path
        self.fh = open(path, "a") if path else None

    def say(self, msg):
        print("%s  %s" % (datetime.now().strftime("%H:%M:%S"), msg), flush=True)

    def record(self, obj):
        if not self.fh:
            return
        obj["wall"] = datetime.now().isoformat(timespec="milliseconds")
        self.fh.write(json.dumps(obj) + "\n")
        self.fh.flush()

    def close(self):
        if self.fh:
            self.fh.close()


async def listen_tentacles(seconds, log):
    """Passively collect Tentacle adverts and return {addr: Box}."""
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

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    try:
        await asyncio.sleep(seconds)
    finally:
        await scanner.stop()
    return {a: b for a, b in boxes.items() if b.deltas}


def tentacle_offset(boxes):
    """Seconds to add to this Mac's clock to get Tentacle time.

    Uses the median across boxes of each box's own median offset. Taking each
    box's median first stops one box with many adverts from outvoting the rest,
    and the outer median ignores a box that is jammed to something else.
    """
    per_box = []
    for b in boxes.values():
        ds = sorted(b.deltas)
        per_box.append(ds[len(ds) // 2])
    per_box.sort()
    return per_box[len(per_box) // 2], per_box


class CameraView:
    """What one connection to the camera told us."""

    def __init__(self):
        self.tc = None
        self.tc_at = None
        self.transport = None
        self.fps = None
        self.state = {}


async def subscribe_camera(client):
    """Start notifications once per connection and return a live view.

    Subscribing twice on one connection raises "notifications already started",
    so the view is created once and keeps updating itself; the verification
    pass after a write samples the same object rather than re-subscribing.
    """
    view = CameraView()

    def on_tc(_c, data):
        parts = camera_tc(data)
        if parts:
            view.tc = parts
            view.tc_at = time.monotonic()

    def on_incoming(_c, data):
        for _dest, cmd, body in parse_sdi_stream(bytes(data)):
            if cmd != CMD_CHANGE_CONFIG or len(body) < 4:
                continue
            view.state[(body[0], body[1])] = decode_values(
                body[2], bytes(body[4:]))

    await client.start_notify(CH_TIMECODE, on_tc)
    try:
        await client.start_notify(CH_INCOMING_CTRL, on_incoming)
    except Exception:
        pass
    return view


def refresh(view):
    """Pull the derived fields out of whatever has arrived so far."""
    view.transport = view.state.get(PARAM_TRANSPORT)
    rate = view.state.get(PARAM_FRAMERATE)
    view.fps = rate[0] if rate and rate[0] else None
    return view


async def await_camera(view, seconds):
    """Wait for the camera to have told us the time and its transport mode."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        await asyncio.sleep(0.2)
        if view.tc and PARAM_TRANSPORT in view.state:
            break
    return refresh(view)


def camera_sod(view, fps):
    if not view.tc:
        return None
    h, m, s, f = view.tc
    return h * 3600 + m * 60 + s + (f / float(fps) if fps else 0.0)


async def aligned_write(client, target_sod_at, offset, bias, args, log):
    """Write the RTC so the value lands on a second boundary.

    The RTC field is whole seconds, so writing "now" at an arbitrary moment
    throws away the fraction and lands up to a second slow. Instead pick the
    next whole second V in Tentacle time, wait until this Mac's clock reaches
    the instant V corresponds to, and send then. What is left is BLE latency,
    which is tens of milliseconds rather than half a second.
    """
    now = datetime.now(timezone.utc)
    target = secs_of_day(now) + offset + bias
    whole = math.floor(target) + 1
    while whole - target < args.lead:
        whole += 1
    wait = (whole - target) - args.lead
    if wait > 0:
        await asyncio.sleep(wait)

    send_at = datetime.now(timezone.utc)
    when = send_at + timedelta(seconds=offset + bias)
    # Force the value to the exact whole second we aimed at, rather than
    # re-truncating whatever the clock says at this instant.
    when = when.replace(microsecond=0)
    pkt = rtc_packet(when, 0)
    t0 = time.monotonic()
    await client.write_gatt_char(CH_OUTGOING_CTRL, pkt, response=True)
    latency = time.monotonic() - t0
    return when, latency, pkt


async def connect_camera(state, args, log):
    """Connect to the camera, scanning only when we have to.

    Once the address is known, CoreBluetooth can usually connect straight to
    it. Scanning for 20 s every cycle would otherwise dominate the poll
    interval and keep the radio busy for no reason.
    """
    addr = state.get("camera_addr")
    if addr:
        client = BleakClient(addr)
        try:
            await client.connect(timeout=args.connect_timeout)
            return client
        except Exception as exc:
            log.say("direct connect to %s failed (%s) -- rescanning"
                    % (addr[:8], type(exc).__name__))

    dev = await find_camera(args.camera, args.scan_timeout)
    if dev is None:
        return None
    state["camera_addr"] = dev.address
    client = BleakClient(dev)
    try:
        await client.connect(timeout=args.connect_timeout)
    except Exception as exc:
        log.say("connect failed: %s: %s" % (type(exc).__name__, exc))
        return None
    return client


async def cycle(state, args, log):
    """One pass: look at the Tentacles, then maybe correct the camera."""
    rec = {"event": "cycle"}

    boxes = await listen_tentacles(args.listen, log)
    if not boxes:
        log.say("no Tentacle boxes heard -- nothing to sync to")
        rec.update(action="skip:no-tentacle", tentacles=0)
        log.record(rec)
        return
    offset, per_box = tentacle_offset(boxes)
    spread = max(per_box) - min(per_box)
    rec["tentacles"] = len(boxes)
    rec["tentacle_offset_s"] = round(offset, 4)
    rec["tentacle_spread_s"] = round(spread, 4)
    rec["boxes"] = {b.name: {"offset_s": round(sorted(b.deltas)[len(b.deltas) // 2], 4),
                             "adverts": len(b.deltas), "rssi": b.rssi,
                             "resolution": b.resolution}
                    for b in boxes.values()}
    if spread > args.bench_spread:
        log.say("WARNING: Tentacle boxes disagree by %.3fs -- not all jammed"
                " to the same source" % spread)
        rec["bench_disagreement"] = True

    client = await connect_camera(state, args, log)
    if client is None:
        log.say("Tentacles at %+.3fs, but no camera found" % offset)
        rec.update(action="skip:no-camera")
        log.record(rec)
        return
    rec["camera_addr"] = state.get("camera_addr")

    try:
        view = await subscribe_camera(client)
        await await_camera(view, args.camera_wait)
        fps = view.fps or args.fps
        cam = camera_sod(view, fps)
        rec["camera_fps"] = fps
        rec["camera_transport"] = view.transport

        if cam is None:
            log.say("camera connected but sent no timecode")
            rec.update(action="skip:no-timecode")
            log.record(rec)
            return

        mac_now = secs_of_day(datetime.now())
        want = mac_now + offset
        error = wrap_delta(cam - want)
        rec["camera_tc"] = "%02d:%02d:%02d:%02d" % view.tc
        rec["camera_sod"] = round(cam, 4)
        rec["target_sod"] = round(want, 4)
        rec["error_s"] = round(error, 4)

        # Drift since the previous observation, which is the number worth
        # collecting: it says how fast the camera walks away between writes.
        last = state.get("last_obs")
        now_mono = time.monotonic()
        if last and not state.get("wrote_since_obs"):
            dt = now_mono - last[0]
            if dt > 5:
                drift = (error - last[1]) / dt
                rec["drift_s_per_s"] = round(drift, 8)
                rec["drift_ppm"] = round(drift * 1e6, 2)
        state["last_obs"] = (now_mono, error)
        state["wrote_since_obs"] = False

        recording = bool(view.transport) and view.transport[0] == TRANSPORT_RECORD
        rec["recording"] = recording

        drift_note = ("  drift %+.1f ppm" % rec["drift_ppm"]
                      if "drift_ppm" in rec else "")
        log.say("tentacles %+.3fs (%d boxes, spread %.3fs) | camera %s "
                "err %+.3fs%s"
                % (offset, len(boxes), spread, rec["camera_tc"], error,
                   drift_note))

        if recording:
            log.say("  gate: camera is RECORDING -- leaving the clock alone")
            rec["action"] = "skip:recording"
            log.record(rec)
            return

        if state.get("failures", 0) >= args.max_failures:
            log.say("  gate: %d writes in a row did not take -- assuming an"
                    " external timecode source owns this camera"
                    % state["failures"])
            rec["action"] = "skip:external-suspected"
            log.record(rec)
            return

        if abs(error) <= args.tolerance:
            log.say("  within %.2fs tolerance -- no change" % args.tolerance)
            rec["action"] = "skip:in-tolerance"
            log.record(rec)
            return

        if args.dry_run:
            log.say("  --dry-run: would correct %+.3fs" % -error)
            rec["action"] = "skip:dry-run"
            log.record(rec)
            return

        bias = state.get("rtc_bias", args.rtc_bias)
        when, latency, pkt = await aligned_write(
            client, want, offset, bias, args, log)
        rec["write_utc"] = when.strftime("%H:%M:%S")
        rec["write_latency_s"] = round(latency, 4)
        rec["rtc_bias"] = bias
        log.say("  wrote RTC %s UTC (bias %+ds, %.0fms latency)"
                % (rec["write_utc"], bias, latency * 1000))

        # A GATT ack proves nothing -- verify against the camera's own clock.
        # Same live view -- the notifications never stopped, so just let it
        # settle and read whatever the camera has reported since.
        view.tc = None
        await asyncio.sleep(args.verify_wait)
        await await_camera(view, args.camera_wait)
        cam2 = camera_sod(view, fps)
        if cam2 is None:
            log.say("  could not verify: no timecode after the write")
            rec["action"] = "write:unverified"
            state["failures"] = state.get("failures", 0) + 1
            log.record(rec)
            return

        err2 = wrap_delta(cam2 - (secs_of_day(datetime.now()) + offset))
        rec["error_after_s"] = round(err2, 4)
        rec["camera_tc_after"] = "%02d:%02d:%02d:%02d" % view.tc
        moved = abs(err2) < abs(error) - 0.25 or abs(err2) <= args.tolerance
        rec["verified"] = bool(moved)
        state["wrote_since_obs"] = True
        state["last_obs"] = (time.monotonic(), err2)

        if moved:
            state["failures"] = 0
            rec["action"] = "write:ok"
            log.say("  verified: error %+.3fs -> %+.3fs" % (error, err2))
            # A residual after a verified write means the bias is wrong. Fold
            # it back in, or the daemon writes every cycle forever and never
            # converges. This body's offset is not a constant of nature: it
            # was -75s before a power cycle and 0 after one, so the bias has
            # to be learned rather than configured.
            if abs(err2) > args.tolerance and args.adapt_bias:
                step = int(round(-err2))
                if abs(step) > args.max_bias_step:
                    step = args.max_bias_step * (1 if step > 0 else -1)
                    log.say("  (clamping bias correction to %+ds)" % step)
                state["rtc_bias"] = bias + step
                rec["rtc_bias_next"] = state["rtc_bias"]
                log.say("  learned: RTC bias %+ds -> %+ds, will retry next"
                        " cycle" % (bias, state["rtc_bias"]))
            elif abs(err2) > args.tolerance:
                log.say("  NOTE: still outside tolerance; --rtc-bias may need"
                        " to move by %+d" % round(-err2))
        else:
            state["failures"] = state.get("failures", 0) + 1
            rec["action"] = "write:no-effect"
            log.say("  WRITE DID NOT TAKE: error %+.3fs -> %+.3fs (%d in a row)"
                    % (error, err2, state["failures"]))
            log.say("  something else may be driving this camera's timecode")
        log.record(rec)
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass


async def main_async(args):
    log = Logger(args.log)
    state = {"failures": 0}
    stopping = asyncio.Event()

    def stop(*_a):
        stopping.set()

    try:
        asyncio.get_running_loop().add_signal_handler(signal.SIGINT, stop)
    except (NotImplementedError, RuntimeError):
        pass

    log.say("octomancer sync starting -- poll every %.0fs, tolerance %.2fs, "
            "%s" % (args.poll, args.tolerance,
                    "DRY RUN" if args.dry_run else "will write"))
    log.record({"event": "start", "poll_s": args.poll,
                "tolerance_s": args.tolerance, "rtc_bias_s": args.rtc_bias,
                "dry_run": bool(args.dry_run)})

    while not stopping.is_set():
        started = time.monotonic()
        try:
            await cycle(state, args, log)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            # A dropped connection or a camera that slept is routine here; log
            # it and carry on rather than dying overnight.
            log.say("cycle failed: %s: %s" % (type(exc).__name__, exc))
            log.record({"event": "error", "error": "%s: %s"
                        % (type(exc).__name__, exc)})
        if args.once:
            break
        remain = args.poll - (time.monotonic() - started)
        if remain > 0:
            try:
                await asyncio.wait_for(stopping.wait(), timeout=remain)
            except asyncio.TimeoutError:
                pass

    log.say("stopping")
    log.record({"event": "stop"})
    log.close()
    return 0


def main():
    p = argparse.ArgumentParser(
        description="Keep a Blackmagic camera's clock on Tentacle time")
    p.add_argument("--camera", help="camera BLE address or name hint")
    p.add_argument("--poll", type=float, default=60.0, metavar="SECONDS",
                   help="how often to touch the camera (default 60)")
    p.add_argument("--listen", type=float, default=8.0, metavar="SECONDS",
                   help="how long to listen for Tentacles each cycle (default 8)")
    p.add_argument("--tolerance", type=float, default=1.0, metavar="SECONDS",
                   help="leave the clock alone if it is this close (default 1)")
    p.add_argument("--rtc-bias", type=int, default=0, metavar="SECONDS",
                   help="starting guess for the camera's own RTC offset; it is"
                        " learned from the first write, so 0 is fine (default 0)")
    p.add_argument("--adapt-bias", action=argparse.BooleanOptionalAction,
                   default=True,
                   help="learn the RTC offset from what each write actually"
                        " lands on (default: on)")
    p.add_argument("--max-bias-step", type=int, default=120, metavar="SECONDS",
                   help="largest single correction to the learned bias")
    p.add_argument("--lead", type=float, default=0.05, metavar="SECONDS",
                   help="how early to send, to cover BLE latency (default 0.05)")
    p.add_argument("--verify-wait", type=float, default=3.0, metavar="SECONDS",
                   help="settle time before checking the write took (default 3)")
    p.add_argument("--camera-wait", type=float, default=6.0, metavar="SECONDS",
                   help="how long to wait for camera state (default 6)")
    p.add_argument("--scan-timeout", type=float, default=20.0,
                   help="BLE scan duration when looking for the camera")
    p.add_argument("--connect-timeout", type=float, default=15.0,
                   metavar="SECONDS", help="camera connect timeout (default 15)")
    p.add_argument("--max-failures", type=int, default=3, metavar="N",
                   help="failed writes before assuming an external timecode "
                        "source owns the camera (default 3)")
    p.add_argument("--bench-spread", type=float, default=0.5, metavar="SECONDS",
                   help="warn if the Tentacle boxes disagree by more than this")
    p.add_argument("--fps", type=int, default=24, metavar="N",
                   help="fallback frame rate if the camera doesn't report one")
    p.add_argument("--log", default="octomancer-sync.jsonl",
                   help="JSONL log path ('' to disable)")
    p.add_argument("--dry-run", action="store_true",
                   help="decide and log, but never write to the camera")
    p.add_argument("--once", action="store_true",
                   help="run a single cycle and exit")
    args = p.parse_args()
    try:
        return asyncio.run(main_async(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
