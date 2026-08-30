# The Tentacle Sync BLE protocol

Reverse-engineered on 2026-08-24 against a bench of five boxes. There is no
documentation for any of this — every claim below comes from captured traffic,
and each one says what the evidence is and how strong it is.

Captures were taken with a Python script that wrote raw adverts to JSONL so
the same data could be re-examined without going back to the radio; that script
has since been replaced by `octomancerd --log`, which writes the same kind of
record continuously. The headline figures come from a 150-second capture of 591 adverts
across all five boxes.

## 1. Transport

Tentacles put their clock in the **BLE advertising payload**, as **service
data** under the 16-bit UUID `FDAC` (`0000fdac-0000-1000-8000-00805f9b34fb`).

The consequences are worth stating plainly, because they shape everything built
on top:

* **It is passive.** No connection, no pairing, no bonding, no GATT. You cannot
  disturb a box by reading it, and a box cannot refuse you.
* **It is unlimited.** Every box in radio range is readable simultaneously.
  Reading five is no harder than reading one.
* **It survives anything.** No session to drop, no reconnection logic, no
  timeout handling.

This is the opposite of the Blackmagic side of this project, where reading
timecode needs a connection, and it makes the Tentacle the natural clock source
in any pairing of the two.

## 2. Finding the boxes

**Never match on the device name.** Tentacles advertise under the name of the
camera they are strapped to, so a bench of them looks like a rack of cameras:

| Advertised name | Address (macOS UUID) | Median RSSI | Payload type |
| --- | --- | --- | --- |
| `BMPCC` | `B80D95C9-7D0B-140A-0351-2F4D55A1114E` | −41 | `0x22` |
| `Krysta` | `F1139A0E-2275-1188-1855-9C5A0794D7FF` | −64 | `0x32` |
| `F55` | `97A75BDD-1262-468A-4775-B53ADB34509F` | −73 | `0x22` |
| `FS7` | `42723B20-45C0-272F-4313-973390EB1542` | −75 | `0x22` |
| `FS5` | `E7EEBE32-6DCC-F159-B304-B45ACE7FCA0A` | −78 | `0x22` |

Four of those five names are camera models. None of them is a camera. The real
Blackmagic 6K Pro in the same room advertises as `A:1EAE18A7`.

Match on **FDAC service data**. The addresses above are CoreBluetooth UUIDs
that macOS generates per host, so they are not portable identifiers either —
they will differ on another machine.

## 3. Advertising behaviour

From the 150 s capture, after collapsing consecutive duplicate payloads (macOS
re-reports the same advert several times):

| Box | adverts | unique | median interval | unique rate |
| --- | --- | --- | --- | --- |
| BMPCC | 243 | 124 | 1.17 s | 0.83/s |
| Krysta | 207 | 110 | 1.48 s | 0.75/s |
| FS7 | 62 | 42 | 3.27 s | 0.28/s |
| F55 | 59 | 43 | 3.05 s | 0.29/s |
| FS5 | 20 | 15 | 7.49 s | 0.13/s |

Roughly one new payload per second from a strong box. The rate falls off with
signal, not because the box slows down but because adverts are missed: at −78
dBm the worst gap was 20 s, and at −83 dBm in an earlier session one box
produced a single advert in 45 s.

**Practical consequence:** listen for at least 10 s to be confident of hearing
every box in a room, and 45 s or more if you care about the weak ones.

## 4. Packet types

Byte 0 is a type tag. Three values were seen. All three share a three-byte
header.

### `0x22` — timecode, 9 bytes

```
22 3d 18 15 00 3a a3 92
^  ^  ^  ^  ^  ^  ^  ^^^^^  bytes 7-8: microseconds into the frame, big-endian
|  |  |  |  |  |  frames
|  |  |  |  |  seconds
|  |  |  |  minutes
|  |  |  hours              0x15 = 21. Plain binary, NOT BCD.
|  |  frame rate            low 6 bits: 0x18 = 24 fps
|  flags                    see below
type
```

Full example: `22 7d 18 15 00 3a 0a a3 92` decodes to **21:00:58:10.041**.

### `0x32` — microsecond clock, 8 bytes

Seen on the Track E, which also records audio.

```
32 3d 18 11 a2 23 f1 57
^  ^  ^  ^^^^^^^^^^^^^^  microseconds since midnight, 40-bit big-endian
|  |  frame rate
|  flags
type
```

`0x11a223f157` = 75,878,138,199 µs = **21:04:38.138**.

### `0x42` — static, 9 bytes

```
42 3d 00 26 02 15 02 a1 00
42 7d 00 26 02 15 02 a1 00     <- a different box, otherwise byte-identical
```

Interleaved with the timecode adverts at roughly one in every thirteen. Carries
no clock: the payload never changes over time, and is identical across boxes
apart from the flags byte. Byte 2 is `0x00` here rather than a frame rate, so
byte 2's meaning is type-specific. Not decoded — most likely a product or
capability identifier.

## 5. The timecode is plain binary, not BCD

This is the single most important thing to get right, and it is worth stating
separately because of how it fails. Blackmagic packs timecode as BCD in a
structurally similar field; Tentacle does not. Byte `0x15` means **21**, not 15.

Decoding these as BCD does not raise an error. It produces a plausible-looking
wrong time that drifts in a plausible-looking way, and every value below 0x0A
decodes identically under both schemes, so casual spot checks pass. Any code
handling both vendors should keep the two decoders visibly separate.

**Evidence:** four physically separate boxes decode to the same timecode within
milliseconds of each other and all sit at the same offset from the host clock,
across minute and hour rollovers, with 0 of 212 payloads violating
`HH ≤ 23, MM ≤ 59, SS ≤ 59`. A wrong layout cannot do all of that at once.

## 6. Byte 2 is the frame rate

Low six bits of byte 2 = frames per second. Observed `0x18` = 24 on all five
boxes, consistent with the frames field spanning 0..23.

**Read it rather than inferring the rate from the largest frame number seen.** A
box heard only a few times may never show a high frame: in one capture that
made a 24 fps box look like 22 fps, which then corrupted every downstream
calculation that divided by it.

Bit `0x40` of byte 2 is separate from the rate — see below.

## 7. Bytes 7–8 are microseconds within the frame

This took three attempts to get right and is the most useful finding here,
because it takes the `0x22` boxes from frame resolution (42 ms) to a few
milliseconds.

The value is a 16-bit big-endian count of **microseconds elapsed into the
current frame**.

**Evidence.** Fitting decoded time against host monotonic time, and comparing
the residual with and without the microsecond term:

| Box | frames only | with µs | improvement |
| --- | --- | --- | --- |
| BMPCC | 12.1 ms | 4.2 ms | 2.9× |
| FS7 | 9.7 ms | 0.9 ms | 11× |
| F55 | 11.5 ms | 1.0 ms | 12× |
| FS5 | 12.6 ms | 0.7 ms | 18× |

Fitting the scale as a free parameter instead of assuming it gives 0.963,
0.982, 0.992 and 0.992 µs per unit on the four boxes — all within 4 % of
exactly 1 µs, from four independent devices.

**Two caveats, both honest.**

*The zero point is offset by about 3.6 ms.* Observed values run roughly
3600..45300 rather than 0..41666. The span matches one frame at 24 fps
(41,667 µs) almost exactly, so the scale is right, but the field does not start
at zero. A linear fit absorbs this into its intercept, so it does not affect
rate or drift measurements — but treat the absolute sub-frame value as carrying
a small constant bias of unknown origin.

*The pair is not a canonical spelling of an instant.* This follows from the
offset above and only matters to something that writes these packets rather
than reads them, which until now nothing did. Because the sub-frame value can
exceed a frame, a box may describe an instant as frame 3 plus 45.3 ms where the
same instant would more naturally be written frame 4 plus 3.6 ms. Both decode
to the same microsecond and both are correct. `octo::encode_timecode` in
`src/tentacle.h` therefore promises only that its output decodes back to the
time it was given — the test that holds it to this file compares the decoded
times, not the bytes, and 11 of the 212 timecode packets here are cases where
the bytes would differ.

*They are not a checksum, which was ruled out rather than assumed.* Two
identical 7-byte prefixes were observed carrying different suffixes, which
alone disproves any function of the preceding bytes. Beyond that, all 65,536
CRC-16 polynomials were tested in both bit orders and both endiannesses, using
the linearity trick that lets init and xorout cancel out — no match. Byte sum
and XOR match 0 of 212 payloads.

## 8. The flags bit

Byte 1 reads `0x3d` or `0x7d`; byte 2 reads `0x18` or `0x58`. In both cases the
difference is bit `0x40`, and in both cases it is stable within a capture but
changes between captures taken about twenty minutes apart.

Between those two captures the bench was re-jammed (see below), and the bit
flipped on every box. That is suggestive but not proof, and the direction was
not consistent across boxes: `BMPCC` held `0x7d` while the others held `0x3d`
in the first capture, and the reverse in the second.

**Unresolved.** Candidates are a jam-sync state, a link role, or a
slowly-toggling counter. Deliberately not guessed at further. It has no bearing
on decoding the time.

## 9. Clock quality and jam-sync behaviour

The Track E's microsecond counter makes it possible to measure the boxes rather
than just read them.

* **Rate accuracy:** the 40-bit counter ran at 1,000,002.3 ticks/s against the
  host clock — 2.3 ppm fast, over a 148-second window, with a maximum residual
  of 2,316 ticks (2.3 ms).
* **Stability:** its offset from the host clock varied by 5 ms across 110
  adverts, and the drift between the first and second halves of the capture was
  −2.91 ppm.
* **Agreement:** with all five boxes decoded to microseconds, their offsets
  from the host clock agreed to within **2 ms**, and in a later run to within
  **1 ms**.

That agreement is also the strongest single piece of evidence that the decode
is correct: a wrong layout would not produce agreement, only coincidence.

**The boxes synchronise each other, and that is why they broadcast
constantly.** The continuous advertising is not there for the benefit of
passive readers like this one — it is how the bench holds itself together.
Which means the millisecond agreement above is *designed behaviour*, not five
good crystals happening to line up, and an earlier draft of this document
calling them "independently clocked" had it backwards.

Two consequences follow, and both matter for anything built on top:

* **The bench drifts as a group.** Its ensemble rate is free-running with
  respect to any outside clock, including a Mac being disciplined by NTP. A
  common-mode offset growing between the bench and the host is therefore
  expected, and says nothing about either side being wrong.
* **A common-mode drift cannot be attributed.** Measuring bench-against-host
  gives their *relative* rate and only that. Deciding which one is moving needs
  a third reference, and there isn't one here. Over one hour all five boxes
  measured **−13.0 ppm ±0.2** against this Mac — about 1.1 s/day — and that
  figure belongs to the comparison, not to the boxes and not to the Mac.

The useful signal about an individual box is therefore its disagreement with
*the rest of the bench*, not its offset from the host. A box drifting away from
the group has fallen out of the sync mechanism; the whole group drifting away
from the host is business as usual.

The mechanism itself is unidentified — see section 11.

**Jam state is not permanent, and this bit them.** Earlier in the same session
the Track E sat **+76.8 s** away from the other four boxes: a real difference,
far too large to be a sampling artefact, and measured at microsecond
resolution. Some time later it was back in agreement at −6.23 s along with
everything else. Nothing in the protocol announced either state.

Given that the boxes synchronise each other, the likeliest reading is that the
Track E had dropped out of the sync group and was later pulled back into it,
rather than that it was re-jammed by hand. That is inference, not observation.

So: **a box being precise says nothing about it being correct.** Any tool that
picks a sync reference should check that the rest of the bench agrees with the
box it picked, rather than trusting resolution alone. The Python prototype that
did that, `tentacle_ref.py`, is gone; what carries the idea now is that nothing
picks a single box at all. `src/registry.cc` reduces each box to its own median
offset, and `measure_bench()` in `src/syncd.cc` takes the median across the
live, enabled boxes — one vote each — so a box that has fallen out of the sync
group is outvoted rather than followed. The disagreement is still reported: the
spread between the extreme boxes travels with the reading, and a spread wider
than `--bench-spread` (0.5 s by default) prints a warning. It warns and then
syncs anyway, which is worth knowing before trusting a write that happened
during one.

## 10. Comparing boxes correctly

Two methodology traps, both of which produced confidently wrong numbers here
before being caught.

**Compare offsets, not readings.** Ranking boxes by their most recent decoded
timecode charges each box for how stale its last advert was — a box last heard
4 s ago looks 4 s out of sync when it is fine. Compare each box's offset *from
the host clock sampled at that box's own moment of receipt*; the staleness then
cancels. Getting this wrong made three well-synced boxes appear 2–4 s apart.

**Take a median per box, then across boxes.** A strong box contributes ten
times as many adverts as a weak one, so pooling all adverts lets the strongest
box outvote the bench. Reducing each box to its own median first, then taking
the median of those, gives every box one vote and ignores a single box that is
jammed elsewhere.

## 11. What is still unknown

* **Byte 1 flags, and bit `0x40` of byte 2** — section 8.
* **The ~3.6 ms offset in the sub-frame field** — section 7.
* **The `0x42` static payload** — `26 02 15 02 a1 00` is identical on every box
  and never changes. No hypothesis worth recording.
* **Whether any box reports its own jam state.** Nothing observed distinguished
  the Track E's 76.8 s excursion from normal operation.
* **How the boxes synchronise each other.** That they do is established; the
  mechanism is not. Nothing in the decoded fields is an obvious candidate for
  carrying it, so it is presumably in the parts still unidentified — byte 1's
  flags, or the `0x42` payload. Worth attacking with two boxes and a Faraday
  bag: isolate one, let it drift, and watch what changes when it rejoins.
* **Behaviour at other frame rates.** Everything here was captured at 24 fps.
  Byte 2 should simply read a different value, and the sub-frame span should
  change to match, but that is a prediction, not an observation.
* **Drop-frame.** No candidate field identified. At 24 fps it does not arise.

## 12. Corrections to earlier versions of this document

Recorded because the mistakes are instructive, and because anyone re-deriving
this will probably make the same ones.

* **"Byte 2 is a constant `0x58`."** Wrong. It is the frame rate in the low six
  bits plus a flag bit. It looked constant because every box was in the same
  state during the first capture.
* **"Bytes 7–8 are unidentified, possibly a checksum."** Wrong. They are
  microseconds into the frame. An early fit appeared to confirm a sub-frame
  scale, then a later scan appeared to refute it — the refutation was itself
  flawed, because it required the divisor to exceed the largest observed value
  and so excluded the correct answer.
* **"The frame-resolution boxes can only pin an offset to about ±1 s."** Wrong,
  and it mattered: it made the Track E look uniquely valuable as a sync source
  when in fact every box gives millisecond-grade time.
* **"The Track E is 76.8 s out of sync."** True when measured, but it is a
  transient state and not a property of the device or the protocol.
* **"Five independently clocked devices."** Wrong: the boxes synchronise each
  other. Their agreement is the mechanism working, not evidence about crystal
  quality.
* **"The common-mode drift against this Mac is probably the Mac's clock."**
  Unfounded. Two clocks give you their difference and nothing else; attributing
  it needs a third.

## 13. Reproducing

```
./octomancerd --probe 45                     # scan, decode, print, exit
./octomancerd --log cap.jsonl --log-interval 5 --foreground   # for analysis
octomancerctl json | jq .                    # what the running service can see
```

Give the scan 45 s or more if you want the weak boxes; at −80 dBm a box may
only be heard once a minute.
