# The Zoom BTA-1 timecode link

Notes from 2026-08-26, on a **Zoom F6** with a **BTA-1** Bluetooth adapter. The
F6 can take timecode over Bluetooth from an Atomos (formerly Timecode Systems)
**UltraSync BLUE**. We have the F6 and the BTA-1; we do not have an UltraSync
BLUE, and the goal is to find out whether the Mac can stand in for one, the way
`octomancer-sync` already stands in for a clock source on the Blackmagic side.

There is no published specification. Atomos calls the protocol patented and has
never documented it, and no teardown of it appears to have been posted. Every
claim below therefore says where it comes from, because two of them come from
the radio and the rest come from a firmware image.

**Status: nothing has been observed on the air yet.** The profile in section 3
is read out of Zoom's own firmware and has not been confirmed against the
hardware. Treat it as a strong lead, not as a fact.

## 1. What the radio said: nothing

This is worth recording because it cost the most time and produced the least.

With the BTA-1 lit and the F6 showing `Searching ...`, a full unfiltered BLE
scan from the Mac shows **no new device at all**. Not an unnamed one, not a
non-connectable one. Repeated across four scans of 25–60 seconds:

* Every device in range was identified and accounted for — the five Tentacles
  (all carrying `FDAC` service data), two GoPros, Govee lamps, a robot vacuum,
  a headset, a fridge, a cell-signal booster reading out as `Nextivity`, and a
  crowd of Google/Eddystone beacons.
* Switching the adapter off and diffing the scans proved nothing, and could not
  have: every device that "vanished" was a rotating-address privacy beacon,
  which changes identity every few minutes whatever we do. **A device-set diff
  is not a valid instrument in a room full of Fast Pair beacons.**
* A Mac advertising itself as `UltraSync BLUE` (local name, plus a
  Nordic-UART-shaped service) drew no connection from the F6 in five minutes.

The last point is weak evidence at best — if the F6 filters on a service UUID,
and section 3 says it does, then an advertisement without that UUID would be
ignored no matter what it called itself.

The most likely explanation for the silence is simply that the F6's pairing
window closes after a timeout, and every scan so far has missed it. That is
testable now that section 3 gives us something specific to look for.

## 2. Where the answers actually were: Zoom's firmware

Zoom publishes F6 firmware as a plain download (`F6_v2.20E.zip`, 13 MB, from
the F6 support page). Inside is `F6SYSTEM.BIN`, 16 MB, and it is **not
encrypted** — roughly 15 500 ASCII strings, including the TI SYS/BIOS task
table with every task and semaphore name intact:

```
g_p_TskId_WireLessModuleUltraSyncBlueHandler
g_p_TskId_WireLessModuleUltraSyncBlueBootLoadHandler
g_p_TskId_WireLessModuleUltraSyncBlueBatteryMonitorHandler
g_p_SemId_SetWireLessModuleUltraSyncBlueTimecodeUpdate
g_p_SemId_SetWireLessModuleUltraSyncBlueUpStreamData
g_p_SemId_SetWireLessModuleUltraSyncBlueDownStreamData
```

The `BootLoad` handler is the important one. **The BTA-1 has no permanent
personality: the F6 flashes firmware into it at run time**, and there are two
separate bootload handlers — one plain `WireLessModule`, one
`WireLessModuleUltraSyncBlue`. The adapter is a different device depending on
which mode you picked.

The images are embedded in `F6SYSTEM.BIN` as ARM Cortex-M0 blobs, marked
`DA14580-01` and carrying the RivieraWaves `RW-BLE` stack banner — so the BTA-1
is Dialog DA14580 silicon. Three of them:

| Image | Offset | Identity | Purpose |
| --- | --- | --- | --- |
| A | `0x1e3000` | `DA14580-01`, name `ZOOM D289` | the F6 Control app link |
| B | `0x1ea380` | DIS strings `ZOOM` / `F6` / `v_0.18` | UltraSync BLUE, older |
| C | `0x1f0600` | DIS strings `ZOOM` / `F6` / `v_0.21` | UltraSync BLUE, newer |

B and C are the same firmware at two revisions; both report the same BLE stack
version `v_3.0.9.504` and **both advertise the same service UUID**, so this
does not depend on which F6 firmware is installed. (Checked in v2.20; the unit
here is on v2.00.)

## 3. The profile

Images B and C carry three characteristic user descriptions —

```
Server TX Data\0Server RX Data\0Flow Control\0
```

— followed immediately by a table of 16-byte little-endian UUIDs. That is the
shape of Dialog's Serial Port Service (DSPS), but with custom UUIDs rather than
Dialog's published base. **The timecode protocol is a byte stream tunnelled
over a serial port profile**, which means the interesting content is in the
stream, not in the GATT layout.

| Role | UUID |
| --- | --- |
| Service | `5e981594-cd7d-4201-86b9-560cf375abae` |
| Server TX Data | `4076b47f-130b-407c-a2f4-d52945cb84b5` |
| Server RX Data | `cbeb8809-028a-4195-b4a5-762ff6e500a9` |
| Flow Control | `f0262f5f-3eba-4719-a265-31f126a9c66c` |

The service UUID also appears twice in each image inside a `0x11 0x07`
structure — AD length 17, AD type "complete list of 128-bit service UUIDs":

```
11 07 aeab75f30c56b98601427dcd9415985e
│  │  service UUID, little-endian
│  AD type 0x07, complete list of 128-bit service UUIDs
AD length 17
```

That is an **advertising data template**: the BTA-1 in UltraSync mode
advertises, and advertises this UUID.

Two further facts settle the role beyond argument. At `0x1efea4` the image
carries a `0x2800` **primary service** declaration followed by six `0x2803`
characteristic declarations — a GATT server's attribute database, which only a
server has — and the three `Server TX Data` / `Server RX Data` / `Flow Control`
strings are Characteristic User Description values, which a client would never
need to store. **Images B and C are GATT servers.**

Two consequences:

* **The BTA-1 is the peripheral and the GATT server.** It hosts the service, it
  hosts a Device Information Service reading `ZOOM` / `F6`, and it puts its
  service UUID in its advertisement. The UltraSync BLUE connects *to it*.
  Atomos's own manual agrees — "the UltraSync BLUE searches for other Bluetooth
  devices in range… when it detects your recording device, it will pair with
  it" — as does Zoom's, whose pairing step 4 is "on the UltraSync BLUE, select
  the F6 as the device to connect".
* **So the Mac plays the central**, which is the role this project already
  plays for the Blackmagic bodies. No advertising impersonation is needed, and
  macOS's restrictions on custom advertising payloads — it will not emit
  manufacturer data at all — do not bite.

The naming is from the module's point of view: *Server TX* is the BTA-1
talking, *Server RX* is the BTA-1 listening. A Mac pretending to be an
UltraSync BLUE would subscribe to TX and write timecode into RX.

**Those four are all of them.** The UUID array was read out of the image
directly, at `0x54b3` in image C and `0x5a8b` in image B, and the two are
byte-identical: one service, three characteristics, no second service, and no
alternative UUID held in reserve. Anything scanning for this device has exactly
one thing to match on.

Worth correcting an earlier misreading in case it survives elsewhere: the six
`0x2803` characteristic declarations near that array are **not** the custom
service. They are a bog-standard Device Information Service — `0x2a29`
Manufacturer, `0x2a24` Model, `0x2a25` Serial, `0x2a26`/`0x2a27`/`0x2a28`
revisions, `0x2a23` System ID, `0x2a2a`, `0x2a50` PnP ID — whose values are the
`ZOOM` / `F6` / `v_0.18` strings. The custom service has no static attribute
table at all, because RivieraWaves builds 128-bit services at run time.

### Image A, and a device name

Image A has no attribute table and no Device Information Service. Instead
`ZOOM D289` sits in the DA14580 **configuration header** near the start of the
image, alongside the stack version banner and an all-`ff` BD address
placeholder meaning "take the address from OTP":

```
0x1b6  09ff0060 52572d424c45 0000...    `RW-BLE
0x1d6  5a4f4f4d20443238 3900...         ZOOM D289
0x216  ...6432 ffffffffffff             BD address: use OTP
```

That is a **device name**, not a service, which makes control-app mode findable
by name with no UUID involved — and that turns it into the diagnostic the
timecode mode cannot provide. See section 6.

## 4. What is still unknown

Everything that matters, namely **the byte stream itself**. The GATT layout
says only that there is a pipe; it says nothing about what goes down it. Open
questions, roughly in the order they can be answered:

1. Does the BTA-1 speak first on connect, or wait to be addressed? A capture of
   TX notifications immediately after connecting settles this at no risk.
2. What does `Flow Control` carry? In stock DSPS it is an on/off byte, and if
   this is unmodified DSPS then the stream may not start until it is set.
3. What is the timecode packet? The F6 side has a
   `SetWireLessModuleUltraSyncBlueTimecodeUpdate` semaphore, so timecode
   arrives as discrete updates rather than a continuous clock.
4. Is there a handshake or pairing identity that a stand-in would have to
   forge? `UltraSyncBlueBatteryMonitorHandler` implies the F6 reads the
   UltraSync's battery over the same link, so the stream carries at least
   telemetry as well as time.

## 5. How to look for it

Scan **filtered on the service UUID**, not unfiltered:

```
btdump --services 5E981594-CD7D-4201-86B9-560CF375ABAE --auto-gatt
```

A filter is not merely tidier. CoreBluetooth will surface a device whose
service UUID landed in the advertisement's overflow area *only* when the scan
explicitly names that UUID, so an unfiltered scan can miss a device that a
filtered one finds. Given that four unfiltered scans found nothing, this is
worth taking seriously rather than assuming the adapter was simply idle.

The pairing window appears to be short. Trigger `Pair` on the F6 with the scan
already running, not before.

## 6. The open contradiction

As of the end of this session the two halves do not agree, and it is worth
stating plainly rather than papering over:

* The firmware says the BTA-1 in UltraSync mode is a GATT server that puts
  `5e981594-…` in its advertisement. Section 3 gives the evidence, and it is
  strong — an attribute database and an advertising template are not ambiguous.
* The radio says nothing is there. Eight scans totalling over an hour,
  including a **thirty-minute scan filtered on that exact UUID**, several taken
  while the F6 displayed `Searching ...`. The adapter has never been seen in
  *any* mode. Everything the long scans did turn up was a rotating-address
  beacon — Google `FE50`/`FE2C`, Tuya, and a handful of tags cycling their
  identities — and none of them carried the service.

Both cannot be true of the same moment, so at least one premise is wrong. The
candidates, cheapest first:

1. The F6 was not actually in the UltraSync timecode mode — the menu item
   reached was the F6 Control app pairing, or the mode needs the software
   extension file installed before it appears at all.
2. The adapter is not radiating: not fully seated, or the F6 does not power it
   in the state it was left in.
3. Range. Adverts from other rooms show up at −83 dBm here, so this would need
   the F6 to be much further away than "same desk".

4. ~~Something already holds a connection to the adapter.~~ **Ruled out.** A
   connected peripheral stops advertising, and F6 v2.0 auto-connects to the F6
   Control app at power-on, which made this attractive — it was the only theory
   predicting silence in *every* mode. But the adapter was bought the same day
   and has never been paired with the app at all, so nothing can be holding it.

5. The boot-load into the adapter is failing. The F6 flashes the module over
   UART every time the mode is entered; if that fails, the adapter can sit
   there lit and completely silent. This now fits best, and it is consistent
   with a brand-new adapter and an F6 that was a firmware revision behind.

### The test that separates these

Put the adapter in **control-app mode** and scan for the *name* `ZOOM D289`.
That mode needs no UUID, no UltraSync, and no pairing window, so it isolates
one question cleanly:

* **`ZOOM D289` appears** — the adapter transmits, the radio path is fine, and
  the fault is specific to the timecode mode (candidate 1 or 5).
* **Nothing appears in either mode** — the adapter is not transmitting at all,
  and this stops being a protocol problem. Not seated, not powered, dead unit,
  or a boot-load that never completes.

Everything in sections 2 and 3 stands either way: it was read out of Zoom's
firmware, not inferred from the radio.

A Mac advertising the real service UUID with all three characteristics drew no
connection either, which rules out the mirror-image theory (that the adapter is
a client hunting for that UUID) about as well as a negative can.

One detail for whoever looks next: the timecode-mode images contain **no local
name** advertising structure — no `0x08`/`0x09` AD type anywhere in them. The
adapter should therefore appear as a nameless device whose advertisement
carries nothing but flags and the one 128-bit service UUID. Do not look for a
device called `ZOOM` anything; that string exists in image A and in the Device
Information Service, neither of which is the advertisement.

The three module images have been carved out of the firmware for disassembly
(Cortex-M0): offsets `0x1e2f00`, `0x1ea380` and `0x1f0600`, lengths 29 824,
25 216 and 29 696 bytes.
