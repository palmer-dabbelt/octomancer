# Protocol notes

Derived from `doc/BlackmagicCameraControl.pdf` (August 2025), page numbers refer
to it, plus what a real Pocket Cinema Camera 6K Pro actually does on the air.
Where the two disagree, the camera wins and it's called out below.

## The shape of the thing

The BLE "Outgoing Camera Control" characteristic is a dumb tunnel for the
**Blackmagic SDI Camera Control Protocol**. The doc says the BLE messages "are
identical to those described in the Blackmagic SDI Camera Control Protocol
section" (p109), so there is no separate Bluetooth command set — we build an SDI
packet and write it to a GATT characteristic.

## Bluetooth LE (p109-110)

**Device Information Service** — UUID `180A`

| Characteristic | UUID | Notes |
| --- | --- | --- |
| Camera Manufacturer | `2A29` | always "Blackmagic Design" |
| Camera Model | `2A24` | e.g. "Pocket Cinema Camera 6K Pro" |

**Blackmagic Camera Service** — UUID `291D567A-6D75-11E6-8B77-86F30CA893D3`

| Characteristic | UUID | Access (observed) |
| --- | --- | --- |
| Outgoing Camera Control | `5DD3465F-1AEE-4299-8493-D2ECA2F8E1BB` | write |
| Incoming Camera Control | `B864E140-76A0-416A-BF30-5876504537D9` | indicate |
| Timecode | `6D8F2110-86F1-41BF-9AFB-451D87E976C8` | notify |
| Camera Status | `7FE8691D-95DC-4FC5-8ABD-CA74339B51B9` | notify, write, read |
| Device Name | `FFAC0C52-C9FB-41A0-B063-CC76282EB89C` | write |
| Protocol Version | `8F1FD018-B508-456F-8F82-3D392BEE2706` | read |

Camera Status is a bitfield: `0x01` power on, `0x02` connected, `0x04` paired,
`0x08` versions verified, `0x10` initial payload received, `0x20` camera ready.
Writing `0x01` powers a connected camera on; `0x00` powers it off. A connected
6K Pro sitting idle reports `0x03` (power on + connected).

Protocol Version reads as `302e312e3000000000000000` — ASCII `"0.1.0"`,
null-padded to 12 bytes.

**Bonding.** The doc says encrypted characteristics only work after bonding, and
that writing to one initiates it with a 6-digit PIN on the camera (p110). In
practice, against an already-paired camera, none of that surfaced — reads,
writes and notifications all worked immediately with no prompt.

## SDI packet format (p96-97)

Four-byte header, then the command, then padding to a 32-bit boundary:

```
byte 0   destination device   255 = broadcast, else 0-254
byte 1   command length       covers bytes 4..n; EXCLUDES header and padding
byte 2   command id           0 = change configuration
byte 3   reserved             0
byte 4   category             the "group" column in the parameter tables
byte 5   parameter
byte 6   data type            0 void/bool, 1 int8, 2 int16, 3 int32,
                              4 int64, 5 utf8, 128 fixed16 (5.11)
byte 7   operation            0 = assign, 1 = offset/toggle
byte 8+  payload              little-endian; padded with 0x00 to a 4-byte edge
```

The length field is the easy thing to get wrong — it counts from the category
byte, so a four-byte payload gives length 8, not 12. Little-endianness is
confirmed by the p105 example "set exposure to 10 ms" (10000 µs = `0x2710`)
encoding as `10 27 00 00`. `scripts/test_packets.py` checks the encoder against
all six worked examples on that page.

Two things the camera does that the doc doesn't mention:

* It sets the **reserved byte to `0xff`**, not `0`, in the packets it sends.
* It reports state using **operation type 2**, which the doc lists as reserved.
  Read it as "here is the current value" as opposed to 0 = "assign this".

## Groups

Documented: 0 Lens, 1 Video, 2 Audio, 3 Output, 4 Display, 5 Tally,
6 Reference, 7 Configuration, 8 Colour Correction, 10 Media, 11 PTZ.

**Groups 9 and 12 are undocumented** — the table jumps from 8 straight to 10 —
but the camera uses both constantly:

| Observed | Meaning |
| --- | --- |
| 9.0 | battery: `[millivolts, percent, flags]` |
| 9.4 | **running timecode**, int32 BCD `HHMMSSFF` |
| 9.1, 9.2, 9.5-9.8 | other status, not decoded |
| 12.9 - 12.12 | lens metadata strings ("Canon EF-S 24mm f/2.8 STM", "f2.9", "24mm") |

## Setting the clock (p102)

Group 7 (Configuration):

| ID | Parameter | Type | Index | Interpretation |
| --- | --- | --- | --- | --- |
| 7.0 | Real Time Clock | int32 | `[0]` time | BCD `HHMMSSFF`, **UTC** |
| | | | `[1]` date | BCD `YYYYMMDD` |
| 7.2 | Timezone | int32 | | minutes offset from UTC |

The full Real Time Clock packet is 16 bytes. For 2026-08-24 09:12:53:10 UTC
broadcast to all devices:

```
ff 0c 00 00  07 00 03 00  10 53 12 09  24 08 26 20
^dest        ^grp/param   ^time BCD LE ^date BCD LE
   ^len=12      ^int32/assign
```

Both BCD words stay well inside positive int32 range (max `0x23595929`), so
signedness never bites.

## Reading timecode — what the camera really sends

The doc describes the Timecode characteristic as "a 32-bit BCD number
(eg. 09:12:53:10 = 0x09125310)". That is **not** what arrives. A 6K Pro sends
**12 bytes**: a complete SDI message wrapping the value.

```
ff 08 00 ff | 09 04 03 00 | 18 14 55 17
             |             |
dest 255     | group 9     | BCD little-endian
len 8        | param 4     | -> 17:55:14:18
cmd 0        | int32       |
reserved ff  | assign      |
```

So the timecode is at **offset 8, little-endian BCD**, and it is reported as the
undocumented parameter 9.4. Notifications arrive roughly every other frame
(~7.5 Hz on a 24 fps body), and the frames field counts 0-23.

## The Timecode characteristic is read-only (tested 2026-08-24)

The Timecode characteristic `6D8F2110-...` is **not a separate service** -- it
lives inside the same Blackmagic Camera Service as the SDI tunnel. It *is* a
separate pipe though, so it was worth testing as a write target in its own
right: every other write in this document went to Outgoing Camera Control.

It was worth testing rather than ruling out on paper, because the doc describes
this characteristic as notify-only in the same paragraph where it claims the
payload is a bare 32-bit BCD number -- and the camera actually sends a 12-byte
wrapped SDI message. A doc that gets the read format wrong is not authoritative
about the write behaviour.

**Result: the camera refuses writes outright.** The 6K Pro advertises this
characteristic as `notify` and nothing else, and all three payload shapes were
rejected at the GATT layer:

| Payload | Bytes | Result |
| --- | --- | --- |
| bare BCD little-endian | `00 58 03 03` | `Write Not Permitted` |
| bare BCD big-endian | `03 03 58 00` | `Write Not Permitted` |
| wrapped SDI message | `ff 08 00 ff 09 04 03 00 00 58 03 03` | `Write Not Permitted` |

`BleakGATTProtocolError: (3, 'GATT Protocol Error: Write Not Permitted')` --
ATT error code 0x03. Timecode free-ran through all three attempts.

This is a **stronger** negative than the SDI-tunnel result. There the camera
ACKed and silently ignored us, which always leaves room for "our packet was
malformed". Here the GATT server refuses the write before any application logic
sees it, so payload shape is irrelevant -- there is no encoding that would work.

Full characteristic properties as advertised by the 6K Pro:

```
5dd3465f-...  write              Outgoing Camera Control
b864e140-...  indicate           Incoming Camera Control
6d8f2110-...  notify             Timecode          <-- no write property
7fe8691d-...  write,read,notify  Camera Status
ffac0c52-...  write              Device Name
8f1fd018-...  read               Protocol Version
```

Side note: the camera's timecode read `23:34` local while the probe's UTC target
was `03:03`, so this body runs time-of-day timecode in **local time**, not UTC --
worth remembering, since the documented RTC parameter is specified in UTC.

Reproduce with:

```
.venv/bin/python scripts/timecode_probe.py --name <addr> --tc-char-test
```

## Findings against a Pocket Cinema Camera 6K Pro (2026-08-24)

**Reading timecode over BLE works well.** The camera runs time-of-day timecode,
correct to the second, and streams it continuously.

**Setting it does not work.** Two attempts, both accepted by GATT and both
ignored by the camera:

| Attempt | Packet | Result |
| --- | --- | --- |
| 7.0 Real Time Clock (documented) | `ff 0c 00 00 07 00 03 00 ...` | no change |
| 9.4 Timecode (undocumented) | `ff 08 00 00 09 04 03 00 ...` | no change |

A GATT write-with-response ack only means the characteristic accepted the
bytes. It says nothing about whether the camera acted on them.

**This was isolated with a control test**, so the negative result is about the
camera and not about our packets. Writing white balance (1.2) 3200 → 5600 K
took effect and was echoed back in the camera's own telemetry, then restored:

```
white balance is currently [3200, 0]; setting it to 5600
  bytes: ff 08 00 00 01 02 02 00 e0 15 00 00
white balance now reads [5600, 0]
white balance restored to [3200, 0]
```

Same framing, same destination, same characteristic, same code path. So the
addressing and encoding are right, the BLE path is right, and **group 7 / 9.4
are simply not implemented on this body**. Consistent with that, group 7 never
appears anywhere in the camera's initial state dump, while groups 0, 1, 3, 9,
10 and 12 all do.

Reproduce with:

```
.venv/bin/python scripts/timecode_probe.py --name <addr> --method both
.venv/bin/python scripts/timecode_probe.py --name <addr> --control-test
```

### What this means for octomancer

The Mac cannot push timecode into a Pocket 6K Pro over Bluetooth. Approaches
still open, roughly in order of how promising they look:

* **Feed LTC into the camera's 3.5 mm input** from the Tentacle — the normal,
  supported path, and frame-accurate. The Mac's role shrinks to configuration
  rather than carrying timecode.
* **Set the camera's clock over USB** via Blackmagic Camera Setup, so its
  time-of-day timecode lines up. Coarse, but it's the only documented way to
  set this body's clock.
* **Invert the problem**: read the camera's timecode over BLE (which works
  well) and use it to discipline or annotate the other side.
* Re-test group 7 on a body that documents REST support (URSA Cine, PYXIS).
  The REST API has explicit timecode endpoints; the 6K Pro isn't on its
  compatibility list.

### Field note: Tentacle Sync boxes impersonate cameras

Tentacle Sync devices get named after the camera they are strapped to. During
this work the strongest BLE signal in the room advertised as `BMPCC` and was a
Tentacle Sync GmbH device, not a camera; `F55`, `FS5` and `FS7` were also
Tentacles. The actual 6K Pro advertised under the opaque name `A:1EAE18A7`.

**Match on the service UUID `291D567A-…`, never on the name**, and confirm with
the manufacturer string `2A29` before writing anything.
