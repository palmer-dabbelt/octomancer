# Known issues

What is wrong, or unproven, or known to be missing. Things that are merely not
built yet live in `doc/TODO.md`; this file is for the places where the program
does something questionable, or where something is believed to work without
anybody having watched it.

Each entry says what would settle it, because "needs investigation" ages into
"nobody remembers what the question was".

## The camera connection

### Nothing reports whether the connection is being held

This is the one to fix first, because it blocks judging the two below it.

`octomancer-sync` now holds the camera connection between cycles, and there is
no way to see that from outside. `octomancer status` says `camera off the air`
in two completely different situations: the link is held, so the camera has
stopped advertising and octomancerd cannot see it -- which is success -- and
the camera is genuinely gone, which is not. The log has the same problem:
`camera came up -- syncing now` is driven by the presence signal, so it appears
whenever the camera advertises, and a Blackmagic camera is not required to stop
advertising while connected. Seeing that line does not prove the link dropped,
and not seeing it does not prove the link held.

Observed on 2026-08-27: a daemon with holding compiled in and enabled logged
`camera came up` at 20:29:41 and again at 20:32:45, three minutes apart, with
successful writes either side. Whether that is the link dropping and being
remade, or the camera advertising happily while still connected, cannot be
determined from anything the program currently emits.

**What would settle it:** have `octomancer-sync` report its own link state --
it is the only process that knows -- over the control socket, and have
`octomancer status` show "held" as distinct from "off the air". Log the
transitions: held, dropped by the camera, reconnected. Until that exists,
neither of the next two entries can be answered.

### Recovering from a dropped link, unattended, is unproven

Holding works while it lasts. What has never been watched is the camera going
away on its own -- a power cycle, a flat battery, someone carrying it out of
range -- and the daemon having to come back with nobody present.

The pessimistic version is that it cannot: if a reconnection needs pairing, it
needs a person to read six digits off the camera, and an unattended daemon has
nobody. But that is *not* established, and one observation points the other
way. On 2026-08-27 a manual `octomancer sync` connected and wrote successfully
after an earlier disconnect, without anything being re-paired, which suggests
CoreBluetooth is keeping the bond across connections the way it kept the
firmware updater's bond for days. `doc/dongle-notes.md` says there is no bond
storage, and that is true of *our* SMP implementation for the dongle; it is not
a statement about macOS, and it should not be read as one.

**What would settle it:** power-cycle the camera with the daemon running and
nobody touching anything, and see whether it comes back. Then leave it
overnight and see whether it is still synced in the morning.

### "connected but sent no timecode" still happens

Less often than before pairing worked, but it has not gone away. On 2026-08-27,
with a paired and syncing camera: `could not verify: no timecode after the
write` at 20:29:36, and `camera connected but sent no timecode` at 20:29:51 --
both between cycles that wrote and verified correctly.

The verdict logic in `src/pairing.h` reads timecode silence as "not paired",
which was right when the cause was a missing bond and is misleading now that it
plainly has other causes.

**What would settle it:** log the notification state per characteristic when
this fires -- which subscriptions were live, whether anything at all had
arrived on them, how long since the connection came up. Then `src/pairing.h`
can distinguish "no bond" from "bonded and quiet" instead of guessing.

### The camera goes off the air for long stretches

Repeatedly, across 2026-08-27, the camera stopped advertising for many minutes
and then returned with nothing having been done to it. Sometimes this was
explained -- something was holding a connection to it -- and sometimes nothing
was. Signal strength is not the answer: it read -76 to -79 dBm sitting next to
the Mac, weaker than Tentacles two rooms away.

**What would settle it:** log every advertisement from the camera with its
timestamp and RSSI, and look at whether the gaps line up with anything -- the
camera's own idle behaviour, its screen sleeping, our connections.

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

* `read_status()` returns an error saying it is not implemented. It wants doing
  alongside the SMP passkey work rather than as a stub that half answers.
* `scan()` accepts the streaming callback but only calls it once the scan is
  over -- this backend classifies advertisements afterwards, so there is no
  moment during the scan when it knows it has found something. Truly streaming
  it means restructuring that loop.
* Everything in `doc/dongle-notes.md` under "What is tested, and what is not"
  still stands: the SMP key-derivation functions are not pinned to the
  specification's worked examples, LE Secure Connections is not implemented,
  and none of it has been run against an nRF52840.

## Tests

* **`test_proclock` is flaky.** It forks a child and polls for about two
  seconds for it to take a lock. Under a seventeen-way parallel `make check` on
  a loaded machine the child does not always get scheduled in time. Observed
  failing twice on 2026-08-27 and passing alone both times. Widening the window
  would fix it; the test is not wrong, it is just impatient.
* **The `refused` verdict has never been produced by a radio.** Its matcher in
  `src/pairing.cc` is tested against strings written from documentation rather
  than strings a camera caused, so the words it looks for are a guess at how
  CoreBluetooth phrases things.
* **The 90-second pairing default is a guess.** Long enough to read a number
  off a screen and type it, and nothing more principled than that.

## Watch this

Not yet a bug, and possibly an artifact, but it was strange enough to write
down. On 2026-08-27, before the daemons were restarted, `octomancerd` reported
drift of 4429, 7101 and 15355 ppm across three Tentacles, with a bench offset
of -89.8s and a spread of 63.8s. Those drift figures are two to three orders of
magnitude above anything a real clock does. The daemon had been running across
what was probably a Mac sleep, and after a restart the same boxes read a -1.77s
offset with 2ms of spread and no measurable drift, which is healthy.

The likely explanation is that a sleeping host corrupts the lever arm the drift
measurement is built on. If so, `forget_drift()` should probably be triggered
by a wake as well as by a power cycle.

**What would settle it:** sleep the Mac deliberately with the daemon running,
wake it, and read the drift column.
