# octomancerd -- design notes

The C++ service that watches the Tentacle Sync bench, and the menu-bar app that
displays it. The Python in `scripts/` remains the research tool and the place
where the camera side lives; this is the part that is meant to run all the time.

## What it does, and deliberately does not do

`octomancerd` listens passively to BLE advertisements, decodes any Tentacle
payload it hears, and keeps a picture of every box in range. It **never
connects to a device and never writes to one**, which is what makes it safe to
leave running: it cannot disturb the Tentacle app, cannot interfere with a
camera holding a connection, and cannot touch a recording in progress.

It does not set anyone's clock. Correcting the camera is the Python daemon's
job and stays there for now; conflating "observe" with "act" in a service that
runs unattended is how an unattended service ends up doing something surprising
at three in the morning.

## Layout

```
src/tentacle.{h,cc}    the advert decoder: pure, portable, no radio
src/timeutil.{h,cc}    seconds-of-day, 24-hour wrap, formatting
src/registry.{h,cc}    device state, median offsets, drift fitting, alert policy
src/proto.{h,cc}       the wire format, both directions
src/server.{h,cc}      the control socket
src/client.{h,cc}      the other end of it, shared by the CLI and the app
src/render.{h,cc}      snapshot -> text a human reads
src/jsonlog.{h,cc}     append-only JSONL
src/scanner_mac.mm     CoreBluetooth. The only file that knows about Apple.
src/octomancerd.cc     the service
src/octomancerctl.cc   the control tool
ui/main.mm             the menu-bar app
```

The seam that matters is `src/scanner.h`. Everything above it is portable C++
with no Apple headers, which is what allows the decoder and the drift
arithmetic to be tested at all; everything below it needs a real antenna. If
that line ever blurs, the tests stop being able to run.

## Threading

Two threads, one lock.

CoreBluetooth delivers advertisements on its own serial dispatch queue.
`Registry` takes a mutex on every entry point and hands back self-contained
snapshots rather than pointers into live state, so the socket loop on the main
thread never observes a half-updated device. Advertisement rates are a few
hertz per box; there is no contention worth engineering around.

The main thread runs `poll()` over the listening socket and its clients, waking
every 200 ms to drain alert events and write periodic log lines. Signals are
handled by writing one byte to a self-pipe -- `write()` is async-signal-safe,
and almost nothing else that would be useful in a handler is.

## The wire protocol

Line-based, escaped `key=value`, versioned by the first line:

```
octomancer 1
snapshot wall=... radio=poweredOn devices=5 live=5 bench_offset=-6.2049 ...
device id=... name=Krysta offset=-6.2065 median=-6.2058 drift_ppm=-12.3 ...
device ...
end
```

Not JSON, and that is a decision rather than laziness. Every consumer in this
tree is a C++ program that would otherwise need a JSON *parser* -- rendering is
easy, parsing is where the bugs live -- and avoiding a third-party dependency
by hand-rolling one would trade a dependency for a supply of subtle bugs.
Splitting on spaces and then on the first `=` has no edge cases once values are
escaped, and box names arrive over the air from devices we do not control, so
they are escaped as hostile input. A box called `Cam 1 = A` is entirely legal
and walks straight through a naive tokenizer; `tests/test_proto.cc` uses
exactly that name.

Readers must ignore unknown keys, so the daemon can grow fields without
breaking an older UI. A genuinely breaking change arrives as a new version
number on line one, which is refused rather than misread.

`octomancerctl json` renders the same snapshot as JSON, for everything that
is *not* this program: `jq`, a scratch script, a future web view.

Connections are strictly one command, one reply, close. There is no
subscription and no streaming: a client that wants live data asks again, which
over a Unix socket costs microseconds and means neither side carries connection
state.

## Drift, and refusing to report it

Drift is fitted by least squares over the retained window and reported in parts
per million. The registry **refuses to report it** from a short lever arm --
by default under fifteen minutes, or under thirty samples.

This is the lesson the Python daemon learned expensively. Over a 20-second gap,
a camera reporting whole frames at 24 fps quantises to 42 ms, which invents
figures like `+453 ppm` and `-2858 ppm` on consecutive cycles. They look like
measurements. They are noise, and drift on these clocks is genuinely parts per
million, so measuring it needs a long lever arm rather than more samples. The
UI shows `~4m` in the drift column while it waits, with a tilde precisely so
nobody reads it as a number.

Per-box offsets are summarised by **median**, not mean, at both levels: median
over a box's samples, then median across boxes for the bench figure. One
mangled advert should not be able to declare a box out of sync, and one strong
box should not be able to outvote the bench.

**Read the drift figure as belonging to the comparison, not to the box.** The
Tentacles synchronise each other -- that is what the constant broadcasting is
for -- so the bench moves as a group, free-running against any outside clock
including an NTP-disciplined Mac. Measuring a box against the host gives the
relative rate of two clocks and nothing more; saying which one is moving would
need a third reference, and there is none here. In the first hour of running,
all five boxes measured -13.0 ppm +/- 0.2 against this Mac, which is about
1.1 s/day of separation and is unremarkable.

The corollary is that the per-box drift figures will nearly always agree with
each other, because they are mostly reporting the same common-mode term. What
distinguishes one box is its disagreement with the *rest of the bench*, which
is what `bench_spread` reports.

## Host clock steps

If NTP corrects this Mac, or the timezone changes, every offset moves at once.
Fitting a line through that discontinuity reports a spectacular and entirely
fictional drift. Monotonic time does not step, so comparing the advance of the
two clocks catches it: on a divergence over a second, the sample history is
discarded rather than fitted, and `clock_steps` is reported in every snapshot
so the gap in the data has a stated cause.

## Alerts

A box more than `--alert-threshold` (default 60 s) from this Mac needs
re-jamming in the Tentacle app. Three things keep that from becoming noise:

* the judgement is made on the **median**, not the latest reading;
* **hysteresis** -- it alerts above 60 s and only clears below 45 s, so a box
  sitting on the threshold cannot oscillate;
* **confirmation** -- three consecutive observations before either transition
  is believed.

Because the bench drifts as a group, this alert tends to fire for every box at
once -- the bench and the host have separated, and re-jamming brings them back
together. A *single* box alerting on its own means something different: that
box has fallen out of the sync group the others are still holding. Both call
for a re-jam, but only the second is a fault in a device.

Policy lives in the daemon, not the UI: the decision is made once, from the
full history, by the process that has the full history. The UI only displays
it. That is also why the UI can be quit and restarted freely.

Notifications are posted by the app rather than the daemon, because
`UNUserNotificationCenter` requires a bundled, signed application inside a
login session -- none of which a launchd agent has. For a headless install,
`--notify-command` runs `sh -c` on each transition. The box name arrives over
the air, so it is passed in `$OCTOMANCER_BOX` and friends and never
interpolated into the command string, which would let a device name off the air
execute as a shell command.

## Why a LaunchAgent and not a daemon

CoreBluetooth access is gated by the per-user privacy database. A system-wide
`LaunchDaemon` runs outside any login session, has no user to ask, and is
simply refused the radio. Running in the user's session is what makes the
antenna usable at all.

The `octomancerd` executable carries an embedded `Info.plist`
(`-sectcreate __TEXT __info_plist`) giving it a bundle identifier and an
`NSBluetoothAlwaysUsageDescription`. A bare command-line tool has neither, so
macOS has no name to show in the permission prompt and nothing stable to
remember the answer against. Launched from a terminal it borrows the terminal's
approval and appears to work; launched by launchd it would not.

## Tests

`make check` runs four binaries, none of which need a radio:

* `test_tentacle` decodes **322 real advertisements** captured from the bench
  and compares every field against expectations generated by the Python
  implementation that was validated on the hardware. A rewrite of a decoder is
  exactly where a plausible wrong answer hides -- reading a byte as BCD instead
  of binary yields a valid-looking time, never an error -- so this pins the C++
  to behaviour known to be correct in the field rather than to itself.
* `test_registry` drives the whole registry from a synthetic clock: median
  robustness, the drift refusal, alert hysteresis, the no-flapping band, clock
  steps, staleness.
* `test_proto` round-trips a snapshot including a deliberately hostile box name.
* `test_timeutil` covers the 24-hour wrap, which is the arithmetic that decides
  whether a box one second past midnight is one second fast or a day slow.
