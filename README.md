# octomancer

**This entire repository is vibe-coded, so I wouldn't read too much into any of it...**

Synchronise a Blackmagic camera's timecode with a Tentacle Sync, using a Mac as
the proxy in the middle.

Two daemons, and the tools that talk to them. All C++, sharing one library:

| | |
|---|---|
| `octomancerd` | watches the Tentacle Sync bench and says when a box has drifted. Passive: it never connects to anything. |
| `octomancer-sync` | connects to the camera and sets its clock from the bench. The only part that acts. |
| `octomancer` | the command everyone runs. Asks the daemons things and tells them to do things, over a socket. |
| `Octomancer.app` | the same, with buttons. Also where notifications come from. |
| `octomancer-report` | reads the log the sync daemon leaves behind and says what the clocks are actually doing. |
| `octomancerctl` | the older, bench-only view of `octomancerd`. Still there; `octomancer` covers more. |

The split between the two daemons is deliberate and is the main design decision
in the project. `octomancerd` is meant to run all the time under launchd, so it
is built so that it *cannot* disturb a recording: there is no `connect` and no
`write` anywhere in it. Setting a clock is an action, and an action belongs in
a program somebody chose to run — and in one whose Bluetooth grant can be taken
away on its own, without also blinding the listener.

Neither daemon has a user interface. Both serve a Unix socket, and everything
you look at or press is a separate process asking over it. That is what lets
the app be quit, restarted, or never run at all without affecting a single
measurement.

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

### One of each, at a time

Each daemon takes a lock before it starts, so a second one refuses rather than
running alongside the first:

```
$ octomancer-sync
octomancer-sync: another one is already running (pid 14271)
```

Two of them connect to the same camera and share one file of learned biases,
and neither looks broken while they do it -- they just quietly disagree about
what the camera reads. The lock is held by the open file rather than written
into it, so a daemon that is killed outright does not lock its successor out.

The modes that never write anything take no lock and can be run next to a
running daemon: `--dry-run`, `--scan-only`, `--watch` and `--poke` for
`octomancer-sync`, and `--probe` for `octomancerd`.

## Nothing is written until you say so

**Octomancer will not change any camera it has not been told it may.** A fresh
install syncs nothing, and says so at startup rather than looking busy and doing
nothing:

```
$ octomancer writes on --camera A:1EAE18A7
A:1EAE18A7 writes enabled
saved to ~/.octomancer/cameras.conf
the running daemon has been told to re-read it
```

That permission covers everything the program can change on a body: its clock
*and* its timecode source. Off means octomancer reads the camera, reports on it,
and never touches it.

It lives in `~/.octomancer/cameras.conf`, which is a commented line format
meant to be opened in an editor. **The daemon only ever reads it**; only the
tools write it, so a setting cannot quietly become something else because a
measurement moved. Rewriting it preserves comments, ordering, and any setting a
newer version added, so editing by hand is not a thing you get punished for:

```
# octomancer camera configuration.
default writes=off
camera 09EE26AF-D630-DB5A-0CAC-ECB7B610DFBC writes=on name=A:1EAE18A7
```

After editing it by hand, `octomancer reload` makes the running daemon re-read
it. That happens between cycles rather than mid-decision, so a cycle never acts
on two different configurations.

This is deliberately separate from `~/.octomancer/per_camera.json`, which is the
daemon's own notebook — learned biases, measured apply delays, write history —
and which the daemon rewrites constantly. Permissions and measurements should
not share a file.

## Driving it

`octomancer` is the front door. It has no radio of its own: every command is a
question or an instruction put to a running daemon over its socket.

```
octomancer                          # status: the daemons, the bench, the cameras
octomancer list-cameras             # one line each
octomancer sync                     # correct the clock now, even if it looks fine
octomancer sync --camera A:1EAE18A7 # ...that one. Repeat --camera for several.
octomancer source                   # what is the timecode following?
octomancer source time-of-day       # make it follow the camera's clock
octomancer writes                   # what may be changed, and what may not
octomancer writes on --camera ID    # may octomancer change that camera at all?
octomancer writes off --all         # ...or every camera it knows about
octomancer reload                   # re-read the configuration after editing it
octomancer status --json | jq .     # for everything that isn't this program
```

The daemons themselves are started and stopped through launchd rather than a
socket, for the obvious reason:

```
octomancer start                    # ...and install the LaunchAgents if needed
octomancer stop
octomancer restart
octomancer restart --daemon sync    # just the one that writes to cameras
```

`sync` overrules the gates that mean *there is no need* — already close enough,
written recently, backed off after repeated failures. It does not overrule the
ones that mean *must not*: a camera that is recording, one whose timecode does
not follow its clock, or a daemon started with `--dry-run`. It says which it
refused on and why.

Because a correction takes tens of seconds — scan, connect, wait for a frame,
write on a second boundary, verify — the daemon takes the request, hands back an
id, and `octomancer` asks after it until it finishes. Interrupting the command
does not interrupt the sync; `--no-wait` queues it and returns immediately.

### The app

`Octomancer.app` is the same set of controls with a window: a camera picker, the
live figures, a **Sync Now** button, a timecode-source picker, and a menu-bar
item showing the bench at a glance.

It can notify you when:

* a sync fails,
* a camera syncs for the first time,
* a camera drops off the air,
* the Tentacle boxes disagree with each other — the bench failing to be one
  bench, which no amount of syncing against it can fix,
* the bench drifts away from this Mac, in ppm, since the absolute offset between
  timecode-of-day and a wall clock is a constant with no meaning and only its
  rate of change says anything.

Each is separately switchable, because which of those is worth interrupting
someone for is a matter of taste and not something the daemon should decide on
their behalf. The camera ones are events the daemon emits regardless and the app
filters, so turning one off costs nothing and turning it back on loses nothing
but the backlog.

The menu-bar icon can be hidden — it is a shortcut to the window, not the
program, and someone driving all this from the command line has no use for it.
Hidden, the app keeps running and keeps notifying (posting a notification needs
a bundled app, so this process is the only thing here that can); opening
Octomancer.app again brings the window back. Quitting it stops notifications and
nothing else: the daemons hold the clocks and do not care whether anybody is
watching.

There are **Start**, **Stop** and **Restart** buttons for the daemons, and the
window shows what launchd currently thinks of each.

**Start at boot** installs both daemons as LaunchAgents in your login session
and starts them. Agents rather than system daemons, and deliberately:
CoreBluetooth access is gated by the per-user privacy database, so a process
outside a login session has no user to grant it the radio. `make install-agent`
does exactly the same thing from a terminal.

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
been switched on deserves to be asked again.

The learned RTC bias and the measured send lead both survive, and now survive a
daemon restart too. Neither is a statement about what the clock currently
reads — the bias is which second the camera lands on, the lead is how long a
write takes to get there — and switching the camera off and on again changes
neither. They are also the two things here that cost hours to reacquire.

Switching the camera on is noticed within seconds rather than at the next poll:
`--presence-poll` (5 s) is a read of `octomancerd`'s socket, not a scan.
Without a daemon to ask, it falls back to scanning each cycle as before.

### Landing on the second, not near it

The RTC field is whole seconds, so the write is timed to land on a second
boundary: wait until the target instant is `--lead` away, then send. That only
works if `--lead` is actually how long the camera takes to act on the packet,
and the original 50 ms was a guess at BLE latency rather than a measurement.

The bench says the guess was low. Writes verified but landed consistently
behind — −0.118 s once and −0.099 s another time, against a trigger threshold
of half a frame (20.8 ms at 24 fps). A write that is sent `lead` early and still
lands `e` behind took `lead − e` to arrive, so those two are a camera taking
168 ms and 149 ms to act, not 50.

So the lead is now measured. After each verified write the residual gives one
observation, and the lead becomes the median of the last `--lead-window` (9) of
them, clamped by `--max-lead` (0.5 s). A median rather than a mean because the
camera reports whole frames: a single observation carries ±21 ms it cannot
resolve, and one write that landed during a mode change should not drag the
next nine. Nothing is used until `min_lead_samples` (3) have accumulated —
`--no-adapt-lead` keeps the configured value.

Only writes whose residual is a *fair* measurement count. A write that missed by
more than half a second missed because the whole-second RTC bias was wrong, and
that says nothing about timing; feeding it in would have the lead chasing a
whole second it can never reach.

### What is remembered about each body

Two learned figures are properties of the camera rather than of its current
clock reading: the whole-second RTC bias, and the sub-second apply delay above.
Neither is invalidated by a power cycle — switching a camera off and on changes
what its clock says, not how long a write takes to arrive — but both used to die
with the process, so every restart re-learned them at the cost of one rationed
write per attempt.

They now live in `~/.octomancer/per_camera.json`, keyed by the camera's BLE
identifier, alongside a bounded history of the writes they were derived from
(`--db-max-samples`, 1000 per body — about six weeks at one write an hour).
`--camera-db PATH` moves it; `--no-camera-db` turns it off. A database that
will not open is reported and then ignored: an unsyncable camera is a worse
outcome than a forgotten setting.

It is JSON Lines rather than a single document, with `camera`, `write` and
`compact` records, so recording an observation costs one appended line instead
of a full rewrite on a path that runs while the camera is connected. Reading it
back needs `jq -s` or a line at a time.

The file is kept from growing by compaction rather than by generational
rotation: when it passes twice what the retained data would occupy, it is
rewritten with each body's learned parameters first and then only the samples
still worth keeping, via a temporary file and a rename so a crash halfway
through leaves the old file intact. The threshold is a *ratio* rather than a
fixed size on purpose — with a fixed one, a database whose live set already sits
near the limit rewrites itself on almost every append.

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
  parameter is accepted by GATT and ignored. This is not an encoding mistake at
  our end — seven distinct encodings were tried, five of them against a
  deliberately frozen generator so that "no effect" and "small effect" could not
  be confused, and none moved anything. 9.4 is read-only telemetry.
  `doc/timecode-write-sweep.md` has the packets.
* **4.7 is writable, and it is the only write ever found that moves the
  timecode generator.** It selects what the timecode follows. At 0 it
  free-runs as time of day and follows the RTC, which is the mode this whole
  program depends on; at 1 it parks at `00:00:00:00` and stops. Anything above
  1 is clamped to 1.

  This matters beyond curiosity: with 4.7 set to 1 the camera reports a clock
  roughly twelve hours wrong that no RTC write can fix, so `octomancer-sync`
  reads 4.7 on connect and refuses to write, saying why, rather than trying
  once an hour forever.

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
