# Running octomancer without any hardware

Almost everything in this program has only ever been exercised against five
Tentacle boxes and one Blackmagic camera, in one room, by one person. The
decision-making has tests — that is the seam `CLAUDE.md` describes, and it has
held — but the daemons above it have not, because until now there was no way to
give them anything to decide about.

`--radio fake` gives them a bench that is not there.

```
octomancerd --radio fake --probe 12
```

```
octomancerd: radio: a fake bench -- no radio is in use (the standard bench)
octomancer  5 timecode boxes, 5 live  radio poweredOn  up 12s  125 adverts
bench -3.593s vs this Mac,  spread +24.8ms across 5 live timecode boxes
camera on the air -- A:1EAE18A7  up for 12s,  1 session

BOX               AGE  RSSI  TIMECODE             OFFSET     MEDIAN  RESOLUTION
BMPCC              0s   -49  12:47:15:10.029     -3.578s    -3.578s  frame+us
F55                0s   -78  12:47:15:10.004     -3.603s    -3.603s  frame+us
FS5                0s   -80  12:47:15:10.022     -3.585s    -3.585s  frame+us
FS7                0s   -82  12:47:15:10.009     -3.599s    -3.598s  frame+us
Krysta             0s   -70  12:47:15.431        -3.593s    -3.593s  microsecond
```

Those numbers are the real bench's, rounded — five boxes about 3.59 s behind
this Mac, agreeing with each other to about 25 ms, all drifting around −23 ppm.
A default that resembled nothing would make the whole facility useless for the
thing it is for.

## It has to be asked for

`--radio auto` will never choose it, and neither will anything else. A program
that silently invented its own devices would be worse than one that found none:
the output of a fake bench looks exactly like the output of a real one, which
is the property that makes it useful and the property that makes an accidental
one dangerous.

Two ways in, and they mean the same thing:

```
octomancerd --radio fake
OCTOMANCER_RADIO=fake octomancerd
```

Setting a bench also selects it, because a spec that quietly did nothing is the
failure that would cost the most time here:

```
OCTOMANCER_FAKE='box,A,-1.5;box,B,-1.6,25,us' octomancerd --probe 5
```

## Describing a bench

Devices are separated by `;`, fields by `,`. A leading `@` reads the rest from
a file, in which newlines may be used instead of `;`.

```
box,<name>,<offset_s>[,<fps>][,<kind>][,<drift_ppm>]
cam,<id>,<name>,<error_s>[,<fps>]
```

`kind` is one of `frame+us`, `frame`, `us`, `static` — the three payload types
in `doc/tentacle-notes.md` plus the one that carries no clock at all. A bench
of a single kind leaves two decoder branches unvisited, which is how a fake
radio ends up proving less than it appears to.

A typo is refused with a reason rather than skipped. This is not politeness: a
bench that is subtly not the one you meant to build produces output
indistinguishable from the one you did.

## An isolated installation

Every path the program uses hangs off `$HOME`, so a whole second octomancer can
run beside a real one without touching it:

```
export FAKE=/tmp/octo-fake
mkdir -p "$FAKE/Library/Application Support/octomancer" "$FAKE/.octomancer"
printf 'camera A:1EAE18A7 writes=yes warn=yes name=Studio\n' \
    > "$FAKE/.octomancer/cameras.conf"

HOME=$FAKE OCTOMANCER_RADIO=fake ./octomancerd --foreground --log '' &
HOME=$FAKE ./octomancer status
HOME=$FAKE ./octomancer-ui
```

Its socket, its lock, its camera database and its configuration are all in
there. Nothing in the real installation is read or written, and the launchd
agents are untouched.

One limit worth knowing before you go looking for it: a unix socket path is
capped at 103 bytes, and the path above is 56 of them. A `$HOME` more than
about forty characters deep fails with `socket path is too long`, which is at
least a clear message but is not obviously about `$HOME`.

## What it does not do

It is not a simulation of Bluetooth. There is no advertising jitter, no packet
loss, no connection state machine, and the timing is a sleep in a loop rather
than a radio. It answers "what does the program above do when it is told these
things", which is the question that has been unanswerable — not "does the radio
work", which only hardware can answer.

The things it *can* arrange that hardware cannot, on demand:

* a box that goes off the air at a chosen second, and comes back at another
* a box drifting at a chosen rate, without waiting an hour to see it
* a bench that disagrees with itself by a chosen amount
* a box speaking a payload type nobody on the real bench speaks
* a payload carrying no clock, to check that it is refused

Each of those is an afternoon's work with five boxes in a room, which is why
none of them had ever been tested.

## How far the fake is trusted

The adverts are built by `octo::encode_timecode` in `src/tentacle.h`, which is
the inverse of the decoder every real advert goes through. That is the obvious
place for a fake to be quietly worthless: an encoder written from the same
misreading of the format as the decoder agrees with it perfectly, and a bench
built on it proves the decoder against itself.

So the encoder is pinned to the hardware rather than to its neighbour.
`tests/test_tentacle.cc` reads `tests/data/adverts.golden` — 212 payloads
captured off five real boxes — decodes each one, re-encodes from the time that
came out, and requires the same time back. `tests/test_fakebench.cc` then holds
the bench itself to the properties everything above it relies on: that a box
reports the offset it was given, that no advert is delivered twice or lost when
the caller polls irregularly, and that a box which goes quiet actually stops.
