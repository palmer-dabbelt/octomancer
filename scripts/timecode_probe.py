#!/usr/bin/env python3
"""
octomancer -- timecode probe

Answers one question: can we set a Blackmagic camera's clock over Bluetooth LE?

The BLE "Outgoing Camera Control" characteristic is a plain tunnel for the
Blackmagic SDI Camera Control Protocol, so we set the camera's Real Time Clock
(group 7, parameter 0) by writing an SDI protocol packet to it.

Reference: doc/BlackmagicCameraControl.pdf (August 2025)
  * Blackmagic SDI Camera Control Protocol .... p96-105
  * Example Protocol Packets ................... p105
  * Blackmagic Bluetooth Camera Control ........ p109-110
"""

import argparse
import asyncio
import struct
import sys
import time
from datetime import datetime, timedelta, timezone

from bleak import BleakClient, BleakScanner

# ---------------------------------------------------------------- UUIDs (p109)

SVC_CAMERA = "291d567a-6d75-11e6-8b77-86f30ca893d3"

CH_OUTGOING_CTRL = "5dd3465f-1aee-4299-8493-d2eca2f8e1bb"  # write   (encrypted)
CH_INCOMING_CTRL = "b864e140-76a0-416a-bf30-5876504537d9"  # notify  (encrypted)
CH_TIMECODE      = "6d8f2110-86f1-41bf-9afb-451d87e976c8"  # notify  (encrypted)
CH_CAMERA_STATUS = "7fe8691d-95dc-4fc5-8abd-ca74339b51b9"  # r/w/notify (encrypted)
CH_DEVICE_NAME   = "ffac0c52-c9fb-41a0-b063-cc76282eb89c"
CH_PROTOCOL_VER  = "8f1fd018-b508-456f-8f82-3d392bee2706"

CH_MANUFACTURER  = "00002a29-0000-1000-8000-00805f9b34fb"  # Device Info Service
CH_MODEL         = "00002a24-0000-1000-8000-00805f9b34fb"

STATUS_FLAGS = [
    (0x01, "Power On"),
    (0x02, "Connected"),
    (0x04, "Paired"),
    (0x08, "Versions Verified"),
    (0x10, "Initial Payload Received"),
    (0x20, "Camera Ready"),
]

# ------------------------------------------------------- SDI protocol encoding

CMD_CHANGE_CONFIG = 0
TYPE_INT32        = 3
OP_ASSIGN         = 0
GROUP_CONFIG      = 7
PARAM_RTC         = 0
PARAM_TIMEZONE    = 2
BROADCAST         = 255

# Group 9 is NOT in the published parameter table -- the doc jumps from 8
# (Colour Correction) to 10 (Media). But a Pocket 6K Pro reports its running
# timecode as 9.4 int32 (BCD HHMMSSFF) on the Timecode characteristic, so it's
# the obvious candidate for setting timecode directly rather than via the RTC.
GROUP_STATUS      = 9
PARAM_TIMECODE    = 4


def build_packet(category, parameter, data_type, operation, payload, dest=BROADCAST):
    """Wrap a change-configuration command in the 4-byte SDI header (p96-97).

    The length field covers category/parameter/type/operation plus payload --
    it excludes the header and any trailing 32-bit alignment padding.
    """
    body = bytes([category, parameter, data_type, operation]) + payload
    pkt = bytes([dest, len(body), CMD_CHANGE_CONFIG, 0]) + body
    return pkt + b"\x00" * (-len(pkt) % 4)


def bcd2(n):
    """Two decimal digits -> one packed BCD byte."""
    return ((n // 10) << 4) | (n % 10)


def encode_time(h, m, s, f):
    """BCD HHMMSSFF (p102)."""
    return (bcd2(h) << 24) | (bcd2(m) << 16) | (bcd2(s) << 8) | bcd2(f)


def encode_date(y, mo, d):
    """BCD YYYYMMDD (p102)."""
    return (bcd2(y // 100) << 24) | (bcd2(y % 100) << 16) | (bcd2(mo) << 8) | bcd2(d)


def rtc_packet(when, frames, dest=BROADCAST):
    """Real Time Clock: group 7, parameter 0, int32[2] = {time, date}.

    `when` must already be UTC -- the doc specifies the RTC is held in UTC and
    the camera applies its own Timezone parameter for display.
    """
    t = encode_time(when.hour, when.minute, when.second, frames)
    d = encode_date(when.year, when.month, when.day)
    payload = struct.pack("<II", t, d)  # little-endian, per the p105 examples
    return build_packet(GROUP_CONFIG, PARAM_RTC, TYPE_INT32, OP_ASSIGN, payload, dest)


def timecode_packet(h, m, s, f, dest=BROADCAST):
    """Undocumented: group 9 parameter 4, int32 BCD HHMMSSFF.

    This is the same shape the camera uses when *reporting* its timecode, so
    assigning to it is the natural guess for setting the timecode outright.
    """
    payload = struct.pack("<I", encode_time(h, m, s, f))
    return build_packet(GROUP_STATUS, PARAM_TIMECODE, TYPE_INT32, OP_ASSIGN, payload, dest)


def timezone_packet(minutes, dest=BROADCAST):
    """Timezone: group 7, parameter 2, int32 minutes offset from UTC."""
    payload = struct.pack("<i", minutes)
    return build_packet(GROUP_CONFIG, PARAM_TIMEZONE, TYPE_INT32, OP_ASSIGN, payload, dest)


def decode_bcd_timecode(word):
    """0x09125310 -> '09:12:53:10'. Returns None if any nibble isn't a digit."""
    parts = []
    for shift in (24, 16, 8, 0):
        byte = (word >> shift) & 0xFF
        hi, lo = byte >> 4, byte & 0x0F
        if hi > 9 or lo > 9:
            return None
        parts.append(hi * 10 + lo)
    return "%02d:%02d:%02d:%02d" % tuple(parts)


def describe_timecode(data):
    """Decode the Timecode characteristic.

    The doc calls this "a 32-bit BCD number", but a Pocket 6K Pro actually
    sends a whole 12-byte SDI message wrapping it:

        ff 08 00 ff | 09 04 03 00 | 18 14 55 17
        dest/len/cmd  grp 9 par 4   BCD little-endian -> 17:55:14:18
                      int32/assign

    so parse the wrapper and fall back to a raw scan if it looks different.
    """
    raw = bytes(data)
    for _dest, cmd, body in parse_sdi_stream(raw):
        if cmd == CMD_CHANGE_CONFIG and len(body) >= 8 and body[2] == TYPE_INT32:
            word = struct.unpack("<I", bytes(body[4:8]))[0]
            tc = decode_bcd_timecode(word)
            if tc:
                return "%s   (%d.%d)" % (tc, body[0], body[1])

    # Unrecognised shape -- show everything and let a human look at it.
    out = ["raw=%s(%dB)" % (raw.hex(), len(raw))]
    for off in range(0, max(1, len(raw) - 3)):
        word = raw[off:off + 4]
        if len(word) < 4:
            break
        le = decode_bcd_timecode(int.from_bytes(word, "little"))
        be = decode_bcd_timecode(int.from_bytes(word, "big"))
        if le or be:
            out.append("[%d:]LE=%s BE=%s" % (off, le or "--", be or "--"))
    return "  ".join(out)


def describe_status(value):
    flags = [name for bit, name in STATUS_FLAGS if value & bit]
    return "0x%02x [%s]" % (value, ", ".join(flags) if flags else "none")


def hexdump(pkt):
    return " ".join("%02x" % b for b in pkt)


# ------------------------------------------------- decoding what comes back

TYPE_NAMES = {0: "void/bool", 1: "int8", 2: "int16", 3: "int32",
              4: "int64", 5: "utf8", 128: "fixed16"}

# Only the handful we care about; everything else prints as "group.param".
PARAM_NAMES = {
    (7, 0): "Real Time Clock",
    (7, 1): "System language",
    (7, 2): "Timezone",
    (7, 3): "Location",
    (9, 0): "Battery",
    (9, 4): "Timecode",
    (0, 0): "Focus",
    (1, 0): "Video mode",
    (1, 5): "Exposure (us)",
    (4, 7): "Timecode Source",
    (10, 0): "Codec",
    (10, 1): "Transport mode",
}


def decode_values(dtype, data):
    try:
        if dtype == 0:
            return list(data)
        if dtype == 1:
            return list(struct.unpack("<%db" % len(data), data))
        if dtype == 2:
            n = len(data) // 2
            return list(struct.unpack("<%dh" % n, data[:n * 2]))
        if dtype == 3:
            n = len(data) // 4
            return list(struct.unpack("<%di" % n, data[:n * 4]))
        if dtype == 4:
            n = len(data) // 8
            return list(struct.unpack("<%dq" % n, data[:n * 8]))
        if dtype == 5:
            return data.decode("utf-8", "replace")
        if dtype == 128:
            n = len(data) // 2
            return [v / 2048.0 for v in struct.unpack("<%dh" % n, data[:n * 2])]
    except Exception:
        pass
    return list(data)


def parse_sdi_stream(buf):
    """Split a run of concatenated SDI messages (p96: up to 32 per packet)."""
    msgs = []
    i = 0
    while i + 4 <= len(buf):
        dest, length, cmd = buf[i], buf[i + 1], buf[i + 2]
        body = buf[i + 4:i + 4 + length]
        if len(body) < length:
            break
        msgs.append((dest, cmd, body))
        step = 4 + length
        step += -step % 4          # skip the implicit alignment padding
        if step <= 0:
            break
        i += step
    return msgs


def describe_sdi(buf):
    """Render incoming camera-control traffic as one line per message."""
    lines = []
    for dest, cmd, body in parse_sdi_stream(buf):
        if cmd != CMD_CHANGE_CONFIG or len(body) < 4:
            lines.append("dest=%d cmd=%d raw=%s" % (dest, cmd, bytes(body).hex()))
            continue
        cat, param, dtype, op = body[0], body[1], body[2], body[3]
        data = bytes(body[4:])
        name = PARAM_NAMES.get((cat, param), "%d.%d" % (cat, param))
        vals = decode_values(dtype, data)

        if (cat, param) == (7, 0) and dtype == 3 and len(data) >= 8:
            t, d = struct.unpack("<II", data[:8])
            tc = decode_bcd_timecode(t)
            lines.append("RTC  time=%s  date=%08x  (raw %s)"
                         % (tc or ("?" + "%08x" % t), d, data.hex()))
        else:
            lines.append("%-18s [%s] op=%d %s"
                         % (name, TYPE_NAMES.get(dtype, dtype), op, vals))
    return lines


# ---------------------------------------------------------------- BLE plumbing

FDAC_UUID = "0000fdac-0000-1000-8000-00805f9b34fb"


async def find_camera(name_hint=None, timeout=8.0, show_all=False):
    print("scanning %.0fs for Blackmagic cameras..." % timeout)
    found = await BleakScanner.discover(timeout=timeout, return_adv=True)

    cameras = []
    for dev, adv in found.values():
        uuids = [u.lower() for u in (adv.service_uuids or [])]
        label = adv.local_name or dev.name or ""
        # A Tentacle broadcasts its timecode in FDAC service data, which is
        # proof of what it is. Boxes get named after the camera they ride on,
        # so the bench here includes one called "BMPCC" -- and with the real
        # camera switched off, the name guess below happily picked it, then
        # spent every cycle connecting and failing to find the control
        # characteristic. Positive identification beats a name every time.
        is_tentacle = (FDAC_UUID in uuids
                       or any(str(k).lower() == FDAC_UUID
                              for k in (adv.service_data or {})))
        # A service-UUID match is proof; a name match is only a guess.
        if SVC_CAMERA in uuids:
            cameras.append((dev, adv, label, "service uuid"))
        elif is_tentacle:
            continue
        elif any(k in label.lower()
                 for k in ("blackmagic", "bmpcc", "bmcc", "bmd", "ursa", "pocket",
                           "pyxis", "cinema camera", "studio camera")):
            cameras.append((dev, adv, label, "name guess"))

    # Trust the UUID matches ahead of the name guesses.
    cameras.sort(key=lambda c: (c[3] != "service uuid", -(c[1].rssi or -999)))

    if show_all:
        print("  -- every LE device seen (%d) --" % len(found))
        for dev, adv in sorted(found.values(), key=lambda p: -(p[1].rssi or -999)):
            label = adv.local_name or dev.name or "(no name)"
            print("     %-38s %-28s rssi=%s" % (dev.address, label[:28], adv.rssi))
        print("  -- end --")

    for dev, adv, label, why in cameras:
        print("  %-38s %-22s rssi=%-5s (%s)"
              % (dev.address, label or "(no name)", adv.rssi, why))

    if not cameras:
        print("  no Blackmagic cameras found.")
        if not found:
            print("  * no LE devices at all -- Bluetooth may still be blocked for this process")
        else:
            print("  * saw %d other LE devices, so the radio and permissions are fine" % len(found))
        print("  * enable Bluetooth in the camera's setup menu")
        print("  * a camera already connected to another app won't advertise")
        print("  * an already-bonded camera may need to be un-paired from macOS first")
        return None

    if name_hint:
        for dev, adv, label, why in cameras:
            if name_hint.lower() in (label or "").lower() or name_hint.lower() == dev.address.lower():
                return dev
        # Allow targeting anything seen, not just things that looked like cameras.
        for dev, adv in found.values():
            label = adv.local_name or dev.name or ""
            if name_hint.lower() in label.lower() or name_hint.lower() == dev.address.lower():
                print("  (%s didn't look like a camera, connecting anyway)" % (label or dev.address))
                return dev
        print("no device matched %r" % name_hint)
        return None
    return cameras[0][0]


async def read_text(client, uuid, label):
    try:
        val = await client.read_gatt_char(uuid)
        text = val.decode("utf-8", "replace").strip()
        print("  %-18s %s" % (label + ":", text))
        return text
    except Exception as exc:
        print("  %-18s <unavailable: %s>" % (label + ":", exc))
        return None


def find_char(client, uuid):
    """Return the characteristic object for uuid, or None."""
    for svc in client.services:
        for ch in svc.characteristics:
            if ch.uuid.lower() == uuid.lower():
                return ch
    return None


def dump_services(client):
    print("\nGATT services advertised by this device:")
    for svc in client.services:
        marker = "  <-- Blackmagic Camera Service" if svc.uuid.lower() == SVC_CAMERA else ""
        print("  %s  %s%s" % (svc.uuid, svc.description, marker))
        for ch in svc.characteristics:
            print("      %s  %-28s %s"
                  % (ch.uuid, ",".join(ch.properties), ch.description))


async def timecode_char_test(client, seen, when, args):
    """Can we write timecode straight to the Timecode characteristic?

    The doc (p109) only ever describes this characteristic as a source of
    notifications, but it also describes the notification payload as a bare
    32-bit BCD number when the camera actually sends a 12-byte wrapped SDI
    message. So the doc is not a reliable guide to what this characteristic
    does, and a write is worth trying on its own terms -- it is a different
    pipe from the SDI tunnel.

    We try three payload shapes, because there is no documentation telling us
    which (if any) is right:

      1. bare 32-bit BCD, little-endian   -- matches how the camera REPORTS it
      2. bare 32-bit BCD, big-endian      -- matches how the doc WRITES it
                                             (09:12:53:10 = 0x09125310)
      3. the full 12-byte wrapped SDI message, byte-for-byte in the shape the
         camera itself emits (reserved = 0xff, group 9 param 4)
    """
    ch = find_char(client, CH_TIMECODE)
    if ch is None:
        print("\nno Timecode characteristic on this device -- cannot test")
        return 1

    props = ",".join(ch.properties)
    print("\nTimecode characteristic properties: %s" % props)
    writable = any(p in ch.properties for p in
                   ("write", "write-without-response", "authenticated-signed-writes"))
    if not writable:
        print("  the camera does NOT advertise any write property here.")
        print("  Writing anyway -- an undeclared property is not proof of")
        print("  refusal, and we would rather see the error than assume it.")

    h, m, s, f = when.hour, when.minute, when.second, args.frames
    word = encode_time(h, m, s, f)
    target = "%02d:%02d:%02d:%02d" % (h, m, s, f)

    variants = [
        ("bare BCD little-endian", struct.pack("<I", word)),
        ("bare BCD big-endian",    struct.pack(">I", word)),
        ("wrapped SDI message (as the camera emits it)",
         bytes([BROADCAST, 8, CMD_CHANGE_CONFIG, 0xff,
                GROUP_STATUS, PARAM_TIMECODE, TYPE_INT32, OP_ASSIGN])
         + struct.pack("<I", word)),
    ]

    print("\ntrying to set timecode to %s via the Timecode characteristic" % target)
    results = []
    for label, payload in variants:
        before = seen[-1] if seen else "<none yet>"
        print("\n  -- %s --" % label)
        print("     bytes: %s" % hexdump(payload))
        acked, err = True, None
        try:
            await client.write_gatt_char(CH_TIMECODE, payload, response=True)
        except Exception as exc:
            acked, err = False, exc
            print("     write REJECTED: %s: %s" % (type(exc).__name__, exc))
        if acked:
            print("     write acked (GATT only -- says nothing about effect)")
        await asyncio.sleep(args.settle)
        after = seen[-1] if seen else "<none yet>"
        print("     timecode before: %s" % before)
        print("     timecode after:  %s" % after)
        results.append((label, acked, err, after))

    print("\n---- Timecode characteristic write summary ----")
    print("target was %s" % target)
    for label, acked, err, after in results:
        state = "acked" if acked else "rejected (%s)" % type(err).__name__
        print("  %-46s %-28s last TC %s" % (label, state, after))
    print("\nA camera that took any of these would show a timecode jumping to")
    print("roughly %s. Free-running time-of-day means it did not." % target)
    return 0


def tc_seconds(line):
    """'23:34:47:10   (9.4)' -> seconds since midnight, or None."""
    head = line.split()[0] if line else ""
    parts = head.split(":")
    if len(parts) != 4:
        return None
    try:
        h, m, s, _f = (int(p) for p in parts)
    except ValueError:
        return None
    return h * 3600 + m * 60 + s


def wrap_delta(seconds):
    """Shortest signed distance around a 24-hour clock."""
    return (seconds + 43200) % 86400 - 43200


def fmt_hms(seconds):
    """-3723 -> '-1h02m03s'."""
    sign = "-" if seconds < 0 else "+"
    h, rem = divmod(abs(int(seconds)), 3600)
    m, s = divmod(rem, 60)
    return "%s%dh%02dm%02ds" % (sign, h, m, s)


async def rtc_test(client, seen, args):
    """Does the camera obey a Real Time Clock (7.0) write?

    Our first attempt at this wrote the *correct* current UTC time, which is
    not a test at all: a camera whose clock is already right looks exactly the
    same whether it honoured the write or dropped it on the floor. So write a
    clock that is deliberately wrong by a known amount, watch for a jump of
    that exact size, then put the clock back.

    This only shows anything if the camera's timecode is set to Time of Day.
    In Record Run the timecode isn't derived from the clock and the test is
    blind -- it would report "ignored" even for a write that landed.
    """
    off = args.rtc_offset
    print("\n--- RTC test: write a deliberately wrong clock, watch for the jump ---")
    print("offset under test: %s (%+d s)" % (fmt_hms(off), off))

    print("\nletting timecode settle %.1fs ..." % args.settle)
    await asyncio.sleep(args.settle)

    before = seen[-1] if seen else None
    b = tc_seconds(before) if before else None
    t0 = time.monotonic()
    utc0 = datetime.now(timezone.utc)
    if b is None:
        print("no usable timecode notifications -- nothing to measure a jump")
        print("against. Check that the camera's timecode is running.")
        return 4
    print("timecode before: %s" % before)

    # Worth knowing before we touch anything: the camera reports time-of-day
    # timecode in local time, while the RTC parameter is specified in UTC.
    utc_secs = utc0.hour * 3600 + utc0.minute * 60 + utc0.second
    skew = wrap_delta(b - utc_secs)
    print("camera clock sits %s from UTC (%s UTC now)"
          % (fmt_hms(skew), utc0.strftime("%H:%M:%S")))

    target = utc0 + timedelta(seconds=off)
    pkt = rtc_packet(target, args.frames, args.dest)
    print("\nWRITING Real Time Clock (7.0) = %s UTC"
          % target.strftime("%Y-%m-%d %H:%M:%S"))
    print("  bytes: %s" % hexdump(pkt))
    await client.write_gatt_char(CH_OUTGOING_CTRL, pkt, response=True)
    print("  write returned without error (GATT ack only -- this does NOT")
    print("  mean the camera acted on it)")

    print("\nwatching %.1fs for the timecode to move ..." % args.settle)
    await asyncio.sleep(args.settle)

    after = seen[-1]
    elapsed = time.monotonic() - t0
    a = tc_seconds(after)
    print("timecode after:  %s" % after)

    jump = None
    if a is not None:
        # Subtract the wall-clock time that genuinely passed, so what's left is
        # the discontinuity the write caused, if any.
        jump = wrap_delta(a - b - int(round(elapsed)))
        print("\nclock moved %s more than the %.1fs that actually elapsed"
              % (fmt_hms(jump), elapsed))

    worked = jump is not None and abs(jump - off) <= args.tolerance

    print("\nrestoring the clock to true UTC ...")
    restore = rtc_packet(datetime.now(timezone.utc), args.frames, args.dest)
    print("  bytes: %s" % hexdump(restore))
    try:
        await client.write_gatt_char(CH_OUTGOING_CTRL, restore, response=True)
        await asyncio.sleep(args.settle)
        print("timecode now:    %s" % (seen[-1] if seen else "(none)"))
    except Exception as exc:
        print("  restore FAILED: %s" % exc)
        print("  if the write worked, the camera's clock is still %s off."
              % fmt_hms(off))

    print("\n--- RTC test result ---")
    if worked:
        print("The camera FOLLOWED the write. Timecode jumped %s, within %ds of"
              % (fmt_hms(jump), args.tolerance))
        print("the %s we asked for. Setting the RTC over BLE works, and that's"
              % fmt_hms(off))
        print("enough to place a timeline against wall-clock time.")
    elif jump is not None and abs(jump) <= args.tolerance:
        print("The camera IGNORED the write. Timecode kept free-running with no")
        print("discontinuity at all, so the RTC parameter looks unimplemented")
        print("here -- the GATT ack was just the characteristic taking bytes.")
        print("\nOne thing to rule out first: if the camera is in Record Run")
        print("rather than Time of Day, its timecode wouldn't follow the clock")
        print("even for a write that landed. Check that setting on the camera.")
    else:
        print("The clock moved %s, which is neither the %s we asked for nor"
              % (fmt_hms(jump), fmt_hms(off)))
        print("standing still. Something responded -- worth a second run.")
    return 0 if worked else 5


async def control_test(client, latest, args):
    """Does this camera act on ANY camera-control write?

    Without this, a timecode write that does nothing is ambiguous: it could
    mean "group 7 is unsupported" or "our packets are malformed/misaddressed".
    White balance (1.2) is reported back in the camera's own telemetry, so we
    can set it, watch it change, and put it straight back.
    """
    print("\n--- control test: will this camera act on any write at all? ---")

    orig = latest.get((1, 2))
    if not orig:
        print("waiting for the camera to report white balance (1.2) ...")
        await asyncio.sleep(4)
        orig = latest.get((1, 2))
    if not orig or len(orig) < 1:
        print("never saw 1.2 reported -- cannot run the control test")
        return 4

    tint = orig[1] if len(orig) > 1 else 0
    target = 5600 if orig[0] != 5600 else 3200
    print("white balance is currently %s; setting it to %d" % (orig, target))

    def wb(kelvin):
        return build_packet(1, 2, 2, OP_ASSIGN,
                            struct.pack("<hh", kelvin, tint), args.dest)

    packet = wb(target)
    print("  bytes: %s" % hexdump(packet))
    await client.write_gatt_char(CH_OUTGOING_CTRL, packet, response=True)
    await asyncio.sleep(4)

    now = latest.get((1, 2))
    print("white balance now reads %s" % (now,))
    worked = bool(now) and now[0] == target

    print("\nrestoring white balance to %d" % orig[0])
    await client.write_gatt_char(CH_OUTGOING_CTRL, wb(orig[0]), response=True)
    await asyncio.sleep(3)
    print("white balance restored to %s" % (latest.get((1, 2)),))

    print("\n--- control test result ---")
    if worked:
        print("The camera DID obey a control write. Our framing, addressing and")
        print("the BLE path are all correct -- so the timecode parameters are")
        print("specifically not implemented on this body.")
    else:
        print("The camera ignored this write too. That points at framing or")
        print("addressing rather than a missing timecode feature -- try --dest 1.")
    return 0 if worked else 5


async def run(args):
    when = datetime.now(timezone.utc)
    if args.time:
        try:
            h, m, s = (int(x) for x in args.time.split(":"))
        except ValueError:
            print("--time wants HH:MM:SS (UTC)", file=sys.stderr)
            return 2
        when = when.replace(hour=h, minute=m, second=s)

    pkt = rtc_packet(when, args.frames, args.dest)
    print("Real Time Clock packet for %s UTC frame %d"
          % (when.strftime("%Y-%m-%d %H:%M:%S"), args.frames))
    print("  bytes: %s" % hexdump(pkt))
    print("  dest=%d len=%d cmd=0 | group=7 param=0 type=int32 op=assign" % (args.dest, pkt[1]))
    print("  time=%s  date=%08x" % (
        decode_bcd_timecode(encode_time(when.hour, when.minute, when.second, args.frames)),
        encode_date(when.year, when.month, when.day),
    ))

    if args.dry_run:
        print("\n--dry-run: not touching Bluetooth.")
        return 0

    dev = await find_camera(args.name, args.scan_timeout, args.all)
    if dev is None:
        return 1
    if args.scan_only:
        return 0

    print("\nconnecting to %s ..." % dev.address)
    async with BleakClient(dev) as client:
        print("connected.")
        maker = await read_text(client, CH_MANUFACTURER, "manufacturer")
        await read_text(client, CH_MODEL, "model")

        if args.dump_services:
            dump_services(client)

        # Bail out clearly rather than throwing a characteristic-not-found
        # traceback four steps later.
        have = {ch.uuid.lower()
                for svc in client.services for ch in svc.characteristics}
        if CH_OUTGOING_CTRL not in have:
            print("\nThis device has no Blackmagic camera-control characteristic.")
            if maker and "blackmagic" not in maker.lower():
                print("Its manufacturer is %r -- this isn't a Blackmagic camera." % maker)
                if "tentacle" in maker.lower():
                    print("It's a Tentacle Sync box. They're often named after the")
                    print("camera they're attached to, which makes them easy to")
                    print("mistake for the camera itself.")
            print("\nRe-run with --dump-services to see what it does expose,")
            print("or with --all to list every device in range.")
            return 3

        try:
            ver = await client.read_gatt_char(CH_PROTOCOL_VER)
            print("  %-18s %s" % ("protocol ver:", ver.hex()))
        except Exception as exc:
            print("  %-18s <unavailable: %s>" % ("protocol ver:", exc))

        # Any write to an encrypted characteristic kicks off bonding; the camera
        # then shows a 6-digit PIN that macOS will prompt for (p110).
        print("\nwriting Camera Status power-on (0x01) -- triggers bonding if unpaired")
        print("  if the camera shows a 6-digit PIN, type it into the macOS prompt")
        try:
            await client.write_gatt_char(CH_CAMERA_STATUS, b"\x01", response=True)
            status = await client.read_gatt_char(CH_CAMERA_STATUS)
            print("  camera status: %s" % describe_status(status[0]))
        except Exception as exc:
            print("  power-on/status failed: %s" % exc)

        seen = []
        last_tc = [None]

        def on_timecode(_char, data):
            line = describe_timecode(data)
            seen.append(line)
            # These arrive many times a second and are usually identical;
            # only print when the value actually changes.
            if line != last_tc[0]:
                last_tc[0] = line
                print("  TC  %s" % line)

        incoming_seen = set()
        latest = {}

        def on_incoming(_char, data):
            buf = bytes(data)
            for (dest, cmd, body), line in zip(parse_sdi_stream(buf), describe_sdi(buf)):
                key = (body[0], body[1]) if len(body) >= 2 else (dest, cmd)
                if len(body) >= 4:
                    latest[key] = decode_values(body[2], bytes(body[4:]))
                # The camera re-sends its whole state constantly. Show each
                # parameter once, then only the ones this probe is about.
                interesting = key[0] == GROUP_CONFIG or key == (GROUP_STATUS, PARAM_TIMECODE)
                if args.verbose or interesting or key not in incoming_seen:
                    incoming_seen.add(key)
                    print("  <-  %s" % line)

        def on_status(_char, data):
            if data:
                print("  ST  %s" % describe_status(data[0]))

        try:
            await client.start_notify(CH_TIMECODE, on_timecode)
            print("\nsubscribed to timecode notifications")
        except Exception as exc:
            print("\ncould not subscribe to timecode: %s" % exc)

        try:
            await client.start_notify(CH_INCOMING_CTRL, on_incoming)
            print("subscribed to incoming camera control")
        except Exception as exc:
            print("could not subscribe to incoming control: %s" % exc)

        try:
            await client.start_notify(CH_CAMERA_STATUS, on_status)
            print("subscribed to camera status")
        except Exception as exc:
            print("could not subscribe to camera status: %s" % exc)

        if args.watch:
            print("\n--watch: listening %.1fs, not writing anything ..." % args.watch)
            await asyncio.sleep(args.watch)
            print("\nsaw %d timecode notifications" % len(seen))
            if seen:
                print("last: %s" % seen[-1])
            return 0

        if args.tc_char_test:
            print("\nletting timecode settle %.1fs before writing ..." % args.settle)
            await asyncio.sleep(args.settle)
            return await timecode_char_test(client, seen, when, args)

        if args.rtc_test:
            return await rtc_test(client, seen, args)

        if args.control_test:
            return await control_test(client, latest, args)

        print("listening %.1fs BEFORE the write ..." % args.settle)
        await asyncio.sleep(args.settle)
        before = len(seen)

        if args.tz_minutes is not None:
            tzpkt = timezone_packet(args.tz_minutes, args.dest)
            print("\nwriting Timezone = %d minutes from UTC" % args.tz_minutes)
            print("  bytes: %s" % hexdump(tzpkt))
            await client.write_gatt_char(CH_OUTGOING_CTRL, tzpkt, response=True)

        writes = []
        if args.method in ("rtc", "both"):
            writes.append(("Real Time Clock (7.0)", pkt))
        if args.method in ("timecode", "both"):
            writes.append(("Timecode (9.4, undocumented)",
                           timecode_packet(when.hour, when.minute, when.second,
                                           args.frames, args.dest)))

        for label, packet in writes:
            print("\nWRITING %s -> Outgoing Camera Control" % label)
            print("  bytes: %s" % hexdump(packet))
            await client.write_gatt_char(CH_OUTGOING_CTRL, packet, response=True)
            print("  write returned without error (GATT ack only -- this does")
            print("  NOT mean the camera acted on it)")

        print("\nlistening %.1fs AFTER the write ..." % args.settle)
        await asyncio.sleep(args.settle)

        try:
            await client.stop_notify(CH_TIMECODE)
        except Exception:
            pass

        print("\n--- result ---")
        print("timecode notifications: %d before, %d after" % (before, len(seen) - before))
        if not seen:
            print("no timecode notifications at all -- the write may still have")
            print("landed; check the camera's clock in its setup menu.")
        else:
            tc_before = seen[before - 1] if before else "(none)"
            tc_after = seen[-1]
            target = "%02d:%02d:%02d" % (when.hour, when.minute, when.second)
            print("  wanted (UTC):  %s" % target)
            print("  last before:   %s" % tc_before)
            print("  last after:    %s" % tc_after)

            hhmm = target[:5]
            if tc_after.startswith(hhmm):
                print("\n=> the timecode now matches what we wrote. It worked.")
            elif tc_after[:5] != tc_before[:5]:
                print("\n=> the timecode jumped, but not to the value we asked for.")
                print("   Worth checking the camera's timezone offset.")
            else:
                print("\n=> the timecode did NOT change. The write was accepted at")
                print("   the GATT layer but the camera ignored it.")
        print("\nNOTE: the RTC only drives the camera's timecode when the camera's")
        print("timecode mode is 'Time of Day' rather than 'Record Run'. If the TC")
        print("didn't move, check that setting before blaming the packet.")
    return 0


def main():
    p = argparse.ArgumentParser(description="Probe: set a Blackmagic camera's clock over BLE")
    p.add_argument("--name", help="camera name or BLE address to target (default: first found)")
    p.add_argument("--time", help="UTC time to set as HH:MM:SS (default: now)")
    p.add_argument("--frames", type=int, default=0, help="frames field of the timecode (default 0)")
    p.add_argument("--tz-minutes", type=int, help="also set the Timezone parameter (minutes from UTC)")
    p.add_argument("--dest", type=int, default=BROADCAST,
                   help="SDI destination device id (default 255 = broadcast)")
    p.add_argument("--settle", type=float, default=3.0,
                   help="seconds to watch timecode either side of the write")
    p.add_argument("--scan-timeout", type=float, default=8.0, help="BLE scan duration")
    p.add_argument("--scan-only", action="store_true", help="scan and exit without connecting")
    p.add_argument("--all", action="store_true", help="list every LE device seen, not just cameras")
    p.add_argument("--dump-services", action="store_true",
                   help="enumerate the connected device's GATT services and characteristics")
    p.add_argument("--watch", type=float, metavar="SECONDS",
                   help="listen this long and exit without writing anything")
    p.add_argument("--method", choices=("rtc", "timecode", "both"), default="rtc",
                   help="rtc = set group 7.0 Real Time Clock (documented); "
                        "timecode = set group 9.4 (undocumented, what the camera "
                        "reports); both = try each in turn")
    p.add_argument("--dry-run", action="store_true", help="print the packet and exit, no Bluetooth")
    p.add_argument("--tc-char-test", action="store_true",
                   help="try writing timecode DIRECTLY to the Timecode "
                        "characteristic instead of the SDI tunnel")
    p.add_argument("--rtc-test", action="store_true",
                   help="write a deliberately wrong clock, check the timecode "
                        "jumps by that exact amount, then restore it")
    p.add_argument("--rtc-offset", type=int, default=-3723, metavar="SECONDS",
                   help="how wrong to make the test clock "
                        "(default -3723, ie 1h02m03s back)")
    p.add_argument("--tolerance", type=int, default=3, metavar="SECONDS",
                   help="how close the observed jump must be to count (default 3)")
    p.add_argument("--control-test", action="store_true",
                   help="briefly change and restore white balance, to prove whether "
                        "the camera obeys control writes at all")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="print every incoming camera-control message, not just new ones")
    args = p.parse_args()

    # Progress matters more than throughput here, and this often runs piped.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except Exception:
        pass

    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
