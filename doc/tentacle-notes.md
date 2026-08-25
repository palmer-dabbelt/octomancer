# Tentacle Sync over Bluetooth LE

Reverse-engineered on 2026-08-24 against a bench of five boxes, using
`scripts/tentacle_scan.py`. Nothing here comes from documentation — it is all
inferred from what the boxes put on the air, so the confidence level of each
claim is stated.

## The short version

Tentacles broadcast their timecode in the **advertising payload**, as BLE
**service data** under the 16-bit UUID `FDAC`. That makes reading them
completely passive: no connection, no pairing, no bonding, and no limit on how
many boxes you listen to at once. A laptop can sit in the corner and read every
box in the room at the same time.

**The timecode is plain binary, not BCD.** This is the opposite of Blackmagic,
which packs BCD into the same kind of field. Byte `0x15` here means 21, not 15.
Getting this backwards produces a plausible-looking wrong answer rather than an
obvious error, which is why it is worth saying twice.

## Finding them

Do **not** match on the device name. Tentacles advertise under the name of the
camera they are strapped to, so the bench looks like a rack of cameras:

| Advertised name | Address (macOS UUID) | RSSI | Type |
| --- | --- | --- | --- |
| `BMPCC` | `B80D95C9-7D0B-140A-0351-2F4D55A1114E` | −40 | frame |
| `Krysta` | `F1139A0E-2275-1188-1855-9C5A0794D7FF` | −67 | microsecond |
| `FS5` | `E7EEBE32-6DCC-F159-B304-B45ACE7FCA0A` | −72 | frame |
| `F55` | `97A75BDD-1262-468A-4775-B53ADB34509F` | −73 | frame |
| `FS7` | `42723B20-45C0-272F-4313-973390EB1542` | −81 | frame |

Four of those five names are cameras. None of them is a camera. The real
Blackmagic 6K Pro in the same room advertises as `A:1EAE18A7`.

Match on **FDAC service data** instead. Addresses shown are CoreBluetooth
UUIDs, which macOS generates per host — they will differ on another machine, so
they are not identifiers to hardcode either.

Reception is the practical limit, not the protocol: at −81 dBm the FS7 produced
a single advert in 45 seconds, while the box at −40 produced 75.

## Payload formats

Three payload types were observed, distinguished by byte 0. All share the same
three-byte header.

### `0x22` — frame-resolution timecode, 9 bytes

```
22 7d 58 15 00 3a 0a a3 92
^  ^  ^  ^  ^  ^  ^  ^^^^^
|  |  |  |  |  |  |  sub-frame position (see below)
|  |  |  |  |  |  frames
|  |  |  |  |  seconds
|  |  |  |  minutes
|  |  |  hours          -- 0x15 = 21, plain binary
|  |  constant 0x58 on every box seen
|  flags/unit: 0x3d on four boxes, 0x7d on one
type
```

Decoded, that is `21:00:58:10`.

**Confidence: high.** Four physically separate boxes decode to the same
timecode within a second of each other, all sit the same −8.6 s from the host
clock, and the seconds field advances exactly once per second across minute and
hour rollovers. A wrong layout would not do all of that at once.

### `0x32` — microsecond-resolution clock, 8 bytes

Seen on one box (`Krysta`, a Track E, which also records audio).

```
32 3d 58 11 a2 23 f1 57
^  ^  ^  ^^^^^^^^^^^^^^
|  |  |  microseconds since midnight, 40-bit big-endian
|  |  constant 0x58
|  flags
type
```

`0x11a223f157` = 75 878 138 199 µs = 21:04:38.138.

**Confidence: high.** Fitting the counter against the host clock over 70
seconds gives **999 998 ticks/s — 1.0000 MHz** with a maximum residual of 3 ms.
A 40-bit field covers a full day (86 400 s needs 37 bits). Nothing but a
microsecond-of-day counter fits that rate and that range.

This box is the more useful sync reference of the two formats: it gives
sub-millisecond time, where the `0x22` boxes give whole frames.

### `0x42` — static, 9 bytes

```
42 7d 00 26 02 15 02 a1 00
42 3d 00 26 02 15 02 a1 00      <- different box, same payload
```

Interleaved with the timecode adverts. Contains no clock: the payload is
byte-identical across boxes and never changes over time, apart from the flags
byte in position 1. Not decoded, and there is no obvious reason to.

## What is still unknown

**Bytes 7–8 of `0x22` carry sub-frame position, but the scale is unresolved.**
Treating them as a fraction and adding it to the whole timecode improves the
linear fit against host time from 23 ms of residual to **3 ms** — an eight-fold
improvement, so they are certainly timing rather than payload. But the observed
range runs to 44 946, which exceeds one frame at 24 fps (41 666 µs), so they
are not simply microseconds-within-frame. Not pursued further; reading whole
frames does not require them.

**Byte 1 is per-unit but not unique.** `0x7d` on one box, `0x3d` on the other
four, stable per box across a session. Too coarse to be a serial number —
possibly a group, a link role, or a status bitfield.

**Byte 2 is `0x58` on every box**, in every payload type. A protocol version or
a fixed marker, but nothing observed varies it.

**Frame rate is inferred, not read.** The frames field was seen spanning 0..23
on the strongest box, which means 24 fps (or 23.976). Which byte encodes the
rate, if any, was not identified — a box running at another rate would settle
it immediately.

## Sync observations

The four `0x22` boxes are jammed together tightly: their offsets from the host
clock agreed within **0.2 s** of each other (−8.56 to −8.74 s), which is inside
the one-second quantisation of a whole-seconds field.

`Krysta` was **+76.8 s** away from that group — a real difference, far too
large to be a sampling artefact, and measured at microsecond resolution. It is
simply jammed to a different reference.

Two methodology notes that cost time and are easy to get wrong:

* **Compare offsets, not readings.** Comparing each box's most recent timecode
  charges every box for how stale its last advert was — a box last heard 4 s
  ago appears 4 s out of sync when it is fine. Comparing each box's offset
  *from the host clock at its own moment of receipt* cancels the staleness.
  Doing this wrong made three well-synced boxes look 2–4 s apart.
* **A whole-seconds field cannot resolve sub-second sync.** The `0x22` boxes
  quantise to the second, so about a second of apparent spread between them is
  the format, not the boxes.

Unexplained coincidence, recorded only so it is not rediscovered as if new: the
76.8 s Krysta gap is suspiciously close to the 75 s offset the Blackmagic camera
applies to RTC writes (see `protocol-notes.md`). No mechanism connects them —
different devices, different direction, different protocol — and the most likely
explanation by far is that Krysta was jammed at a different time. Treat as
coincidence unless something else turns up.

## Reproducing

```
.venv/bin/python scripts/tentacle_scan.py 45          # scan and decode
.venv/bin/python scripts/tentacle_scan.py 30 --raw    # every advert, raw bytes
```

Give it 45 seconds or more if you want the weak boxes: at −81 dBm a box may
only be heard once a minute.
