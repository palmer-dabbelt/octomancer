# octomancer

**This entire repository is vibe-coded, so I wouldn't read too much into any of it...**

Synchronise a Blackmagic camera's timecode with a Tentacle Sync, using a Mac as
the proxy in the middle.

Two daemons, and the tools that talk to them. All C++, sharing one library:

| | |
|---|---|
| `octomancerd` | watches the Tentacle Sync bench and says when a timecode box has drifted. Passive: it never connects to anything. |
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
octomancer  5 timecode boxes, 5 live  radio poweredOn  up 15s  60 adverts
bench -6.205s vs this Mac,  spread +2.0ms across 5 live timecode boxes

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
about timecode boxes. Nothing is decoded and nothing is connected to — a camera
puts no clock in its advertisement — so all it can report is whether the camera
is on the air, and how many times it has come and gone:

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

It notifies you when a timecode box drifts more than a minute from this Mac,
which is the signal to re-jam it in the Tentacle app. That judgement is made on
a median rather than a single reading, with hysteresis and three-observation
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
default enabled=on
default warn=off
camera 09EE26AF-D630-DB5A-0CAC-ECB7B610DFBC writes=on  warn=on name=A:1EAE18A7
box    42723B20-45C0-272F-4313-973390EB1542 enabled=off        name=FS7
```

The same file describes the timecode boxes, because there is one question a
person is answering — use this thing, ignore that one — and splitting it in two
would only mean two places to look when a device does not show up. A box's
setting is `enabled`, and it defaults to **on**, which is the opposite answer to
the camera's and is deliberate. Listening to a box is passive: the timecode is
in the advertisement, nothing is connected to and nothing is written, so hearing
a box nobody asked about costs nothing and denies nobody anything. Writing to a
camera is an action taken on someone's equipment. Permission is worth insisting
on for the second and pointless for the first.

```
$ octomancer disable --box FS7
FS7 disabled
saved to ~/.octomancer/cameras.conf
```

That is for the timecode box in the next room that is not part of this shoot:
it drops out of every list and stops voting on what the time is. `octomancer
enable` with no `--box` reports what the file says, including the boxes it has
never heard of — on a healthy bench the file holds only the exceptions, and a
report that listed only those would look exactly like one where nothing is
configured at all.

After editing it by hand, `octomancer reload` makes the running daemon re-read
it. That happens between cycles rather than mid-decision, so a cycle never acts
on two different configurations.

This is deliberately separate from `~/.octomancer/per_camera.json`, which is the
daemon's own notebook — learned biases, measured apply delays, write history —
and which the daemon rewrites constantly. Permissions and measurements should
not share a file.

## Being told when something is wrong

Both kinds of line also carry `warn`, which is not a permission but a request:
*tell me when this one is wrong.*

```
$ octomancer warn on --box FS7 --camera A:1EAE18A7
FS7 will be warned about
A:1EAE18A7 will be warned about
saved to ~/.octomancer/cameras.conf
```

A device with it set gets a marker in `octomancer status` and colours the
menu-bar circle, in one of two ways:

* **red** — it is being heard, and it is more than **100 ms** from the
  canonical time. That is past anything normal: a jammed bench sits within a
  few milliseconds of itself, and a camera an hour after its last write is tens
  of milliseconds out, which is what the re-write cycle exists to mop up. 100 ms
  is about two frames at 24, which is far enough that somebody would see it in
  the edit. Anything tighter would go red every time a camera got warm.
* **yellow** — nothing has been heard from it for **five minutes**, so there is
  no opinion to have. A stale offset is not evidence of being in sync; it is a
  measurement of where the device was the last time anybody listened, and
  drawing an hour-old reading as though it were current is how somebody ends up
  shooting against a clock that walked off while they were not looking. Better
  to say "we do not know" than to say something reassuring that nothing
  supports. Five minutes because a box at -84 dBm can genuinely go three
  minutes between advertisements, and a light that flickers whenever somebody
  stands in front of the cart is a light nobody reads — but short enough that a
  device switched off, or carried out of the room, is noticed within a setup
  break rather than in the rushes. Yellow also covers having nothing to compare
  against — no canonical time, or a device that has not said what time it
  thinks it is — because staying quiet there would amount to saying it is fine.

Red beats yellow, since a device known to be wrong outranks one we cannot
speak for. A camera whose link is being *held* is heard by definition and never
goes yellow for silence: it stopped advertising because something is talking to
it.

It defaults to **off**, and that default is the whole design. An indicator that
lights up about a timecode box sitting in a case in the truck, or about a
camera nobody is shooting with today, is one people learn to ignore inside a
day — and a red light that has been learned to mean nothing is worse than no
red light at all, because it is still there on the day it means something. So
somebody has to name the devices they are actually working with before anything
goes red on their behalf. For the same reason a device that has been switched
off never warns: switching it off is how somebody says they are not working
with it today, and it would be no relief at all if the light stayed on
afterwards.

`octomancer warn` with no argument reports what the file says. Unlike
`octomancer enable`, it does not go on to list the devices the file has never
heard of: they are off, everything is off until asked for, and printing the
whole room to say so would bury the two lines that matter. `octomancer status`
is where the room is listed.

## Driving it

`octomancer` is the front door. It has no radio of its own: most commands are
a question or an instruction put to a running daemon over its socket. The two
that need one -- `scan` and `pair` -- hand the work to `octomancer-sync`, which
has a radio already. A second binary asking for its own Bluetooth grant would
be a second thing to approve, and a second thing to be quietly refused.

```
octomancer                          # status: both daemons and every device
octomancer status --verbose         # ...and the rest of what they know
octomancer tui                      # the same page, staying put and redrawing
octomancer list-cameras             # one line each
octomancer scan                     # which Blackmagic cameras are on the air?
octomancer scan --all               # ...and every other LE device, which is how
                                    #    you tell a silent camera from a deaf radio
octomancer pair                     # bond with the camera: it shows a six-digit
                                    #    code on its own screen, macOS asks for it
octomancer sync                     # correct the clock now, even if it looks fine
octomancer sync --camera A:1EAE18A7 # ...that one. Repeat --camera for several.
octomancer source                   # what is the timecode following?
octomancer source time-of-day       # make it follow the camera's clock
octomancer writes                   # what may be changed, and what may not
octomancer writes on --camera ID    # may octomancer change that camera at all?
octomancer writes off --all         # ...or every camera it knows about
octomancer enable --box FS7         # listen to that timecode box
octomancer disable --box FS7        # ...or ignore it, and stop counting its vote
octomancer warn                     # which devices are worth a red light?
octomancer warn on --box FS7        # tell me when that one is wrong
octomancer warn off --camera ID     # ...and stop telling me about that one
octomancer reload                   # re-read the configuration after editing it
octomancer status --json | jq .     # for everything that isn't this program
```

### What status shows

`octomancer status` — which is what you get by typing `octomancer` with nothing
after it — asks both daemons, because neither of them can see the whole room.
`octomancerd` hears the timecode boxes and `octomancer-sync` connects to the
cameras. What somebody standing at the bench wants is one list, with numbers
that mean the same thing on every line:

```
DEVICE            AGE     OFFSET LINK         RSSI
BMPCC              1s     +1.3ms on the air    -51
F55                2s     -0.8ms on the air    -76
FS5                4s     +0.8ms on the air    -81
Krysta             1s     -0.8ms on the air    -55
FS7             3m02s         -- off the air   -84
A:1EAE18A7     56m32s         -- off the air    --
```

`RSSI` is there because "why is this one not being heard" comes up constantly
and the answer is usually in that column. The bench in this room reads about
-51 to -55 dBm; a box at -84 is at the edge of what gets decoded and will go
quiet for minutes at a time. It is also how the camera in these examples was
diagnosed: never heard above -75 dBm even in the same room, about 20 dB quieter
than everything else in the rig.

On a terminal the colour carries two rules and no decoration. A device nobody
is hearing is dimmed all the way across, because every figure on its row is a
memory rather than a measurement — the age is how long ago, the signal is how
loud it was then, the timecode is what it said at the time. Half a row dimmed
would read as a bug in the table; none of it dimmed reads as a device that is
fine. The headings are the one row that is always true, so they are drawn in
their own colour rather than in the ink that means "do not trust this number".
Piped to a file it is all plain text, and the `!` and `?` markers are
characters rather than colours so nothing that matters is lost on the way.

That is the whole of it, deliberately. There is no version, no pair of daemon
lines and no arithmetic above the table, because none of that changes between
one run and the next: the command cannot answer at all unless the daemons are
running, and they start themselves. Four lines of preamble before the thing
somebody ran the command for is how a status page stops being read. `--verbose`
has all of it, and so does this page a few paragraphs down.

Two things do speak without being asked, and both are exceptions rather than
inconsistencies. A daemon that is *not* answering says so, because the table
will then be quietly missing half the room and nothing else would explain it.
And when no box is live at all, the line saying there is nothing to measure
against is printed — the offsets are all dashes at that point, and a column of
dashes with nothing above it looks like a broken program rather than an empty
room. Saying why something is missing is not verbosity.

The offsets in that table are **not** against this Mac. Everything either
daemon measures is quoted against the Mac's clock, and the Mac's clock is the
least interesting one in the building — it is the thing being compared with,
not the thing anybody is shooting against. So the Mac's error is stated once,
in the header, and each row carries the device's distance from the *canonical*
time: the median across the live, enabled timecode boxes. Four boxes nearly two
seconds away from a laptop that has not seen an NTP server all week are still
in millisecond agreement with each other, and `--verbose` is the view that says
so:

```
canonical time  -1.773s vs this Mac,  spread +2.1ms across 4 timecode boxes on the air
canonical source: octomancerd
1 timecode box off the air: listed below, but not voting on the canonical time and not in the spread
```

`OFFSET` is blank for a device nobody is hearing, and the blank is deliberate.
A box that has gone quiet goes on free-running while the canonical time goes on
moving, so the gap between the two widens for as long as the silence lasts:
`FS7` above would read tens of milliseconds out after an hour away, having done
nothing whatsoever to deserve it. That number is a measurement of the silence,
not of a sync error, and putting it in the same column as the live boxes on a
page whose spread is two milliseconds invites exactly one conclusion — that the
spread must be wrong. It is not. A box off the air is out of the median and out
of the spread, which is what the second header line exists to say out loud, and
the last raw thing it said survives in the `MEDIAN` column under `--verbose`,
where it is quoted against this Mac and so does not rot.

The boxes come first and the cameras after, and within each the devices being
heard come before the ones that are not: `FS7` above has not been heard for
three minutes, so it sits below the boxes that are still talking. Everything
else stays in the order the daemon listed it in, which does not rearrange
itself between polls. A device somebody has switched off gets no row at all —
it is counted under the header instead, as `1 device hidden: disabled in the
configuration`, because a device missing on purpose and a device missing for a
reason nobody has found yet should not look the same.

`AGE` is how long ago the device was last heard from, which is the column that
answers "is this thing still here" — more useful, day to day, than when it was
last synced. A camera whose link is being held reads as `held` with an age of
zero rather than ageing away: it stopped advertising *because* something is
talking to it, and showing that the same way as a camera somebody switched off
would have people power-cycling a body mid-write. Nobody has yet watched that
column with a camera in the room, which is the only way to find out whether
`held` means what it says; `doc/KNOWN_ISSUES.md` says what that would take.

A device somebody has asked to be warned about carries a marker in the `DEVICE`
column — `!` when it is out of sync with the bench, `?` when it has been quiet
too long to say — and the names are repeated under the table with what each mark
means, because "one device out of sync" only sends somebody looking. The marker
rides inside the name column rather than in one of its own, so a table that
already fills a narrow terminal does not get two characters wider for a flag
most rows do not carry.

Either daemon may be down, and the half that answered still prints. Which one
went quiet is usually the answer rather than an obstacle, so it is said in
plain words at the top, and the exit status only goes non-zero when neither
answered.

`--verbose` adds the rest: signal strength, the timecode each device is
reading, its median offset against this Mac before the canonical time is
subtracted off it, drift, frame rate, which daemon the canonical time came
from, and everything `octomancer-sync` knows about each camera. It also prints
what launchd makes of the two daemons, which is the question the moment one of
them stops answering.

`--json` is not that table in another format. It hands back
`octomancer-sync`'s own answer unchanged, because the merging happens here, in
the process that asked both daemons; `octomancerd`'s half of the room is
`octomancerctl json`.

Two daemons means two sockets, and they are not interchangeable. `--socket` has
always meant `octomancer-sync`'s control socket and still does; `octomancerd`'s
is `--bench-socket`. Pointing one at the other fails somewhere deep inside a
reply rather than at the flag, which reads like a corrupt daemon, so the two are
kept apart.

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

### Watching it, rather than asking it

`octomancer tui` is that same page, on screen and staying there. It redraws
once a second until you press `q`.

```
octomancer 0.1.0         09:17:06
  octomancerd      answering, up 12h48m
  octomancer-sync  answering, up 12h48m

DEVICE            AGE     OFFSET LINK         RSSI
BMPCC              0s     +8.8ms on the air    -52
F55                1s     -1.2ms on the air    -80
Krysta             1s     +0.0ms on the air    -72
FS5             2m25s         -- off the air    -80
FS7             2h47m         -- off the air    -83

q to quit
```

The reason to have it is that a bench moves. A box drifts, a camera goes quiet,
a sync lands — and `octomancer status` answers about the instant it was run and
then scrolls away. Watching a jam take hold with `status` means running it over
and over and reading a different set of lines each time, which is a slow way to
watch a number stop changing.

The version and the two daemon lines are on this page and not on the one-shot
one, and that is not an inconsistency. `status` hides them because four lines
of preamble in front of the table somebody ran the command for is how a status
page stops being read; here nothing scrolls past, and after the first glance
they cost a reader nothing. What they buy is that the two facts most likely to
explain a table with half the room missing are already on screen at the moment
it happens.

The clock in the corner is the one thing on the page guaranteed to change every
second. That is the whole reason it is there: a quiet bench and a wedged
program look identical otherwise, and only one of them is worth knowing about.

A window too short for the page keeps the footer and says what it dropped —
`... 6 more lines than this window has room for`. The footer is the way out,
and a person whose terminal has been taken over by a table needs the way out
more than they need the last two devices; a table that quietly ended early
would read as a bench with fewer boxes in it. `Ctrl-L` redraws, `Ctrl-C` and
`Ctrl-D` leave in case the footer went unread, and every other key does
nothing, because this page has one verb.

It needs a terminal on both ends and says so if it does not have one. There is
no piped mode: `octomancer status` is that mode, and it is already written.

### The app

`Octomancer.app` is the same set of controls with a window. Two lines across the
top say what the sync daemon is doing and how the bench looks, because those are
the only things worth seeing without having chosen to look at them; under them
are four tabs.

**Devices** is the merged list — a row per device, how long ago it was heard,
how far it is from the canonical time, and what its link is doing. It is the
same view `octomancer status` prints, out of the same code: `build_device_view()`
in `src/devices.h` decides which devices are on the list and what the numbers on
them mean, and the terminal and the window are each left drawing the result. Two
programs answering that question separately would answer it differently
eventually, and on the day they disagreed neither would be obviously the wrong
one.

**Details** is the page about one device at a time, in two halves with the same
shape: a picker, the readings, then the settings. Somebody should not have to
learn two layouts to ask the same question about two kinds of device.

The camera half has the live figures, a timecode-source picker, and a **Jam
Sync** button — the operation's real name, and this window is the one place the
program speaks to the people who do it rather than about them. The timecode-box
half has what an advertisement carries and nothing else: its timecode, what it
is counting in, how far it is from the canonical time, how far it is from this
Mac, its drift, its signal and when it was last heard. It has no controls
because there are none to give — nothing here can set a Tentacle's clock, only
the Tentacle app can re-jam one, and the page says so rather than leaving the
gap to be read as unfinished.

Both halves carry the same two settings: **Enabled**, which is `writes` for a
camera and `enabled` for a box, and **Warn if out of sync**, the same `warn` the
command line sets. They sit under the readings they explain, because "why will
this not sync?" and "let it sync" should be the same glance. A switched-off
device has its warn box greyed rather than cleared: it could not warn about
anything while it is off the list, but what somebody asked for is still in the
file and comes back with it.

Both pickers list every device this Mac knows of — from the two daemons *and*
from `cameras.conf` — whether or not anything is hearing it. The file is the
half that matters: a device somebody switched off has stopped appearing anywhere
else, so a picker built from the merged view would have no entry for the one
device somebody most likely came here to switch back on.

**Remove** is on both halves and is not the same thing as switching a device
off, which is the reason it asks first. Switching off is a decision that gets
remembered, and remembering it is exactly what keeps a device on the page
forever. Remove deletes the memory instead — the settings, and whatever the
owning daemon had learned: an hour of drift history for a box, a camera body's
RTC bias and apply delay. Nothing is blacklisted, so a device still switched on
and in range comes back at its defaults, which the alert says out loud. For a
camera it also says the one part octomancer cannot do: the Bluetooth pairing is
undone in the camera's own setup menu and in System Settings ▸ Bluetooth, not
here. `octomancer remove --box NAME` and `--camera ID` do the same thing from a
terminal.

**System** is what is true of the whole installation rather than of any one
device: **Start**, **Stop** and **Restart** for the daemons, what launchd
currently thinks of each and how long each has been up, **Start at boot**, and
pairing.

**Pair Camera...** opens a sheet that runs `octomancer-sync`'s
own scan and pair, and shows what the tool says as it goes, verdict and all.
The six digits are on the camera's screen and macOS asks for them; nothing in
this window ever sees them. If the sync daemon is running the sheet says so
before anything starts, with a button to stop it: a camera takes one connection
at a time, and pairing alongside a daemon that connects on its own usually works
and, when it does not, fails as "could not connect" — the least informative
outcome there is, and the easiest to read as the wrong problem.

**Notifications** is where the switches live. It can notify you when:

* a sync fails,
* a camera syncs for the first time,
* a camera drops off the air,
* the timecode boxes disagree with each other — the bench failing to be one
  bench, which no amount of syncing against it can fix,
* the bench drifts away from this Mac, in ppm, since the absolute offset between
  timecode-of-day and a wall clock is a constant with no meaning and only its
  rate of change says anything.

Each is separately switchable, because which of those is worth interrupting
someone for is a matter of taste and not something the daemon should decide on
their behalf. The camera ones are events the daemon emits regardless and the app
filters, so turning one off costs nothing and turning it back on loses nothing
but the backlog.

The menu-bar icon can be hidden, and its switch is on that same page because it
is the same question — how much of itself this process puts on screen. The icon
is a shortcut to the window, not the program, and someone driving all this from
the command line has no use for it. Hidden, the app keeps running and keeps
notifying (posting a notification needs a bundled app, so this process is the
only thing here that can); opening Octomancer.app again brings the window back.

The icon is one circle, always, and only its colour changes: ordinary when
nothing is wrong, **yellow** when a device somebody asked about has gone quiet
for too long to have an opinion about, **red** when one of them is out of sync,
and greyed when neither daemon is answering. That last one is kept a colour of
its own rather than folded into red, because it is a thing to fix on this Mac
rather than at the cameras.

Ordinary is the system label colour rather than literally white. The menu bar
follows the system appearance, so white is only white half the time and is
invisible the other half.

Two earlier versions of this icon are worth naming, because both looked like
information and were not. It carried a count of the boxes being heard: five
boxes on the bench reads as 5 whether all five are jammed to each other or one
of them left the building an hour ago. Then it was a clock glyph that grew a
second dot beside it when something was wrong — which changed the icon's width,
so the menu bar shuffled every time a box went quiet, and which asked somebody
to read two symbols to learn one thing. "Is anything wrong" is the question
somebody glancing up actually has, and a colour answers it in the space of a
full stop.

The menu has two ways out, because there were two things behind the one that
used to be there and only one of them was quitting. **Quit the Menu Bar App**
closes this process: notifications stop, and nothing else changes — the daemons
hold the clocks and do not care whether anybody is watching. **Stop Octomancer
and its Daemons** is the other thing, asks first, and says in the asking that
nothing will be corrected until they are started again — and that they stay
installed, so with **Start at boot** on they are back at the next login. If a
daemon refuses to stop it says so and stays up rather than quitting and leaving
somebody believing it had.

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
already keeps an hour of history per timecode box and takes proper medians
across it, which is a far better number than anything a few seconds of
listening can produce. Failing that it listens for itself.

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

## Two radios

There is a second radio now, and it is optional. Machines without one carry on
exactly as before.

CoreBluetooth is what every Mac already has and needs no hardware anyone has to
remember to plug in. An **nRF52840 dongle**, driven over raw HCI, can do things
CoreBluetooth will not: put exact bytes in an advertisement, say what it
actually transmitted, and act as a peripheral on somebody else's terms. That
last one is why it exists — see `doc/zoom-bta1-notes.md` for a week spent
losing to `CBPeripheralManager`.

The choice is made at run time and defaults to whichever radio is present:

```
octomancerd                        # dongle if one is plugged in, else CoreBluetooth
octomancerd --radio corebluetooth  # or insist
octomancer-sync --dongle /dev/cu.usbmodem1101
```

`OCTOMANCER_RADIO`, `OCTOMANCER_DONGLE` and `OCTOMANCER_HCI_TRACE` do the same
from the environment, which is what the launchd agents need.

Two differences are real and cannot be papered over:

* **Device identifiers change.** CoreBluetooth gives an opaque per-host UUID;
  HCI gives the real Bluetooth address. A bench learned over one radio is not
  recognised over the other.
* **The camera has to pair.** Over CoreBluetooth it was already bonded and
  everything worked immediately. The dongle arrives as a stranger, so the
  camera displays a six-digit passkey — pass it with `--passkey`.

**Today the dongle can watch but not act.** Scanning over it works and is what
the daemon and the window use. Setting a clock over it does not: that half was
rebuilt on the event loop for the standalone box — see `doc/box-notes.md` — and
the program that drives it is not written yet, so `--radio=dongle` with
`octomancer --set` says so and stops rather than appearing to work. Use
`--radio=corebluetooth` to write a clock. Nothing is lost that ever worked;
`doc/dongle-notes.md` records that writing a clock over a dongle has never been
run against hardware at all.

### Which dongle to buy

The one this was developed against is a **Raytac MDBT50Q-CX-40**, bought from
[Amazon](https://www.amazon.com/dp/B0DP6MVDZQ), listed as "MDBT50Q-CX Nordic
nRF52840 Dongle Development Kit for Bluetooth Zigbee Thread (USB Type
C/Open Bootloader Pre-Loaded) BT5.4 FCC IC CE Pre-Certified". Other nRF52840
dongles should work; nothing here is Raytac-specific. Three things in that
listing are the ones that matter:

* **nRF52840.** The firmware is built for Zephyr's `nrf52840dongle` board. An
  nRF52832 is a different chip and will not take this image.
* **Open Bootloader pre-loaded.** This is the line to look for, and the one
  most easily skimmed past. It is what lets you program the dongle over the
  USB port with nothing but a cable. A dongle without it needs an SWD debug
  probe and a soldering iron before it will accept anything at all.
* **A button.** Entering firmware-update mode needs one. Dongles with no
  button exist and are not usable here.

USB-A or USB-C makes no difference to anything but which port it goes in.

What to avoid is a dongle carrying a **UF2 bootloader** — the Adafruit and
Makerdiary boards ship this way. UF2 is a perfectly good bootloader, but it is
a different mechanism: the dongle appears as a disk and you copy a `.uf2` file
onto it, which is not what `tools/flash-dongle.sh` does.

### Getting firmware onto it

The dongle has to be put into DFU mode by hand, and **the procedure depends on
the board**, which is worth knowing before you spend an afternoon pressing the
wrong thing:

* **One button (Raytac, and most third-party dongles).** Unplug it. Press and
  hold the button, plug it in while still holding, keep holding for about a
  second after it seats, then release. The last step is the one that gets
  missed -- letting go as it goes in just starts the existing firmware again.
* **Two buttons (Nordic's own PCA10059).** Press the small sideways-mounted
  RESET button while it stays plugged in.

Either way the LED starts a slow organic pulse -- a breath rather than a blink
-- and the dongle re-enumerates as `Open DFU Bootloader`. You can confirm it
without guessing:

```
ioreg -l -w 0 | grep -A4 Nordic     # "Open DFU Bootloader" means ready
```

Then:

```
tools/flash-dongle.sh --setup                  # once; fetches Zephyr and a compiler
tools/flash-dongle.sh --build
tools/flash-dongle.sh --package third_party/build-hci/zephyr/zephyr.hex
tools/flash-dongle.sh --flash  third_party/build-hci/zephyr/hci_uart_dfu.zip
```

`--build` defaults to the board this project owns. **If yours is a different
dongle, name its Zephyr board target** -- they are under
`third_party/zephyr/boards`:

```
tools/flash-dongle.sh --build nordic/nrf52840dongle/nrf52840
```

This matters more than it looks. Every nRF52840 dongle is the same chip, so an
image built for the wrong board flashes, verifies and reports success, and then
the dongle never appears on USB again -- which is indistinguishable from an
empty port. Board files differ in how they set the power regulators, and
Nordic's dongle switches the core supply to DC/DC, which needs inductors other
modules do not have. Get it wrong and the chip browns out during boot, before
USB or any LED. `tools/flash-dongle.sh --info` will tell you where the
bootloader lives, which identifies the board: Nordic at 0xE0000, Raytac at
0xF4000.

Unplug and replug afterwards, then `octomancer-zoom --scan 10` is the test that
everything works. It should list every advertiser in the room.

`doc/dongle-notes.md` covers flashing the dongle, what is tested without
hardware and what is not, and `octomancer-zoom`, the Zoom BTA-1 bench.

The nRF52840 is a whole computer, not just a radio, so there is a third thing
it could be: the program itself, running with no Mac anywhere. `doc/standalone-notes.md`
is the plan for that -- what crosses over unchanged (nearly all of the
decision-making), what has to be built (bond storage, a control channel, and
firmware update over USB and Bluetooth), and the one experiment that has to
succeed before any of it is worth starting.

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
