# Setting timecode over Bluetooth LE: what we tried and how each attempt failed

**Camera:** Blackmagic Pocket Cinema Camera 6K Pro
**CCU protocol version reported over BLE:** `0.1.0`
**Camera firmware:** _(fill in from the camera's Setup menu — see note below;
it is not readable over BLE)_
**Host:** macOS, CoreBluetooth via `bleak`
**Tested:** 2026-08-24
**Reference:** Blackmagic Camera Control documentation, August 2025 edition

## Summary

There are two ways to put bytes into the camera over BLE. We tried both — three
attempts in total — and none of them changes the camera's timecode.

| # | Mechanism | Target | How it fails |
| --- | --- | --- | --- |
| 1 | SDI tunnel | group 7.0 Real Time Clock (documented) | GATT ack, silently ignored |
| 2 | SDI tunnel | group 9.4 Timecode (undocumented) | GATT ack, silently ignored |
| 3 | Timecode characteristic, written directly | — | GATT refuses: `Write Not Permitted` |

The two mechanisms fail in *different* ways, which matters when reading this
report: attempts 1 and 2 are accepted and dropped somewhere above the GATT
layer, while attempt 3 never reaches application code at all.

These two mechanisms are not a selection — they are everything available. The
camera exposes exactly two GATT services and nine characteristics in total, of
which only three are writable at all:

```
SERVICE 0000180a  Device Information
   00002a29  [read]                 Manufacturer Name String
   00002a24  [read]                 Model Number String
SERVICE 291d567a  Blackmagic Camera Service
   5dd3465f  [write]                Outgoing Camera Control   <-- mechanism A
   b864e140  [indicate]             Incoming Camera Control
   6d8f2110  [notify]               Timecode                  <-- mechanism B
   7fe8691d  [notify,read,write]    Camera Status
   ffac0c52  [write]                Device Name
   8f1fd018  [read]                 Protocol Version
```

Of the three writable characteristics, Camera Status takes only a power on/off
flag and Device Name takes only a display string. So Outgoing Camera Control is
the only general-purpose write path on the device, and the Timecode
characteristic is the only other one that concerns timecode at all. There is no
third avenue we have overlooked.

## Mechanism A — the SDI Camera Control tunnel

The doc (p109) states that messages written to **Outgoing Camera Control**
(`5DD3465F-1AEE-4299-8493-D2ECA2F8E1BB`) "are identical to those described in
the Blackmagic SDI Camera Control Protocol section", so this characteristic is a
transparent pipe for SDI protocol packets. The camera advertises it as `write`.

Our packet encoder is validated against **all six worked examples printed on
p105** of the documentation, byte for byte, with no hardware involved
(`scripts/test_packets.py`, 15 checks, all passing). So the framing below is
known-good against Blackmagic's own published examples.

### Attempt 1 — group 7.0, Real Time Clock (documented)

This is the documented way to set the camera clock (p102): group 7 parameter 0,
int32 array, `[0]` = time as BCD `HHMMSSFF`, `[1]` = date as BCD `YYYYMMDD`, in
UTC.

For 2026-08-24 09:12:53:10 UTC, broadcast:

```
ff 0c 00 00  07 00 03 00  10 53 12 09  24 08 26 20
^dest=255    ^group 7     ^time BCD    ^date BCD
   ^len=12   ^param 0     little-endian
             ^int32
                ^assign
```

**Result:** the GATT write-with-response completes without error. The camera's
timecode is unaffected and continues free-running time-of-day.

Corroborating detail: **group 7 never appears anywhere in the camera's initial
state dump.** On connection the camera pushes its full state over Incoming
Camera Control, and groups 0, 1, 3, 9, 10 and 12 all appear. Group 7 does not.
This is consistent with the group simply not being implemented on this body.

### Attempt 2 — group 9.4, Timecode (undocumented)

Group 9 is not in the published parameter table — the table skips from 8
(Colour Correction) to 10 (Media) — but this camera *reports* its running
timecode as group 9 parameter 4. Since the camera uses that parameter to
describe its own timecode, we tried assigning to it:

```
ff 08 00 00  09 04 03 00  <time BCD little-endian>
^dest=255    ^group 9
   ^len=8    ^param 4
             ^int32
                ^assign
```

**Result:** identical to attempt 1. GATT write completes, timecode unaffected.
Observed running undisturbed from 17:58:34 through 17:58:39 across the write.

### Why we are confident this is not a malformed-packet problem

A GATT write-with-response acknowledgement only means the characteristic
accepted the bytes. It says nothing about whether the camera acted on them. So
a silent no-op is ambiguous on its own, and we isolated it with a control.

We wrote **white balance** (group 1, parameter 2, int16) over the *identical*
code path — same characteristic, same destination byte, same header framing,
same helper function — and it took effect:

```
white balance is currently [3200, 0]; setting it to 5600
  bytes: ff 08 00 00 01 02 02 00 e0 15 00 00
white balance now reads [5600, 0]      <- echoed back in the camera's telemetry
white balance restored to [3200, 0]
```

The change was confirmed in the camera's own reported state, not merely assumed
from the ack. So the transport works, the addressing works, the packet framing
works, and the camera does obey control writes on this path. Groups 7.0 and 9.4
specifically are being accepted and discarded.

## Mechanism B — writing the Timecode characteristic directly

**Timecode** (`6D8F2110-86F1-41BF-9AFB-451D87E976C8`) is a separate
characteristic in the same Blackmagic Camera Service. The documentation
describes it only as a source of notifications and does not mention writing.

We tested it anyway rather than ruling it out on paper, for a specific reason:
the same paragraph that calls it notify-only also states the payload is "a
32-bit BCD number", and that is **not** what the camera sends (see below). A
description that is wrong about the read format is not authoritative about the
write behaviour.

Because nothing documents what a write *would* look like, we tried three
mutually exclusive payload shapes:

| Payload shape | Bytes | Result |
| --- | --- | --- |
| bare 32-bit BCD, little-endian — matches how the camera reports it | `00 58 03 03` | `Write Not Permitted` |
| bare 32-bit BCD, big-endian — matches how the doc writes it (`0x09125310`) | `03 03 58 00` | `Write Not Permitted` |
| full wrapped SDI message, as the camera itself emits | `ff 08 00 ff 09 04 03 00 00 58 03 03` | `Write Not Permitted` |

All three were rejected identically with **ATT error code 0x03, Write Not
Permitted**, surfaced as
`BleakGATTProtocolError: (3, 'GATT Protocol Error: Write Not Permitted')`.
Timecode free-ran through all three attempts (23:34:39 → 23:34:47).

This is a **firmer** negative than mechanism A. There, the camera accepted the
write and dropped it, which always leaves room for an encoding mistake on our
side. Here the GATT server refuses before any application logic runs, so the
payload shape is irrelevant — there is no encoding that would have worked.

The refusal matches the declared properties. As advertised by the 6K Pro:

```
5dd3465f-...  write              Outgoing Camera Control
b864e140-...  indicate           Incoming Camera Control
6d8f2110-...  notify             Timecode            <-- notify only, no write
7fe8691d-...  write,read,notify  Camera Status
ffac0c52-...  write              Device Name
8f1fd018-...  read               Protocol Version
```

## The ask

Both BLE paths into the camera's clock are closed, so there is currently no way
for a paired host to set this camera's timecode wirelessly — only to read it.
Either of these would be enough to enable it:

1. **Make the Timecode characteristic writable**, accepting the same wrapped
   SDI message the camera already emits on it. This looks like the smallest
   change: the parse path and the value semantics already exist.
2. **Implement group 7.0 (Real Time Clock)** on this body, as already
   documented on p102. This is the path the documentation implies should work,
   and it needs no new protocol surface at all.

The use case is straightforward and common on set: a host holding external
timecode (from a Tentacle Sync or similar) jam-syncing the camera over the
Bluetooth link that already exists, instead of requiring a cable into the
3.5 mm input or a USB trip through Blackmagic Camera Setup.

## Incidental findings, offered as documentation feedback

These are places where the August 2025 documentation and this camera disagree.
They cost us real debugging time and may be worth correcting regardless of the
request above.

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

**No bonding prompt appeared.** The doc (p110) says writing to an encrypted
characteristic initiates bonding and displays a 6-digit PIN. Against an
already-paired camera, reads, writes and notifications all worked immediately
with no prompt.

**Timecode is local time, not UTC.** The camera read `23:34` local while our UTC
target was `03:03`. Worth noting because the documented RTC parameter is
specified as UTC, so the two are in different frames of reference.

**Firmware revision is not exposed over BLE.** The Device Information Service
implements only Manufacturer Name (`2A29`) and Model Number (`2A24`). The
standard Firmware Revision (`2A26`) and Serial Number (`2A25`) characteristics
are absent, so a host application cannot tell which firmware it is talking to,
and cannot adapt to version-specific behaviour. Adding `2A26` would be a small
change with real value for anyone supporting a mixed fleet — and would have let
this report identify its own test firmware automatically.

## Reproducing

```
.venv/bin/python scripts/test_packets.py                        # encoder vs p105, no hardware

.venv/bin/python scripts/timecode_probe.py --name <addr> --watch 20      # read timecode
.venv/bin/python scripts/timecode_probe.py --name <addr> --method both   # attempts 1 and 2
.venv/bin/python scripts/timecode_probe.py --name <addr> --tc-char-test  # attempt 3
.venv/bin/python scripts/timecode_probe.py --name <addr> --control-test  # the white balance control
```

One practical note for anyone reproducing this: **Tentacle Sync boxes advertise
under the name of the camera they are attached to.** The strongest BLE signal in
our test environment advertised as `BMPCC` and was a Tentacle, not a camera. The
6K Pro advertised under an opaque name (`A:1EAE18A7`). Match on the service UUID
`291D567A-…` and confirm with the manufacturer string, never on the name.
