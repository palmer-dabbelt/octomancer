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

## The three layers, and where the code is not them yet

`doc/box-notes.md` has the model, and the line it divides on is **latency**:
a **sync daemon** at the bottom doing the timing and nothing else, a **control
daemon** in the middle doing everything expensive that can afford to wait, and
**interfaces** on top. Both daemons may hold a radio -- the control daemon
needs one to reach a sync daemon that is a dongle somewhere other than a USB
port. What separates them is that the sync daemon is the only thing that ever
speaks to a timecode box or a camera.

This is the list of places the code disagrees with that, written out so they
can be worked through rather than rediscovered one at a time.

A note on which file this belongs in, because the boundary matters. The control
daemon *not existing* is work that has not been done, and it lives in
`doc/TODO.md` as item 2. What is below is the other thing: code that exists,
runs, and actively contradicts the model -- a daemon listening to timecode
boxes that should not be, a connection pointing the wrong way, two protocols
that cannot be bridged without inventing state. Those are not absences. They
have to be *un*built, and each one is something somebody will trip over while
building layer 2.

They are roughly in dependency order. The first eight are the shape -- where
the code and the model disagree about what each half is for. Then the things
that will bite whoever writes the bridge between them, and at the end two about
what the interfaces show, which is where the layering finally becomes visible
to somebody who does not read this file.

### 1. `octomancerd` points its radio at the timecode boxes

`octomancerd` keeps a radio in the target design -- that is how it reaches a
sync daemon which is a dongle somewhere other than a USB port -- so this is not
about taking the radio away. It is about what the radio is *for*. Today
`octomancerd` builds a `ScanBridge`, wires advert, camera and state sinks into
a `Registry`, calls `make_ble_scanner()`, and exits 1 if the scanner will not
start: it is listening to timecode boxes itself, which is the one thing layer 2
must never do.

So the change is a substitution rather than a removal. The scanner comes out
and the roster stops being its own -- there is already a `Registry` inside
`SyncDaemon` -- and what arrives instead is the sync daemon's state broadcast,
over whichever of the three transports that daemon is on. The radio stays, and
gets its real job: being one of those three transports.

Everything `octomancerd` reports today has to start coming from below it rather
than off the air.

**What would settle it:** `octomancerd` serving a full and correct roster while
every timecode box in the room is invisible to it directly -- because the only
thing it is listening to is a sync daemon.

### 2. The connection between the daemons runs the wrong way

The model has status flowing up, sync → control, over a connection the control
daemon opens. Today the only inter-daemon connection is the reverse: the legacy
`octomancer-sync` calls `octo::fetch()` against **octomancerd's** socket to ask
for the bench and for whether the camera is on the air. `octomancerd` never
dials out at all -- it includes `client.h` and calls nothing from it.

Reversing this is most of what layer 2 is. It is listed separately from the
entry above because it is a separate mistake: even after `octomancerd` loses
its radio, something still has to make it the *initiator*, and nothing in
either daemon is written that way today.

**What would settle it:** `octomancer-sync` with no `--socket` handling at all,
and `octomancerd` holding an open connection it opened itself.

### 3. Every interface that shows the merged list opens two sockets

`octomancer` holds `octomancerd.sock` and `octomancer-sync.sock` at once,
queries both, and merges the answers with `build_device_view()` from
`src/devices.h`. So does the TUI. So does `Octomancer.app`. The merge that
belongs on the daemon side is in the client, three times.

`octomancerctl` is the exception and only because it shows nothing about
cameras: it opens `octomancerd.sock` alone and survives unchanged.

The choice of which daemon to ask is made in the client too, at half a dozen
call sites, and in `remove` it is made per device -- a camera's removal goes to
one socket and a timecode box's to the other, in a ternary. All of that
branching goes away with the second socket.

**What would settle it:** `build_device_view()` called in exactly one place,
inside the control daemon, and every front-end holding one socket path.

### 4. Nothing speaks the box protocol, and launchd starts the wrong mode

`octomancer-sync --daemon` serves `octomancer-syncd.sock` in `src/boxmsg.h`'s
language. Outside `tests/`, nothing in the tree is a client of it -- the only
things that speak it are `tests/test_boxmsg.cc` for the codec,
`tests/test_boxsock.cc` for the transport, and `tests/test_syncd.cc`, whose
`FakePeer` is the one that actually exercises the vocabulary. The shipped
LaunchAgent runs `--poll 60`, the legacy mode.

Be exact about what this blocks, because it is easy to overstate. It does
**not** block the hardware verification the rest of this file waits on: the
daemon schedules its own cycles and writes each to the console and the log, so
running it in a terminal with a camera switched on settles those with no client
involved. What is missing there is a camera. What this blocks is everything
else -- the daemon cannot be asked what it thinks, told to do anything, or
configured, except by typing lines into a socket by hand.

**What would settle it:** anything a person runs printing something it learned
over `octomancer-syncd.sock`. Even a one-verb debug client would do it, and
would be worth having before the control daemon rather than after.

### 5. The state broadcast is a poll, and the wrong shape

The model has the sync daemon saying what it knows, unasked, once a second:
per device, how long ago it was last seen, the averaged offset and signal
strength, its pairing state, and the last four exact measurements. Much of that
exists in `src/syncd.cc`'s `dev` line, which already carries `id`, `name`,
`rssi`, `live`, `age`, `offset`, `median`, `samples` and `ppm`. Four things are
wrong with it.

It is **a reply, not a broadcast**. `dev` lines come back from `devices`, so a
control daemon has to ask, on a timer it picks, and anything that happened
between two asks is gone. What is volunteered instead is `bench`, carrying the
merged offset, its spread and a count of the boxes that went into it -- the
wrong granularity in both directions. It is the answer rather than the
observations, and it is one line about the bench as a whole rather than a line
about each device, so a control daemon cannot see which box moved.

The **averaging is right but incomplete**. `median` is the number the daemon
actually synchronises on and is exactly what a person should be shown, so it
belongs in the broadcast -- an earlier version of this entry argued the
opposite and was wrong. What is missing beside it is the averaged RSSI, and the
window of individual sightings that a log or somebody chasing a bad box wants.
`rssi` is the last sighting's, unaveraged, which is the one place the line
still shows a raw number where an averaged one is wanted.

`ppm` is **the one derived value that should move up**, and not because it is a
judgement: because it needs a lever arm. Drift is refused outright from less
than fifteen minutes of samples, and the box cannot hold fifteen minutes of
samples. `Registry`'s Mac defaults are an hour capped at 8192 two-double
samples -- 128 KB per device, on a part with 256 KB in total and five Tentacles
in a typical room. Layer 2 has been receiving the exact measurements all along
and can fit drift out of them; layer 3 cannot and should stop trying.

Which is why **the retention on the box is four samples, not a duration.**
`doc/box-notes.md` has the reasoning under "The rates, which are decided": four
is 64 bytes a device, and it is set by the redundancy the broadcast provides
rather than by what fits.

Note that this is the *broadcast* window and not the averaging window -- the
two are decoupled, and the `dev` line today conflates them by reporting one
`median` over one hour-long history. The averaging window produces the number
camera writes depend on and is computed on the box where nothing is lost; the
four samples are best-effort telemetry for the log. **A control daemon must
never recompute the first from the second**, because its copy has holes and the
box's does not. See "Two windows, and why both go over the wire".

Every sample goes out in four consecutive broadcasts, so losing a measurement
means losing four in a row. The trap to avoid while implementing that is that
the four must be **one per broadcast interval**, not the last four adverts. A
Tentacle advertises about seven times a second (measured 2026-08-29), so
keeping the last four raw adverts would drop three of
every seven and leave consecutive broadcasts sharing nothing at all, which is
the exact opposite of the intent.

It has **no sequence number**, so the Mac cannot tell a complete record from
one with a hole in it. The four-sample redundancy repairs losses of up to three
consecutive broadcasts, which means most gaps heal invisibly -- and that is
exactly why the counter is needed: nothing in the arriving data says whether
the healing happened. Call it `seq` rather than `id`, which already means three
things (entry 17), and make it wide enough that a wrap is not confusable with a
restart: at one broadcast a second, 16 bits wraps in eighteen hours.

It has **no pairing state**, which entry 7 is about.

**What would settle it:** a control daemon that never sends `devices` and still
has a complete, current roster, because everything arrives on its own -- and a
box whose per-device RAM is 64 bytes however long it has been running.

### 6. The control vocabulary is a different set from the one the box needs

The model has three things going down: `enable`/`disable device ID`,
`synchronise device ID now`, and `passcode for device ID`. What
`src/syncd.cc` serves is eight verbs -- `ping`, `hello`, `status`, `devices`,
`sync`, `source`, `announce`, `forget` -- of which half are polls that exist
because the peer was on the same machine and asking was easy.

The nearest equivalents do not line up. `sync` is `synchronise now`, roughly,
but takes `camera=` and `force=` rather than a device id. Nothing enables or
disables anything: enablement lives in `cameras.conf`, read from a filesystem
the box does not have (entry 12). `forget dev=` deletes a device rather than
disabling it, which is a different operation with a different lifetime.

This is not a rename job. `status` and `devices` should disappear entirely
rather than being translated, because their answers become the broadcast.

**What would settle it:** a sync daemon whose whole inbound vocabulary is three
verbs, and a control daemon that never polls.

### 7. There is no pairing flow that works without a person present

`HciCamera` takes a `PasskeyProvider`, and `octomancer-sync` installs one only
when `--passkey` was given on the command line; with no value it installs
nothing, `HciCamera` supplies one that always answers false, and pairing is
abandoned at Passkey Entry. `RadioOptions::prompt_for_passkey` is declared,
defaults to true, and is read by nothing.

So today the six digits have to be known *before* the daemon starts. On a Mac
that is merely awkward. On a box on a rig it is impossible: the camera displays
the digits at the moment it is asked, and there is nobody in front of the box
to read them or anywhere to type them.

The model routes it through the state broadcast instead. Pairing state is a
per-device field with three values -- **seen but not paired**, **pairing,
needs a passcode**, and **paired** -- and the middle one is a request that is
published rather than sent. The control daemon notices it, shows it to whatever
interfaces are attached, and one of them asks a person. The answer comes back
down as `passcode for device ID`. Nothing in the sync daemon knows a person
exists, and nothing is waiting on a reply that may never come.

This is also the argument for the broadcast being unsolicited: a request that
must be asked for cannot be seen by a control daemon that connected a second
too late, and pairing is exactly the case that starts while nobody is watching.

**What would settle it:** a camera paired over the dongle by somebody who
learned the digits from a UI, having never passed `--passkey`.

### 8. The logging is in the low-latency half

`run_daemon()` in `src/octomancer-sync.cc` opens the JSONL log and writes a
line at the end of every cycle, and opens the per-camera database and rewrites
it. Both belong in layer 2: they are the most complicated things the project
does -- rotation, compaction, a history that has to survive restarts -- and the
things that care least about being a few hundred milliseconds late. That
combination is the definition of what goes in the latency-tolerant half.

It is also the only half with a filesystem. The box has NVS and nothing else,
so the current arrangement is not merely misplaced; it is one of the parts that
cannot come along at all.

**What would settle it:** `octomancer-sync --daemon` running with no `--log`
and no camera database, and the same lines appearing on disk because layer 2
wrote them.

### 9. Two line protocols, and layer 2 sits on the seam

There are two framings over one token layer, carrying three vocabularies:

* `src/proto.h` -- banner, escaped `key=value` lines, `end`; one command, one
  whole reply, connection closed. Two unrelated vocabularies ride it:
  `src/proto.cc`'s registry snapshot, served by `octomancerd`, and
  `src/control.cc`'s camera-control surface, served by `octomancer-sync`. Both
  banner themselves `octomancer 1` while being mutually unintelligible.
* `src/boxmsg.h` -- one message per line on a connection that stays open, with
  announcements arriving unasked. Served by the sync daemon.

There is no translation layer anywhere, and the control daemon is where one
would have to live. `doc/box-notes.md` records the decision taken -- take
`boxmsg` all the way up and retire `proto` for daemon traffic -- but that is a
decision, not a fact, and until it is made the entries below are the bill for
the other choice.

### 10. `sync` and `source` cannot be relayed, only brokered

This is the one where translation is not a rename, and it is the reason the
control daemon has to hold state.

`src/control.cc` queues a `Request`, assigns it an id, returns it, and the
caller polls `result id=N` until it finishes -- which `src/octomancer.cc` does
every 500 ms, and so does the app. The box protocol has nothing like it:
`sync` answers `ok what=sync queued=0|1` with no handle, and the outcome
arrives later as a `cycle` announcement carrying no reference to the request
that caused it. The daemon's pending-request state is a single slot shared by
every peer, so two peers' requests cannot be told apart at all.

So layer 2 must invent the id, remember which peer asked, and correlate the
next `cycle` back to it -- and it cannot do that reliably against an untagged
broadcast. Guessing works until two clients ask about different cameras.

**The cheap half of the fix belongs in the sync daemon, not the bridge:** echo
the requesting message's `id=` on the resulting `cycle` line, and make the
pending-request state per-peer. Then the correlation is exact. Doing this
before the control daemon exists is much easier than after.

### 11. The two event channels are not one-for-one

`src/control.cc` keeps a numbered event list and answers `events since=N` by
replaying everything newer -- so a client that was not listening, or that
reconnected, catches up. The sync daemon's announcements are unnumbered and
unreplayable: a peer that connects late has simply missed whatever happened.

Layer 2 cannot synthesise the replayable version from the other without keeping
its own numbered log, which is a second place for the two to disagree about
what happened. Better to decide which set is canonical and make the sync daemon
emit that one.

### 12. No verb grants write permission, and the box has no other route

`src/syncd.h` says permission "will arrive over the control protocol", and
`SyncdOptions::default_writes` is the field waiting for it. There is no verb
that sets it -- the whole dispatch has eight verbs and none of them is about
permission -- and on the Mac the field is dead anyway, because `run_daemon()`
always installs a `CamConf` read from `cameras.conf` and that wins.

The box has no filesystem, so this is not a nicety: as things stand a Nordic
box has no way to be told which cameras it may write to, and the default
permits nothing. The verb is `enable device ID` / `disable device ID` from
entry 6, and this is the same missing thing seen from the other side -- entry 6
is that the vocabulary is wrong, this is what specifically breaks because of
it.

Where the answer *lives* is worth being exact about, because "who decides" and
"who stores it" are different questions. Layer 2 decides, and is where a person
changes it. Layer 3 writes what it was told into flash and then obeys it
without asking again, because **the box has to keep working when the Mac is
not there** -- a rig that stopped syncing because a laptop went to sleep would
be worse than no box at all. So the enabled set is pushed down and kept, not
looked up.

**What would settle it:** a sync daemon with no `CamConf` at all, told what it
may write to over the socket, on both platforms -- and still obeying it after a
power cycle with nothing connected.

### 13. `scan` and `pair` reach past the socket entirely

`octomancer scan` and `octomancer pair` `exec` the sibling `octomancer-sync`
binary; `Octomancer.app` runs it as an `NSTask`. Both do it for the same
reason: that binary is the one holding the Bluetooth grant.

This cannot survive a long-running sync daemon that already holds the radio --
the CLI has to print a note telling you to stop the agent first, which is the
symptom -- and on a Nordic box there is no sibling binary to launch at all.

Where each of the two goes is different, which is why this is separate from
entry 7. **Pairing** becomes the published-request flow there: a state in the
broadcast and a passcode coming back. **Scanning** does not become a verb at
all -- the sync daemon is always listening, so what a person actually wants is
the broadcast it is already sending, and a `scan` command has nothing left to
do. This entry is only about the subprocess: two front-ends that reach past
their own socket to run another program, and have to stop.

### 14. Three sockets, and one of them goes

`octomancerd.sock`, `octomancer-sync.sock`, `octomancer-syncd.sock`. Two
survive and do different jobs: `octomancerd.sock` becomes the one every
interface holds, because it has the launchd label and the muscle memory, and
`octomancer-syncd.sock` stays as the sync daemon's, which is what layer 2
connects *down* to. `doc/box-notes.md` takes that decision, and with it that
`octomancer-sync.sock` is retired along with the mode that serves it.

That retirement is not free: `src/control.cc`'s vocabulary has verbs
`octomancerd`'s does not -- `result`, `events`, `reload` -- and they have to be
folded in rather than dropped. `doc/TODO.md` item 3 is the cutover.

### 15. Two loops in the tree

`octomancerd` runs its own hand-rolled `poll()` loop and drains the scan bridge
by hand; a comment in it says so. Everything else new runs on `src/loop.{h,cc}`.
Two implementations of the same ordering arithmetic, one of them untested.

Worth doing while `octomancerd` is being taken apart for its radio anyway,
since both changes touch the same forty lines.

### 16. The two sync modes share one camera database

`octomancer-sync --daemon` takes a *different* lock file from the legacy mode,
deliberately, so the replacement can run beside the thing it replaces. It
defaults to the same per-camera database. So the arrangement the separate lock
exists to allow is exactly the pair of writers `README.md` and
`doc/service-notes.md` both say cannot happen: two processes holding a camera
and disagreeing about its learned bias.

Not a bug to fix so much as a cost to remember: pass `--camera-db PATH` to one
of them, and it goes away at the cutover.

### 17. Small protocol mismatches that will bite the bridge

Individually trivial, collectively the reason a translation table would be
permanent. Settle them *before* the control daemon is written, not after:

* **Three spellings of `forget`.** `forget <id>` as a bare positional with no
  escaping (`src/server.cc`), `forget camera=<id or name>` (`src/control.cc`),
  `forget dev=<id>` (`src/syncd.cc`). The first cannot carry an id that needs
  escaping at all.
* **`device`/`dev`, `camera`/`cam`, `error`/`err`** -- same things, two
  spellings each, split across the two framings.
* **`id` means three things.** A correlation tag on a boxmsg reply, a
  queued-request handle in `control.h`, and a device or camera identifier on
  `dev` and `cam` lines. Layer 2 adds a fourth use. Worth renaming something
  now.
* **The box protocol's version handshake is decorative.** Both `hello` lines
  carry `proto=1` and nothing anywhere compares it against
  `kBoxProtocolVersion`. `src/proto.cc` refuses a version it does not know;
  `src/control.cc` was doing only half the check until 2026-08-29 and now does
  the whole one. The box protocol still does none.
* **`octomancerd`'s non-status replies are not well-formed.** `ping`, `forget`
  and `error` emit the banner and one line with no `end`, while `proto.cc`'s
  own `parse_text` refuses any reply that has no `end`. It works today only
  because nothing parses those three: `octomancer ping` prints "ok" if the
  query returned at all and never looks at the text. A bridge that parsed
  everything uniformly would reject them.

### 18. `octomancer status --json` quietly omits the bench

The human-readable path asks both daemons and merges. The `--json` path sends
`json` to `octomancer-sync`'s socket alone, so the Tentacle bench is missing
from it and nothing says so. Both daemons answer `json` with their own
unrelated view, which is why it looks like it worked.

It goes away when there is one socket, but until then it is a wrong answer
rather than a missing feature, and `--json` is what anything scripted would
use.

### 19. Nothing records which sync daemon a device was heard by

`build_device_view()` in `src/devices.h` merges two daemons' answers into one
list of rows, and a `DeviceRow` has no idea where it came from. `DeviceView`
carries a single `canonical_source` for the bench as a whole, which is the
nearest thing to provenance in the tree and is one string for the whole table.

That is survivable today, with two daemons on one machine and one radio between
them. It stops being survivable the moment a dongle across the room is a source
too, for two reasons:

* **A person needs to know.** "This Mac can hear that box" and "something
  across the room can hear it and is telling me" are different situations, and
  the second has a link in it that can fail. The model puts that on the screen:
  the dongle gets its own row, and anything reached through it reads
  `NAME (via DONGLE)`.
* **The same box will appear twice.** Heard by a Mac-local sync daemon and by a
  dongle, one physical Tentacle produces two rows with different identifiers --
  CoreBluetooth's per-host UUID and the real Bluetooth address -- and nothing
  can tell they are the same device. Without a `(via …)` on each, that reads as
  a bug in the merge. With one, it reads as what it is.

Structurally: an origin on `DeviceRow`, and a third value in `DeviceKind`,
which today is `kTentacle` and `kCamera`. And the name has to reach the Mac in
the first place, which is the sync-daemon identity in `hello` -- see
`doc/box-notes.md`, where this is what promotes that from a recommendation to a
requirement.

**What would settle it:** `octomancer status` showing a dongle, its dropped
broadcast count, and at least one device listed as reached through it.

### And one piece of dead scaffolding

`HciCamera::open()` -- which opens a dongle for a camera that owns no other
radio -- has no callers. `HciCamera::attach()`, which takes a radio somebody
else owns, is what the daemon uses. `open()` is kept because it is the honest
entry point for a process that only wants a camera, but nothing has ever run
it, so it is untested in the way that only shows up the first time somebody
needs it.

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
