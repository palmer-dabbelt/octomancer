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
hang. Those tools block by construction and there is nothing left for them to
block on; the daemon is the answer for them too, and it is not yet their
answer.

**What would settle the rest of it:** a camera, switched on, and one cycle.

### The daemon and the scanner cannot share one dongle

`hci::Link` has one closed handler, one ATT handler and one SMP handler, so
the scanner and the camera cannot both attach to a link. Each therefore opens
its own -- over the same serial port, which macOS permits -- and the two read
the same byte stream until it collapses, reported as the radio powering off.

On a Mac this is worked around by giving each radio one job: `--radio dongle`
listens and has no camera, `--radio corebluetooth` listens on this Mac and
leaves the dongle for the camera. On the box there is one radio and no
workaround, which is why `doc/TODO.md` now opens with it.

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
