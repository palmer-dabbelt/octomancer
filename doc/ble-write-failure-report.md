# Setting the clock over Bluetooth LE: a 75-second offset, and no way to set timecode directly

**Camera:** Blackmagic Pocket Cinema Camera 6K Pro
**CCU protocol version reported over BLE:** `0.1.0`
**Camera firmware:** _(fill in from the camera's Setup menu — see note below;
it is not readable over BLE)_
**Host:** macOS, CoreBluetooth via `bleak`
**Tested:** 2026-08-24
**Reference:** Blackmagic Camera Control documentation, August 2025 edition

## Summary

Setting the **Real Time Clock (group 7, parameter 0) over BLE works** on this
body, and on a camera in Time of Day mode the timecode follows it. Two things
about it are worth reporting:

| # | What | Status |
| --- | --- | --- |
| 1 | RTC (7.0) write via the SDI tunnel | **Works, but lands ~75 s behind the value written** |
| 2 | Timecode (9.4) write via the SDI tunnel | GATT ack, silently ignored |
| 3 | Timecode characteristic, written directly | GATT refuses: `Write Not Permitted` |

Item 1 looks like a straightforward bug and is the main reason for this report.
Items 2 and 3 are a feature gap: there is no way to set the camera's *timecode*
over BLE, only its clock.

## Finding 1 — the RTC lands 75 seconds behind what was written

The write itself is honoured promptly: the timecode jumps within a second, and
by the amount asked for. Asking for a clock 1h02m03s in the past moved the
timecode by 1h02m00s:

```
timecode before: 20:36:15
WRITING Real Time Clock (7.0) = 2026-08-25 02:35:26 UTC
  bytes: ff 0c 00 00 07 00 03 00 00 26 35 02 25 08 26 20
timecode after:  19:34:21
clock moved -1h02m00s more than the 6.1s that actually elapsed
```

But the value that lands is consistently about **75 seconds earlier** than the
value sent. Writing the true current time and then comparing the camera against
the host's clock, once per second:

```
  t      camera tc    host clock   error
  +1    s 20:48:07     20:49:23     -76s
  +2    s 20:48:08     20:49:24     -76s
  +3    s 20:48:09     20:49:25     -76s
  +4    s 20:48:11     20:49:26     -75s
  +5    s 20:48:11     20:49:27     -76s
  +6    s 20:48:13     20:49:28     -75s
  -> settled at -75s (spread 1s)
```

Three properties of this offset:

* **It is not latency.** The camera lands 75 s behind the *value sent*, not
  75 s behind the moment it was sent — the discontinuity appears within one
  second of the write.
* **It is constant, not drift.** Spread across readings is 0–1 s, and the same
  −75 s appeared on every write we made across roughly twenty minutes.
* **It cancels exactly.** Adding 75 s to the written time brings the camera to
  the host's clock with zero error:

```
pass 2: writing RTC = 2026-08-25 03:50:43 UTC  (+75s bias)
  +1    s 20:49:29     20:49:29     +0s
  +4    s 20:49:32     20:49:32     +0s
  +6    s 20:49:34     20:49:34     +0s
  -> settled at +0s (spread 1s)
```

That last point is what makes us fairly confident this is a bug rather than
something on our side: an error we can subtract out as a clean constant isn't
coming from the encoding or the transport.

For completeness, the timezone behaviour is *correct* and is not part of this
report: the camera adds its Timezone parameter to the RTC before displaying, so
writing UTC (as p102 specifies) produces local time on screen. The 75 s is on
top of that, and is not a whole-minute or whole-hour quantity.

Our packet encoder is validated against **all six worked examples printed on
p105** of the documentation, byte for byte, with no hardware involved
(`tests/test_bmd.cc`, run by `make check`, all passing).

## Finding 2 — no way to set the camera's timecode directly

Separately from the clock, there is no way to jam-sync the camera's *timecode*
over BLE. Both available routes fail.

There are only two ways to put bytes into the camera over BLE, and this is not
a selection — it is everything. The camera exposes exactly two GATT services
and nine characteristics, of which only three are writable at all:

```
SERVICE 0000180a  Device Information
   00002a29  [read]                 Manufacturer Name String
   00002a24  [read]                 Model Number String
SERVICE 291d567a  Blackmagic Camera Service
   5dd3465f  [write]                Outgoing Camera Control   <-- route A
   b864e140  [indicate]             Incoming Camera Control
   6d8f2110  [notify]               Timecode                  <-- route B
   7fe8691d  [notify,read,write]    Camera Status
   ffac0c52  [write]                Device Name
   8f1fd018  [read]                 Protocol Version
```

Of the three writable characteristics, Camera Status takes only a power on/off
flag and Device Name only a display string. So there is no third avenue.

### Route A — group 9.4 over the SDI tunnel

Group 9 is not in the published parameter table — it skips from 8 (Colour
Correction) to 10 (Media) — but this camera *reports* its running timecode as
group 9 parameter 4. Since the camera uses that parameter to describe its own
timecode, we tried assigning to it:

```
ff 08 00 00  09 04 03 00  <time BCD little-endian>
^dest=255    ^group 9
   ^len=8    ^param 4
             ^int32
                ^assign
```

**Result:** the GATT write completes without error and the timecode is
unaffected, observed running undisturbed from 17:58:34 through 17:58:39 across
the write.

That the camera obeys control writes on this path generally is confirmed by a
control experiment: **white balance** (group 1, parameter 2, int16) written over
the *identical* code path took effect and was echoed back in the camera's own
telemetry.

```
white balance is currently [3200, 0]; setting it to 5600
  bytes: ff 08 00 00 01 02 02 00 e0 15 00 00
white balance now reads [5600, 0]      <- echoed back in the camera's telemetry
white balance restored to [3200, 0]
```

So the transport, addressing and framing all work — 9.4 specifically is being
accepted and discarded. (The same is true of the RTC path, which is how we know
Finding 1's offset isn't a framing problem.)

### Route B — writing the Timecode characteristic directly

**Timecode** (`6D8F2110-86F1-41BF-9AFB-451D87E976C8`) is described in the
documentation only as a source of notifications. We tested it anyway rather
than ruling it out on paper, because the same paragraph that calls it
notify-only also states the payload is "a 32-bit BCD number", and that is not
what the camera sends (see below) — a description that is wrong about the read
format is not authoritative about the write behaviour.

Because nothing documents what a write would look like, we tried three mutually
exclusive payload shapes:

| Payload shape | Bytes | Result |
| --- | --- | --- |
| bare 32-bit BCD, little-endian — matches how the camera reports it | `00 58 03 03` | `Write Not Permitted` |
| bare 32-bit BCD, big-endian — matches how the doc writes it (`0x09125310`) | `03 03 58 00` | `Write Not Permitted` |
| full wrapped SDI message, as the camera itself emits | `ff 08 00 ff 09 04 03 00 00 58 03 03` | `Write Not Permitted` |

All three were rejected identically with **ATT error code 0x03, Write Not
Permitted**, surfaced as
`BleakGATTProtocolError: (3, 'GATT Protocol Error: Write Not Permitted')`.
Timecode free-ran through all three attempts (23:34:39 → 23:34:47).

This is a firmer negative than route A: there the camera accepted the write and
dropped it, which leaves room for an encoding mistake on our side. Here the
GATT server refuses before any application logic runs, so the payload shape is
irrelevant — there is no encoding that would have worked. It matches the
declared properties, which advertise `notify` and nothing else.

## The ask

**The bug:** the RTC lands 75 s behind the value written. Since it is a clean
constant, it looks like a fixed offset applied somewhere in the write path.

**The gap, in order of how useful each would be:**

1. **Make the Timecode characteristic writable**, accepting the same wrapped
   SDI message the camera already emits on it. This looks like the smallest
   change — the parse path and the value semantics already exist.
2. **Or implement assignment to 9.4** over the existing SDI tunnel, which needs
   no new protocol surface at all.

The use case is common on set: a host holding external timecode (from a
Tentacle Sync or similar) jam-syncing the camera over the Bluetooth link that
already exists, instead of requiring a cable into the 3.5 mm input. The RTC
path gets us to roughly ±1 s once the 75 s offset is compensated, which is
enough to place clips on a timeline but not enough for frame-accurate sync.

## Incidental findings, offered as documentation feedback

Places where the August 2025 documentation and this camera disagree. They cost
real debugging time and may be worth correcting regardless of the above.

**The Timecode notification format is not a bare 32-bit BCD number.** The doc
says "Timecode (HH:MM:SS:mm) is represented by a 32-bit BCD number (eg.
09:12:53:10 = 0x09125310)". The camera actually sends **12 bytes** — a complete
SDI message wrapping the value:

```
ff 08 00 ff | 09 04 03 00 | 18 14 55 17
dest 255    | group 9     | BCD little-endian
len 8       | param 4     | -> 17:55:14:18
cmd 0       | int32       |
reserved ff | assign      |
```

The value is at **offset 8, little-endian**, reported as group 9 parameter 4.

**The reserved header byte is `0xff`, not `0`,** in packets the camera sends.
The doc specifies 0.

**Operation type 2 is used for state reports.** The doc lists 2–127 as
reserved. The camera uses 2 to mean "here is the current value", as distinct
from 0 "assign this".

**Groups 9 and 12 are undocumented** but used constantly: 9.0 is battery
(`[millivolts, percent, flags]`), 9.4 is running timecode, and 12.9–12.12 carry
lens metadata strings.

**Group 7 is write-only and absent from the state dump.** On connection the
camera pushes its full state over Incoming Camera Control; groups 0, 1, 3, 9,
10 and 12 appear, and group 7 does not — even though 7.0 is demonstrably
writable. Being able to read back the RTC and Timezone would make it possible
to verify a write instead of inferring it from the timecode, and would have
made the 75 s offset obvious immediately.

**No bonding prompt appeared.** The doc (p110) says writing to an encrypted
characteristic initiates bonding and displays a 6-digit PIN. Against an
already-paired camera, reads, writes and notifications all worked immediately
with no prompt.

**Firmware revision is not exposed over BLE.** The Device Information Service
implements only Manufacturer Name (`2A29`) and Model Number (`2A24`). The
standard Firmware Revision (`2A26`) and Serial Number (`2A25`) characteristics
are absent, so a host application cannot tell which firmware it is talking to,
and cannot adapt to version-specific behaviour. Adding `2A26` would be a small
change with real value for anyone supporting a mixed fleet — and would have let
this report identify its own test firmware automatically.

## Reproducing

```
make check                                            # encoder vs p105, no hardware

./octomancer-sync --once --source mac --camera <addr>  # finding 1, with calibration
./octomancer-sync --once --source mac --no-adapt-bias --camera <addr>   # raw offset

./octomancer-sync --watch 20 --camera <addr>          # read timecode, write nothing
./octomancer-sync --rtc-test --camera <addr>          # finding 1: the RTC write lands
```

The probes behind the *negative* results in Finding 2 -- assigning to the
undocumented 9.4 parameter, and writing to the Timecode characteristic -- were
one-shot experiments and were not carried over to the C++ tools. Their answers
are settled and are recorded above: 9.4 is accepted by GATT and ignored, and
the Timecode characteristic advertises `notify` alone and refuses every write
with `Write Not Permitted` regardless of payload. Re-testing them needs a
scratch program, not a maintained flag, and a flag whose only purpose is to
fail is a flag that rots.

One practical note for anyone reproducing this: **Tentacle Sync boxes advertise
under the name of the camera they are attached to.** The strongest BLE signal in
our test environment advertised as `BMPCC` and was a Tentacle, not a camera. The
6K Pro advertised under an opaque name (`A:1EAE18A7`). Match on the service UUID
`291D567A-…` and confirm with the manufacturer string, never on the name.
