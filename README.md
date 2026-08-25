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

One `configure`, one `Makefile`, no recursion and no generated `config.h` —
a config header is a file every translation unit depends on and that
`./configure` rewrites, so keeping one would mean a no-op reconfigure rebuilt
the whole tree for the sake of a single version string.

While working on it:

```
./configure --enable-sanitizers   # address,undefined, in .cc and .mm alike
./configure --enable-werror
```

The sanitizer flags go into `CXXFLAGS`, `OBJCXXFLAGS` and `LDFLAGS` together,
because instrumenting the C++ differently from the Objective-C++ it links
against produces reports about nothing. Leak detection is deliberately not in
that set: LeakSanitizer is not implemented on macOS, so asking for it would
give a build that succeeds and silently never checks. Use
`leaks --atExit -- ./octomancerd --probe 5` for that.

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

It also watches for the camera, which is the one thing it does that is not
about Tentacles. Nothing is decoded and nothing is connected to — a camera puts
no clock in its advertisement — so all it can report is whether the camera is
on the air, and how many times it has come and gone:

```
camera on the air -- Pocket Cinema Camera 6K Pro  up for 2h14m,  3 sessions
```

That is worth having because it is the cheap half of a question
`octomancer-sync` would otherwise answer with a twenty-second scan every
minute. The radio is already listening, so noticing costs nothing.

A camera stops advertising while something holds a connection to it, so "off
the air" means "not advertising", not "switched off"; `--camera-gone-after`
(90 s) is set well above the twenty seconds a sync cycle spends connected, so
an ordinary correction is not mistaken for a power cycle.

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
sampled passively and the camera is touched as rarely as the arithmetic allows,
with a gate on every write:

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

### How often it looks

`--poll` is the floor, not the schedule. Two things have to be true before a
write can happen — the clock has to be wrong by more than the threshold, and
the rate limit has to have lapsed — and whichever is further off sets the
horizon. Each cycle sleeps a quarter of the remaining wait, so the cadence is
coarse when there is nothing to do and tightens as the moment approaches,
bounded by `--poll` below and `--max-poll` (15 minutes) above.

This is what a measured drift figure buys. At the −24.8 ppm this camera
actually shows, half a frame at 24 fps takes about nine minutes to accumulate,
so watching every sixty seconds is nine times more often than the clock can
change its mind. An hour that used to cost sixty connections now costs a dozen
or so, without ever arriving late.

The figure used for scheduling is deliberately pessimistic. The camera reports
whole frames, so a drift measured over an hour carries about 12 ppm it cannot
resolve; that is added rather than averaged away, and `--min-ppm` puts a floor
under the whole thing, because a clock that measured 2 ppm this afternoon is
not a clock that will hold 2 ppm all night. Arriving early costs a radio
wakeup. Arriving late costs the take.

### Power cycles

A power cycle resets the camera's RTC, which makes every drift figure measured
across it a measurement of the step rather than of the clock. This bench logged
−0.023 s at 06:32 and −3.897 s at 06:47: fitted as drift that is 4300 ppm, and
it is nothing of the sort.

Two things catch it. `octomancerd` counts the camera's absent-to-present
transitions, and a new session makes `octomancer-sync` sync immediately and
throw away what it had learned. Independently, any error jump larger than
`--restart-step` (1 s) between consecutive observations is treated as a clock
being *set* rather than a clock walking, which catches the case where the
camera went away and came back between two looks. Either way the drift
estimate, the drift anchor and the failure counters are cleared — including the
"an external source owns this camera" back-off, because a camera that has just
been switched on deserves to be asked again. The learned RTC bias survives,
being the one thing here that costs hours to reacquire.

Switching the camera on is noticed within seconds rather than at the next poll:
`--presence-poll` (5 s) is a read of `octomancerd`'s socket, not a scan.
Without a daemon to ask, it falls back to scanning each cycle as before.

### Logs

Every cycle is logged to `octomancer-sync.jsonl`, including the ones where
nothing happened, so there is drift data to tune against later. Both programs
rotate their own logs — `--log-max-bytes` (16M) and `--log-keep` (5) — which
bounds them at six generations on disk.

Rotating the human-readable output needs `--console PATH` rather than a shell
redirect or launchd's `StandardOutPath`. A file the program does not hold open
cannot be rotated safely: renaming it leaves the writer appending to an inode
with no name, which is how rotation silently loses a night of data.

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
