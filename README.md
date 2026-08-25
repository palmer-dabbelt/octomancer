# octomancer

Synchronise a Blackmagic camera's timecode with a Tentacle Sync, using a Mac as
the proxy in the middle.

Three programs, all C++, sharing one library:

| | |
|---|---|
| `octomancerd` | watches the Tentacle Sync bench and says when a box has drifted. Passive: it never connects to anything. |
| `octomancer-sync` | connects to the camera and sets its clock from the bench. The only part that acts. |
| `octomancer-report` | reads the log the sync daemon leaves behind and says what the clocks are actually doing. |

The split between the first two is deliberate and is the main design decision
in the project. `octomancerd` is meant to run all the time under launchd, so it
is built so that it *cannot* disturb a recording: there is no `connect` and no
`write` anywhere in it. Setting a clock is an action, and an action belongs in
a program somebody chose to run.

## Building

```
./autogen.sh          # only from a git checkout
./configure --prefix=$HOME/.local
make && make check
```

Nothing but the C++ standard library, CoreBluetooth and AppKit — there are no
third-party dependencies to install, and `make check` needs no hardware. The
decoder, the packet encoder, the sync policy and the log reader are all
portable C++ kept clear of CoreBluetooth precisely so they can be tested that
way.

```
make install          # octomancerd, octomancerctl, octomancer-sync, octomancer-report
make install-agent    # run the watcher at login as a LaunchAgent
make install-app      # the menu-bar app, into ~/Applications
```

macOS will ask for Bluetooth permission the first time each binary runs. Until
it is granted, a scan finds nothing rather than failing, so grant it before
wondering why nothing happens. `octomancerd` and `octomancer-sync` ask
separately, on purpose: permission to listen and permission to write to a
camera are different things and should be revocable separately.

## The service

Check it works before installing anything:

```
./octomancerd --probe 15
```

which listens for fifteen seconds and prints what it heard:

```
octomancer  5 boxes, 5 live  radio poweredOn  up 15s  60 adverts
bench -6.205s vs this Mac,  spread +2.0ms across 5 live boxes

BOX               AGE  RSSI  TIMECODE             OFFSET     MEDIAN      DRIFT  RESOLUTION
BMPCC              0s   -43  22:19:02:16.038     -6.208s    -6.205s        ~4m  frame+us
Krysta             4s   -60  22:18:59.337        -6.205s    -6.207s        ~4m  microsecond
```

Once the agent is running, ask it what it can see:

```
octomancerctl              # one report
octomancerctl watch        # redraw until interrupted
octomancerctl json | jq .  # for everything that isn't this program
```

It notifies you when a box drifts more than a minute from this Mac, which is
the signal to re-jam it in the Tentacle app. That judgement is made on a median
rather than a single reading, with hysteresis and three-observation
confirmation, so a box parked near the threshold cannot spam you.

`doc/service-notes.md` covers the architecture, the wire protocol, the
threading, and why drift is refused rather than estimated from short samples.

## Keeping the camera on Tentacle time

```
octomancer-sync                       # sync to the Tentacle bench, forever
octomancer-sync --dry-run --poll 20   # decide and log, but never write
octomancer-sync --once                # a single cycle
octomancer-sync --once --source mac   # set the clock from this Mac instead
```

Each cycle it works out how far this Mac is from the Tentacle bench, connects
to the camera, and corrects its clock if that is both needed and allowed. If
`octomancerd` is running it takes the bench figure from there — the service
already keeps an hour of history per box and takes proper medians across it,
which is a far better number than anything a few seconds of listening can
produce. Failing that it listens for itself.

The camera is the expensive half: connecting takes seconds, and every
connection is a chance to disturb an operator mid-shot. So the Tentacle side is
sampled passively and the camera is touched once a minute at most, with a gate
on every write:

* **Recording.** Never touch the clock while transport mode (10.1) says Record.
  Jumping timecode mid-take corrupts the take.
* **Externally jam-synced.** Nothing in the protocol reports this, so it is
  inferred: if writes stop taking, something else is driving the camera. After
  `--max-failures` in a row it backs off rather than fighting it.
* **Already close enough.** Below the trigger threshold — **half a frame** by
  default, scaled to the frame rate the camera reports — leave it alone.
* **Written recently.** At most one write per `--min-write-interval` (an hour).

Those last two work together. Half a frame is tight enough that nearly every
cycle wants to write, which would be fine for accuracy and ruinous for
measurement: every write ends a free-running stretch, and drift can only be
computed across those stretches. So the threshold decides whether the clock is
wrong and the interval decides whether it is worth acting on yet, leaving an
hour of free-running drift between corrections.

The threshold is expressed in *frames* rather than seconds because "close
enough" means different things at 24 and 60 fps, and because a frame is the
camera's own unit: it reports whole frames, so below half a frame there is
nothing left to resolve. Half a frame is reachable at all only because the
write is *timed* — the RTC field holds whole seconds, so `octomancer-sync`
waits until the value it wants to write lands on a second boundary and sends
then, leaving BLE latency of tens of milliseconds rather than half a second.

Note that "did this write land?" is judged against `--write-tolerance` (1 s)
rather than the trigger threshold. Judging a write against half a frame would
mark every good write a failure, and `--max-failures` of those in a row is what
makes the daemon decide an external source owns the camera and stop.

Two things it learns rather than assumes: the camera's own RTC offset (−75 s
before a power cycle, 0 after one, so a fixed value never converges), and
whether the Tentacle bench agrees with itself.

Every cycle is logged to `octomancer-sync.jsonl`, including the ones where
nothing happened, so there is drift data to tune against later.

## Looking at the log

```
octomancer-report                          # octomancer-sync.jsonl by default
octomancer-report --segments some.jsonl
```

It reports drift only from free-running stretches between writes, and only from
stretches long enough to mean anything — the camera reports whole frames, so a
half-minute sample yields four figures of pure quantisation noise. It also
prints its own measurement floor, and refuses to report a drift figure that
falls below it. Expect it to say "leave it running" until there is an hour or
so of log.

## Probing, without writing anything

```
octomancer-sync --scan-only              # what is in range?
octomancer-sync --scan-only --all        # ...including everything else
octomancer-sync --watch 20               # connect and watch the timecode
octomancer-sync --packet                 # show the RTC packet bytes, no Bluetooth
octomancer-sync --rtc-test               # write a deliberately wrong clock
```

`--rtc-test` is the one diagnostic that writes. It exists because the first
attempt at testing the RTC wrote the *correct* time to a camera whose clock was
already right, saw nothing move, and concluded the parameter was unimplemented.
A test whose pass and fail states look identical is not a test, so this one
aims somewhere the camera demonstrably is not.

One trap worth repeating: **Tentacle Sync boxes are named after the camera they
are attached to**, so a device advertising as `BMPCC` is quite likely a
Tentacle. `octomancer-sync` matches on the camera's service UUID, and treats
FDAC service data as proof that a device is a Tentacle no matter what it calls
itself.

## What we know so far

Tested against a **Pocket Cinema Camera 6K Pro**:

* Reading timecode over BLE works well — time-of-day, continuous, ~7.5 Hz.
* **Setting the Real Time Clock (group 7.0) over BLE works**, and on a camera in
  Time of Day mode the timecode follows it. So the Mac *can* put wall-clock time
  on the camera with no cable, to about ±1 s.
* Two corrections are needed to land the right time: write **UTC** and let the
  camera apply its own timezone, and add **~75 s**, because the clock lands that
  far behind the value written — repeatably, with no spread. `octomancer-sync`
  measures that offset and learns it rather than assuming it, which turned out
  to matter: it was −75 s before a power cycle and 0 after one.
* Setting the timecode *directly* still doesn't work: the undocumented 9.4
  parameter is accepted by GATT and ignored.

Tested against a bench of **five Tentacle Sync boxes**:

* **Reading their timecode works, passively.** They broadcast it in the BLE
  advertising payload as service data under UUID `FDAC` — no connection, no
  pairing, and every box in the room can be read at once.
* The timecode is **plain binary, not BCD** — the opposite of Blackmagic.
* Two payload formats: frame-resolution `HH MM SS FF`, and a microsecond-of-day
  counter on the Track E, which is the better sync reference.

`doc/tentacle-notes.md` has the byte layouts, the evidence for each, and what's
still unidentified.
* The Timecode characteristic is **read-only**: it advertises `notify` alone,
  and writes to it are rejected with GATT `Write Not Permitted` whatever the
  payload. That closes the other BLE pipe -- and it's a firmer no than the SDI
  tunnel's silent ignore, since no encoding could change the outcome.
* This body runs time-of-day timecode in **local time**, not UTC.

`doc/protocol-notes.md` has the packet format, the UUIDs, the several places the
official documentation disagrees with the hardware, and where to go next.

One trap worth repeating: **Tentacle Sync boxes are named after the camera they
are attached to**, so a device advertising as `BMPCC` is quite likely a Tentacle.
Match on the service UUID and confirm the manufacturer string.
