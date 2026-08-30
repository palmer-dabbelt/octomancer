# Known issues

What is wrong, or unproven, or known to be missing. Things that are merely not
built yet live in `doc/TODO.md`; this file is for the places where the program
does something questionable, or where something is believed to work without
anybody having watched it.

Each entry says what would settle it, because "needs investigation" ages into
"nobody remembers what the question was".

## The camera connection

### The link reporting has never been run against a camera

`octomancer-sync` now publishes what its own link is doing -- `connected` over
the control socket, which `octomancer status` and the window both show in the
LINK column as `held`, `on the air` or `off the air`. It logs the two
transitions, samples the held link's cached timecode on every presence tick so
a camera is watched even between writes, and spaces out its attempts to find a
camera that has gone away, starting at `--poll` and doubling to `--max-poll`.

All of that is glue over CoreBluetooth and none of it has been exercised with a
camera in the room. The one part with a test is `reacquire_interval()` in
`src/camsync.h`, which is arithmetic and knows nothing about radios: it says
what the wait should be after n failed looks, and says nothing about whether
the daemon counts those looks correctly or calls it at the right moments. The
obvious way for the rest to be wrong is `link->connected()` staying true after
the link has stopped carrying anything, which would publish `held` for a camera
that is not there -- the exact confusion this was built to remove, wearing the
opposite label.

**What would settle it:** switch the camera on within earshot with the daemon
running and read the console for `camera link held`. Then carry the camera out
of range and read for `camera link dropped`. Then bring it back, and check how
long the daemon took to notice against the interval the log says it was
waiting. The same three moments are in `octomancer-sync.jsonl` as `camera`
records with a `state` of `held` or `dropped`, timestamped, which is the
version worth keeping.

### Recovering from a dropped link, unattended, is unproven

Holding works while it lasts. What has never been watched is the camera going
away on its own -- a power cycle, a flat battery, someone carrying it out of
range -- and the daemon having to come back with nobody present.

The pessimistic version is that it cannot: if a reconnection needs pairing, it
needs a person to read six digits off the camera, and an unattended daemon has
nobody. Two observations point the other way. On 2026-08-27 a manual
`octomancer sync` connected and wrote successfully after an earlier disconnect,
without anything being re-paired. Later the same evening, with the link long
gone, macOS's own `system_profiler` still listed the camera
(0C:43:14:82:B5:CA) as a bonded device under "Not Connected" -- the bond
outliving the connection, which is what the optimistic reading requires and is
direct evidence for it. The bond is macOS's, not ours: `doc/dongle-notes.md`
says there is no bond storage, and that is true of *our* SMP implementation for
the dongle; it is not a statement about macOS and should not be read as one.

None of that is proof that an unattended reconnection actually happens. A bond
that survives is a necessary condition, not the event.

**What would settle it:** power-cycle the camera with the daemon running and
nobody touching anything, and watch for `camera link dropped` followed by
`camera link held` in the console and in `octomancer-sync.jsonl`, with the LINK
column in `octomancer status` going from `off the air` back to `held` without
anybody typing a passkey. Then leave it overnight and read the same three
things in the morning.

### "connected but sent no timecode" still happens

Less often than before pairing worked, but it has not gone away. On 2026-08-27,
with a paired and syncing camera: `could not verify: no timecode after the
write` at 20:29:36, and `camera connected but sent no timecode` at 20:29:51 --
both between cycles that wrote and verified correctly.

The verdict logic in `src/pairing.h` reads timecode silence as "not paired",
which was right when the cause was a missing bond and is misleading now that it
plainly has other causes.

**What would settle it:** half of it is now answerable from the log. When the
complaint next appears, look for a `camera link dropped` line near it and at
what the LINK column in `octomancer status` said at the time: a link that went
away and a link that stayed up and went quiet are different faults, and until
the link state was published they were the same line of output. If the link was
held throughout, what is still missing is the notification state per
characteristic -- which subscriptions were live, whether anything at all had
ever arrived on them, how long since the connection came up. With that,
`src/pairing.h` can distinguish "no bond" from "bonded and quiet" instead of
guessing.

### The camera goes off the air for long stretches

Repeatedly, across 2026-08-27, the camera stopped advertising for many minutes
and then returned with nothing having been done to it. Sometimes this was
explained -- something was holding a connection to it -- and sometimes nothing
was.

One stretch was measured properly, on the evening of 2026-08-27, and it settles
part of this. From about 20:34 the camera stopped advertising and did not come
back for at least an hour. `octomancer-sync` looked with the radio at 20:47:48
and again at 21:03:24; both times a direct connect to the camera's known
CoreBluetooth identifier timed out after 15 s, and both times a full 20 s scan
saw 35 to 37 other LE devices and no camera at all. A separately hand-run
`octomancer-sync --scan-only --all` at 20:56 saw 35 devices down to -84 dBm and
did not see the camera even in the unfiltered list -- that one matters most,
because it bypasses our own filtering entirely, so the absence is not something
we did to ourselves. And macOS's `system_profiler` listed the camera
(0C:43:14:82:B5:CA) under "Not Connected", still bonded with no link held, so
it was not a held link masquerading as absence either.

Three independent looks agree, so that instance was the camera genuinely not
reaching the Mac rather than a defect in the connection path. What made it
unreachable was not determined. Distance, Bluetooth switched off in the
camera's own setup menu, and another application holding the link are all
consistent with what was seen -- a phone running Blackmagic Camera will make
the camera stop advertising -- and none of the three was checked at the time.

Underneath all of it is the signal strength, which is genuinely odd. The camera
has never been heard stronger than -75 dBm in any log, including when it sat in
the same room as the Mac, where the timecode boxes read -51 to -55. Roughly
20 dB quieter than everything else in the rig means it is the first thing to
vanish whenever anything about the room changes.

**What would settle it:** log every advertisement from the camera with its
timestamp and RSSI, and look at whether the gaps line up with anything -- the
camera's own idle behaviour, its screen sleeping, our connections. And the next
time it happens, before anything is touched: look at the camera's Bluetooth
setting, look for a phone that is connected to it, and carry it to the Mac.

## The window

### Most of the interface has not been watched running

The app has four tabs -- Devices, Details, System, Notifications -- with the
Devices page drawing the same `build_device_view()` the terminal draws, the
Details page carrying both halves of the per-device view and every per-device
setting, and a sheet that drives `octomancer-sync --scan-only` and `--pair`
through `NSTask`. It compiles and it runs; the Devices and Details pages have
been seen with real data on them. Most of the rest has not been watched.

The pages used to be pinned straight into their tab, and an `NSView` does not
clip its subviews, so a page taller than the tab drew past the bottom of it --
over the window, and outside the rectangle AppKit invalidates when the selected
tab changes. The overflow was therefore never erased and two pages ended up
legibly on top of each other. Each page is in a scroll view now, which clips,
and the tab is sized from the tallest page measured at the width it will be
given. That was reported from a running window and the fix has been seen to
build; it has not been watched across every pair of tabs.

Four parts are reasoned about rather than observed. No page has been seen at a
size somebody dragged it to. The Devices grid keeps its row views across a
device disappearing and rebuilds only when the set of devices changes, so the
case it exists for -- a camera going and coming back without the page flinching
-- is the case nobody has watched. The pairing sheet reads the tool's output as
it arrives, parses the scan list out of it, and has to kill the child when the
sheet closes. And **Remove** has never been clicked.

**What would settle it:** open the window with both daemons running, switch
between all four tabs at a window height that makes a page scroll, watch the
Devices page for a few minutes while a timecode box is disabled and re-enabled
on Details and a camera comes and goes, then open the pairing sheet, search,
pair, and close the sheet mid-search -- and check with `ps` that no
`octomancer-sync` was left behind.

### Removing a device has not been done to a device that was there

`Registry::forget`, `CamDb::forget`, `CamConf::forget_camera` and
`forget_box` are each unit-tested, including that the deletion survives
reopening the camera database, which is the part an append-only log makes
awkward. The wire path has been exercised end to end from the terminal: a
`box test-fake` line was added to `cameras.conf` by hand, `octomancer remove
--box test-fake` deleted it and octomancerd answered the `forget`, and the
file came back byte-identical to what it had been.

What has not been done is removing a device that is actually in the registry,
because doing it on the bench in this room would delete a real box's drift
history -- hours of listening -- to prove something the unit test already
pins. So the erase-then-reappear cycle has been reasoned about, not watched.

**What would settle it:** on a rig whose history is expendable, remove a box
that is advertising, watch it vanish from `octomancer status` and come back
within a second or two with `samples` at 1 and no drift figure.

## Errors that are still discarded

`src/camera_mac.mm` throws away errors in four places: lines 243, 253, 262 and
284, covering service discovery, characteristic discovery, and notification
state changes. Only the Camera Status read reports what it is told.

This is not a hypothetical. It is exactly the bug that hid the pairing failure:
an authentication error, the one thing that would have named the problem
immediately, was being discarded before anything could see it, and the symptom
was two days of a camera that connected and then said nothing.

**What would settle it:** route them through the same path the status read now
uses, so a failure has somewhere to arrive.

## The dongle backend

### The dongle's camera half is driven, but only as far as the scan

The dongle's camera path used to implement `CameraLink` in `src/camera.h`,
which blocks by contract. It cannot any more: blocking there meant waiting on
the HCI reader thread, and that thread is gone because the box cannot have one.
The logic moved to `octo::HciCamera` in `src/camhci.h` -- the same work with
completion handlers instead of return values -- and for a while nothing called
it at all.

Something does now. `octomancer-sync --daemon` drives it through
`src/camasync.h`, and every cycle scans through it. That scan has run against
a real dongle in a room with 37 LE devices in it and correctly found no
Blackmagic camera, because there was none switched on. **Everything past the
scan has still never run against hardware**: connect, discover, subscribe,
pair, write. `doc/dongle-notes.md` is the table of which line of that is real.

`octomancer --set` and `octomancer-sync` in its older modes still print that
the camera half moved and return no link, rather than returning one that would
hang -- but only when the dongle was asked for by name. Since 2026-08-29
`--radio auto` no longer counts as asking (`src/radio_camera.cc:31`), so with a
dongle merely plugged in those tools quietly get CoreBluetooth instead; the
entry below on the restart loop is what that change was for. Either way they
block by construction and there is nothing left for them to block on; the
daemon is the answer for them too, and it is not yet their answer.

**What would settle the rest of it:** a camera, switched on, and one cycle.

### Sharing one dongle: fixed, and what is still unproven about it

*Was: the scanner and the camera each opened their own `hci::Link` over the
same serial port, which macOS permits, and the two read the same byte stream
until it collapsed -- reported as the radio powering off.*

`src/hcishare.h` is the fix. The daemon owns one link and hands out
subscriptions; scanning is reference counted, active beats passive, and the
scan is restored after a connection comes **up** rather than only after one
fails. `--radio dongle` now listens and drives the camera on the same radio,
which is what the box will have to do. Verified on 2026-08-29: four cycles,
one file descriptor on the port, and the Tentacle roster still ageing 0.1 s
during a camera scan.

That was only the half of it inside one process. Two processes on one dongle
was just as real and had no fix at all: `octomancer start` runs both agents,
and under `--radio auto` each takes the first dongle it finds, so `octomancerd`
and `octomancer-sync` would open the same `cu.*` device and read one byte
stream between them. macOS does not refuse the second open. The ProcLocks were
never going to help -- they are named per program, not per radio
(`default_lock_path("octomancerd")` at `src/octomancerd.cc:53`,
`default_lock_path("octomancer-sync")` at `src/octomancer-sync.cc:119`), so
each program takes its own lock, both succeed, and both then take the same
antenna. A lock on the program is not a lock on the hardware.

Fixed 2026-08-29 with `TIOCEXCL` on the port (`src/hciport_posix.cc:145`),
which is what a tty has for exactly this and costs one ioctl. Verified the same
day by starting a second daemon while the first held the dongle: it was refused
at open with `already open by another program`, rather than joining in. That
sentence is written out in `src/hciport_posix.cc` because `EBUSY` on a serial
port renders as "Device busy", which reads like a driver fault and is in fact
another octomancer. The ioctl is not checked, deliberately: a driver that
declines exclusivity leaves things exactly as they were before the line
existed. No such driver has been met **(unverified)**.

What is **not** verified is the half that needs a camera. No connection has
been made over the dongle, so "the scan comes back after a successful connect"
is pinned by `tests/test_hcishare.cc` against a scripted controller and by
nothing on real hardware. The same sentence as everywhere else in this file: a
camera, switched on, and one cycle would settle it.

The limitation that remains by design: ATT, SMP, advertising and raw commands
are *not* arbitrated. They are reached through `User::link()` and have one
owner by convention. Two things doing ATT on one link would collide silently.

### The LaunchAgent restart loop: fixed 2026-08-29, and why nobody saw it

Plugging a dongle into a Mac put the shipped `octomancer-sync` LaunchAgent into
a ten-second restart loop. `make_camera_link()` used to test
`dongle_selected()`, which is true under `--radio auto` the moment a dongle
appears on any USB port, and it returned null, because there is no blocking
camera client for the dongle and there is not going to be one. The agent runs
the blocking path with no `--radio` flag; that path exits 1 on a null link
(in `src/octomancer-sync.cc`); `KeepAlive`/`SuccessfulExit` false with a
`ThrottleInterval` of 10 brings it back. Forever.

Two things hid it. The symptom is an agent that always looks like it is
running -- `launchctl` shows a live pid every time, because there always is
one, just never the same one for long. And the line it printed was
`octomancer-sync: no CoreBluetooth on this host`, said on a Mac. That reads as
nonsense to be ignored rather than as a bug to be chased, and it was: the null
came from the dongle branch and the complaint came from the CoreBluetooth
branch, which had no idea why the link was missing.

It has not bitten on this machine, which is the part worth remembering, and
that is an observation about one Mac rather than about the bug. The link is
asked for once, before the poll loop, so the loop needs the dongle to be
present at the moment the agent starts -- and the agent starts at login while
the dongle has always gone in afterwards. The bug was reachable the whole time
and was kept away by the order two things happened in, which is not a defence.

Fixed by asking `dongle_requested()` instead (`src/radio_camera.cc`): auto
means "pick something that works", so auto picks CoreBluetooth and says
nothing, while `--radio dongle` still gets the honest refusal naming
`src/camhci.h` and `--daemon`. The lie went with it --
`octomancer-sync` now says "no CoreBluetooth" only when the dongle
was not the thing that refused.

Half of this was measured and half was not, so be exact about which. With the
dongle plugged in, `octomancer-sync --poll 60 --dry-run` and
`octomancer-sync --scan-only` both now run, where before the fix both exited 1;
`--radio dongle` still refuses and now names what to run instead. So the exit
that fed the loop is gone, watched rather than reasoned.

**What would settle the rest:** with a dongle plugged in, `launchctl bootout`
then `bootstrap` the agent and read its pid twice a minute apart. The same pid
is the answer. Nobody has watched the agent itself, before or after, so that
the loop existed at all is still reasoned from the plist and the exit path
**(unverified)**.

### `read_status()` is gone rather than stubbed

`HciCamera` has no `read_status()` at all. The old backend had one that
returned "not implemented"; carrying a stub across a rewrite dresses a gap up
as a feature, so it was left out. It still wants doing alongside the SMP
passkey work.

### Discovery was rewritten and has never run

`HciCamera::discover` walks the primary services, then the characteristics,
then the descriptors, exactly as the blocking version did -- but as a chain of
continuations rather than two nested loops. That is precisely the sort of
rewrite that compiles and is wrong, and unlike the HCI host underneath it, it
has no test. The round limits (16 services, 24 characteristic batches, 8
descriptor batches) are carried over unchanged.

**What would settle it:** point it at any BLE device with a GATT table and
compare what it finds against `octomancer-zoom --dump`, which walks the same
table by a different route.

### Other things

* `scan()` accepts the streaming callback but only calls it once the scan is
  over -- this backend classifies advertisements afterwards, so there is no
  moment during the scan when it knows it has found something. Truly streaming
  it means restructuring that loop.
* Everything in `doc/dongle-notes.md` under "What is tested, and what is not"
  still stands: the SMP key-derivation functions are not pinned to the
  specification's worked examples, LE Secure Connections is not implemented,
  and none of it has been run against an nRF52840.

## Nothing a person runs can talk to the sync daemon

`octomancer-sync --daemon` is real. It owns the radio, runs the cycle, and
serves `octomancer-syncd.sock` (`src/boxsock.cc:43`) in the message language of
`src/boxmsg.h`. Nothing anybody types speaks that language. `octomancer` and
Octomancer.app each open the two *other* sockets themselves -- octomancerd's
and the legacy control socket (`src/octomancer.cc:54`, `ui/main.mm:402`) -- and
merge the answers in the client, and the shipped LaunchAgent starts the legacy
mode rather than the daemon. The only clients of the box protocol in the tree
are tests: `tests/test_boxmsg.cc` for the codec, `tests/test_boxsock.cc` for
the transport, and -- the one that actually exercises the vocabulary --
`tests/test_syncd.cc`, whose `FakePeer` speaks real verbs at the daemon and
decodes what comes back.

The consequence is that the daemon is invisible. It cannot be asked what it
thinks, it cannot be told to do anything, and it cannot be configured, except
by typing lines into a socket by hand.

What this is *not* is the reason so many entries in this file end in "a camera,
switched on, and one cycle" and then stay open. Those stay open because there
is no camera switched on. The daemon schedules its own cycles and writes each
one to the console and the log, so running it in a terminal settles them with
no client involved -- which is how the four-cycle measurement two sections
above was taken. Blaming the missing layer for that would be a tidier story
than the true one, and the true one is already written down.

Not a defect in anything that was built. It is the layer that was never
started, showing up as an absence: `doc/TODO.md` item 2 is the missing daemon,
and item 3 is the cutover that follows it.

## Tests

* ~~**`test_proclock` is flaky.**~~ **Fixed 2026-08-29, and the diagnosis
  above was wrong.** It was not impatience and widening the window did not fix
  it -- the window had already been widened to thirty seconds and it still
  failed. The parent polled by *trying to acquire the lock itself*, and read
  "that failed, and the holder is the child" as the signal to proceed. That
  races with the child: when the parent's first probe wins, the child's own
  acquire is the one that fails, and the child does not retry, it exits. The
  parent then waits out its entire deadline for a holder that is never coming.
  The child now says when it holds the lock, down a pipe, and the parent waits
  for that byte. Measured across 120 runs started at once: two failures before,
  none after.

  Worth keeping as a cautionary entry rather than deleting. "The test is just
  impatient" is the comfortable diagnosis for every flaky test, it was written
  here in good faith, and it was wrong -- the test was genuinely broken and the
  first fix made the failure rarer and slower instead of removing it.
* **The `refused` verdict has never been produced by a radio.** Its matcher in
  `src/pairing.cc` is tested against strings written from documentation rather
  than strings a camera caused, so the words it looks for are a guess at how
  CoreBluetooth phrases things.
* **The 90-second pairing default is a guess.** Long enough to read a number
  off a screen and type it, and nothing more principled than that.

## Watch this

Not yet a bug, and possibly an artifact, but it was strange enough to write
down. On 2026-08-27, before the daemons were restarted, `octomancerd` reported
drift of 4429, 7101 and 15355 ppm across three timecode boxes, with a bench
offset of -89.8s and a spread of 63.8s. Those drift figures are two to three
orders of magnitude above anything a real clock does. The daemon had been
running across what was probably a Mac sleep, and after a restart the same
boxes read a -1.77s offset with 2ms of spread and no measurable drift, which is
healthy.

The likely explanation is that a sleeping host corrupts the lever arm the drift
measurement is built on. If so, `forget_drift()` should probably be triggered
by a wake as well as by a power cycle.

**What would settle it:** sleep the Mac deliberately with the daemon running,
wake it, and read the drift column.
