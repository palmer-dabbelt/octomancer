# octomancerd -- design notes

The C++ service that watches the Tentacle Sync bench, and the menu-bar app that
displays it. This is the part that is meant to run all the time. The camera
side -- the half that connects and writes -- is `octomancer-sync`, a separate
binary for the reasons in "What it does, and deliberately does not do" below.

## What it does, and deliberately does not do

`octomancerd` listens passively to BLE advertisements, decodes any Tentacle
payload it hears, and keeps a picture of every box in range. It **never
connects to a device and never writes to one**, which is what makes it safe to
leave running: it cannot disturb the Tentacle app, cannot interfere with a
camera holding a connection, and cannot touch a recording in progress.

It does not set anyone's clock. Correcting the camera is `octomancer-sync`'s
job and stays there; conflating "observe" with "act" in a service that runs
unattended is how an unattended service ends up doing something surprising at
three in the morning.

It does watch for the camera, which is the one thing here that is not about
Tentacles, and it is worth being precise about how little that means. A camera
advertises the Blackmagic camera-control service UUID and nothing else useful
-- no clock, no transport state. So the only fact available from a distance is
*it is on the air*, and that is all the registry records.

That fact is worth a great deal anyway, because the alternative way of learning
it costs a twenty-second scan. `octomancer-sync` used to pay that every minute
whether or not there was anything to find; now it reads this over a socket
every five seconds instead, and gets a faster answer for a fraction of the
cost. The radio here is already scanning unfiltered for Tentacles, so noticing
a camera in the same callback costs nothing at all.

Two things to keep in mind about it:

* **Absence means "not advertising", not "switched off".** A camera stops
  advertising while something holds a connection to it. `octomancer-sync` used
  to hold one for about twenty seconds every cycle it acted, which is what
  `camera_gone_after` of 90 s was sized against: set it below a cycle's
  connection and every correction would read as the camera being power-cycled.

  It now holds the connection *between* cycles as well, by default, so the
  presence signal for a working camera is false essentially all of the time.
  That is not a fault to be fixed in this file -- `octomancer-sync` treats its
  own live connection as presence, which is a better signal than an
  advertisement because it is the thing the advertisement was evidence for.
  What it does mean is that `octomancer status` will report a camera as not on
  the air while it is being held, and that is correct rather than confusing
  once you know the connection is the point. See `doc/pairing-notes.md` for why
  letting go is expensive.
* **A second camera is ignored.** The first one heard wins and the rest are
  dropped, because alternating between two would flap the presence flag and the
  session counter on every advertisement. Choosing between cameras is
  `octomancer-sync --camera`'s job, where there is a connection to choose with.

## Layout

```
src/tentacle.{h,cc}    the advert decoder: pure, portable, no radio
src/timeutil.{h,cc}    seconds-of-day, 24-hour wrap, formatting
src/registry.{h,cc}    device state, median offsets, drift fitting, alert policy
src/proto.{h,cc}       the wire format, both directions
src/server.{h,cc}      the control socket
src/client.{h,cc}      the other end of it, shared by the CLI and the app
src/render.{h,cc}      snapshot -> text a human reads
src/jsonlog.{h,cc}     append-only JSONL, log rotation, JSON escaping
src/logscan.{h,cc}     reading that JSONL back: strict, flat, not a parser
src/camsync.{h,cc}     the write gates, drift, the poll schedule, the send lead
src/camdb.{h,cc}       what each camera body has taught us, across restarts
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

There is a `camera` line too, and it is **always** emitted, even before a
camera has ever been heard:

```
camera seen=0 id= name= present=0 rssi=0 age=0.00 since=0.00 sessions=0 ...
```

That looks redundant and is not. A reader has to distinguish "this daemon is
watching and there is no camera" from "this daemon is too old to be watching",
because the first means there is nothing to do and the second means find out
the expensive way. Absence of the line is the second; `seen=0` is the first.
`tests/test_proto.cc` pins both.

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

## Logs, and keeping them from eating the disk

Both the JSONL and the human-readable output rotate: past `--log-max-bytes`
(16M) the live file is renamed aside, `PATH.n` becomes `PATH.n+1`, and
`PATH.<keep>` is deleted. That bounds the whole thing at `(keep + 1)` files.

The subtle part is the console output, and it is why `--console PATH` exists at
all rather than leaving people to redirect. **A file the program does not hold
open cannot be rotated by it.** Rename a file that launchd opened as
`StandardOutPath`, or that a shell opened with `>`, and the writer keeps
appending to an inode with no name: the "rotated" file stops growing, the new
file stays empty, and a night of output goes to a place nothing can read it.
So the program opens the file itself, points both `stdout` and `stderr` at the
same descriptor, and re-`freopen`s after the rename. The LaunchAgent plist uses
`--console` for the same reason.

Rotation is checked after a record is written and between cycles, never
mid-line: a log whose last entry is half a JSON object is a log that breaks the
reader that was the point of writing it.

Reopening deliberately counts the bytes already in the file rather than
starting from zero. A daemon restarted every few minutes -- which is exactly
what happens while working on one -- would otherwise append forever and never
reach its own threshold.

`tests/test_jsonlog.cc` runs this against a real filesystem rather than a mock.
The failure that matters is a rename that loses a generation, and a mock would
happily agree it did not.

## The per-camera database

`octomancer-sync` keeps what it has learned about each body in
`~/.octomancer/per_camera.json`. Two things live there, and the reason they are
worth persisting is the same for both: they are properties of the camera, not of
its current clock reading, and each one costs a rationed write to acquire.

* **The RTC bias**, a whole number of seconds. This body's offset is not a
  constant of nature — it was −75 s before a power cycle and 0 after one — so it
  has to be learned rather than configured, and re-learning costs up to
  `max_adapts` writes at one an hour.
* **The apply delay**, sub-second, measured from the residual left after a
  verified write. See "Landing on the second, not near it" in the README.

Neither is cleared by `forget_drift()`, and neither should be: a power cycle
changes what the clock says, not how long a write takes to reach it.

### Single writer, on purpose

`octomancerd` never touches this file, even though it is the process that knows
about camera sessions. Two writers would need locking, and the interesting data
— what a write did — only exists in `octomancer-sync`. `octomancerd` remains the
process with no `connect` and no `write` in it, which is the property that makes
it safe to leave running during a take.

### Why a log rather than a document

The obvious implementation rewrites a JSON document on every observation. That
puts a full serialise-and-fsync on a path that runs while the camera is
connected, for a file that changes by one record at a time. Instead this is JSON
Lines: an observation costs one appended line.

The records are flat objects only, which is what lets `src/logscan.h` read them
without growing into a general JSON parser. That constraint is also why a
checkpoint is a *run of lines* rather than one line holding an array — a
compaction writes a marker, then one `camera` line per body, then the retained
`write` lines. Replay does not need to know a compaction ever happened, because
later records simply win.

### Compaction, and why the threshold is a ratio

Rather than generational rotation — which is right for logs, where the history
is the product — this file is compacted: rewritten with each body's learned
parameters first, then only the samples still worth keeping, bounded at
`max_samples` (1000) per body.

The trigger is `bytes >= live_bytes * compact_factor`, not a fixed byte count.
With a fixed threshold, a database whose retained set already sits near the
limit rewrites itself on nearly every append; with a ratio the wasted space is
bounded at `(factor - 1)` and the amortised cost of an append stays constant.

The rewrite goes to a temporary file, is fsynced, and is renamed into place. The
fsync is before the rename deliberately: a durable rename over non-durable
contents is the one failure that loses everything at once. We are the only
writer and we reopen afterwards, so the rename cannot strand anyone on an
unnamed inode — the failure mode that makes rotating someone else's log unsafe.

A line that does not parse is skipped rather than fatal. The realistic way this
file gets damaged is a lid closing mid-append, which truncates exactly one line
at the end, and that should cost one observation rather than the history.

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

`make check` runs nine binaries, none of which need a radio:

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
* `test_camdb` covers the per-camera database against a real filesystem, not a
  mock: the two things there that can lose data are compaction, which rewrites
  the file, and replay, which is all that stands between a truncated line and a
  lost history. It also pins the arithmetic the send lead is derived from,
  including that a learned bias of zero does not read back as "never measured"
  -- if it did, the daemon would spend a rationed write re-learning it after
  every restart.
