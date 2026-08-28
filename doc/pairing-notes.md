# Pairing

## The bond nobody made

For two days this program drove a Pocket Cinema Camera 6K Pro without ever
having paired with it. It worked because the Blackmagic firmware updater had
bonded the camera to this Mac at some earlier point, and CoreBluetooth kept the
key; `doc/dongle-notes.md` records the same observation from the other
direction, that the control characteristics "worked because the Mac had bonded
with that camera long ago".

Nothing in octomancer created that bond, and nothing in octomancer could. On
2026-08-27 it was cleared from the Mac -- and then, to match, from the camera --
on the entirely reasonable theory that a stale half-bond was what had stopped
the camera advertising. That was correct, and the camera did come back on the
air. What it also did was remove the last copy of a key no part of this program
knew how to replace.

## Why it looks like nothing at all

The failure mode is silence, which is the expensive kind.

The Blackmagic control characteristics are encrypted. An unbonded peer that
subscribes to them is not refused: the connection succeeds, the subscription
succeeds, and then nothing ever arrives. Every layer a program normally checks
reports success. The only visible symptom is an absence, and that same absence
is also what a camera parked in Clip mode looks like, and what a camera with
its timecode stopped looks like.

`octomancer-sync` reported it as `camera connected but sent no timecode`, once
a minute, for as long as it was left running. That line is true and was not
enough to act on, which is the whole reason `src/pairing.h` exists: the job is
not connecting, it is telling four indistinguishable outcomes apart, and that
is decision-making, so it belongs away from the radio where it can be tested.

Worth knowing while reading `src/camera_mac.mm`: the CoreBluetooth delegate
drops characteristic-discovery and notification-state errors on the floor
(`if (error != nil) return;`). If macOS ever does report an authentication
failure there, it is currently discarded before anything can see it.

## macOS does the pairing, not this program

There is no CoreBluetooth call that means "bond with this peripheral". There
are only characteristics that require encryption and an operating system that
negotiates it when one of them is *read or written* -- and that distinction is
the whole story here.

**Subscribing is not enough.** The first version of `--pair` connected,
subscribed, and waited, on the assumption that touching an encrypted
characteristic in any way would make macOS go and get a key. It does not.
Setting notify on a characteristic writes to its descriptor, and against this
camera that completes without encryption ever being negotiated. Nothing asked,
so nothing was offered: no dialog on the Mac, no code on the camera, and a
connection that then sat there producing nothing for ninety seconds. That
matched the symptom exactly and was still the wrong diagnosis.

**Reading is what triggers it.** Of the four characteristics in
`doc/protocol-notes.md`, Camera Status (`7fe8691d-...`) is the only one marked
`read`. `CameraLink::read_status()` exists for that reason, and `--pair` calls
it with the full deadline. It is also worth having for its own sake: the value
is a bitfield whose `0x04` bit is the camera's own opinion of whether it is
paired, so the question gets answered rather than inferred.

A related bug this uncovered: `camera_mac.mm` discarded every error handed to
`didUpdateValueForCharacteristic:` with a bare `if (error != nil) return;`. An
authentication failure -- the one error that would have named the problem on
day one -- was being thrown away before anything could see it. The status read
now reports its errors; the notify paths still drop theirs, which is a smaller
version of the same mistake and is worth fixing next.

Two consequences that are easy to get wrong:

* **The six-digit code is displayed on the camera.** macOS asks for a number;
  the number is on the camera's own screen. Someone watching the Mac for a code
  to appear will wait forever.
* **It has to run in the foreground.** A LaunchAgent has no session in which to
  put up a pairing dialog, which is why `octomancer pair` is a command you run
  rather than something the sync daemon does for itself when it notices it
  cannot read anything.

A third, less obvious: a BLE peripheral takes one connection at a time, and
`octomancer-sync` connects to the camera whenever it sees one. Pairing while it
is running fails as "could not connect", which is the least informative of the
four verdicts. `octomancer pair` says so up front when it finds the daemon
running; stopping it with `octomancer stop --daemon sync` is the fix.

## Holding the connection

Pairing succeeded and then the camera went quiet again, because the daemon
disconnected at the end of the cycle and there is no bond storage: the next
connection pairs from scratch. A key that only lasts as long as one cycle is
not much of a key, and it makes the daemon unrunnable unattended -- every
reconnection would want somebody to read six digits off a camera.

So `octomancer-sync` now keeps the connection open between cycles. `--no-hold`
restores the old behaviour. Three consequences worth knowing:

* **`subscribe()` must not be called twice on one connection**, so there is now
  a `subscribed()` on the seam and the cycle asks rather than remembers. It has
  to ask, because the camera can drop the link at any moment and a remembered
  "yes" would then be wrong in the direction that silently produces no
  timecode.
* **A held camera stops advertising**, so `octomancerd` reports it absent for
  as long as it is working. The main loop counts its own live connection as
  presence; without that, holding would make the daemon stop scheduling cycles
  and only wake for a blind check every fifteen minutes.
* **The timecode keeps arriving between cycles**, which is most of the point. A
  cycle that opens with a reading already in hand does not spend its first
  seconds waiting for one.

## What is tested, and what is not

| Test | What it pins |
|---|---|
| `test_pairing` | the verdict for every combination of observations the radio can produce; that silence after a good connection is read as "not paired" rather than as "nothing to report"; that an error naming authentication outranks the inferred verdicts; the matcher against the phrasings CoreBluetooth and the ATT layer each use; and that the advice for the silent case says the code is on the camera. |

Run against real hardware, and confirmed:

* `octomancer scan` and `octomancer scan --all` against a live radio: 34 to 38
  LE devices, four of them Tentacles.
* That cameras are now printed as they are found rather than at the end of the
  scan: two devices matching a name hint appeared at two and four seconds into
  a twenty-second scan.
* The `not-connected` verdict and its advice, and the warning about the sync
  daemon holding the camera.
* The `silent` condition itself, though observed by the sync daemon rather than
  by `--pair`: the camera advertised at 19:30, accepted a connection, and sent
  nothing.

What has **not** been verified, and should not be read as working:

* **No successful pairing has been observed.** The `bonded` verdict has never
  been returned by real hardware. Everything downstream of a passkey being
  accepted -- that encryption actually comes up, that the characteristics then
  answer, that the bond survives a reconnection -- is untested.
* **The macOS pairing dialog has still not been seen.** It is now known that
  *subscribing* will not produce one, which is why the read exists; whether
  *reading* produces one against this camera is the next thing to find out and
  has not been observed. If it does not, the work is `src/smp.cc`, which
  already implements legacy pairing with a supplied passkey for the dongle:
  the answer would be to drive the bond from there rather than through
  CoreBluetooth.
* **`read_status` is not implemented for the dongle backend.** It returns an
  error saying so. That wants doing alongside the SMP work rather than as a
  stub that half answers.
* **The `refused` verdict has never been produced by a radio.** Its matcher is
  tested against strings written from the documentation, not against strings a
  camera caused.
* **Holding has not been observed against hardware.** The code path is
  exercised and the daemon runs, but every attempt to watch it hold a real
  camera across two cycles has run into a camera that was not advertising at
  the time. That it reconnects cleanly when the camera does drop the link, and
  that a second cycle really does skip the scan, are both unwatched.
* **The 90-second default is a guess.** It is long enough to read a number off
  a screen and type it, and nothing more principled than that.
