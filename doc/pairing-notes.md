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
goes and negotiates it the moment one of them is touched. So `--pair` connects,
subscribes, and waits.

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

## What is tested, and what is not

| Test | What it pins |
|---|---|
| `test_pairing` | the verdict for every combination of observations the radio can produce; that silence after a good connection is read as "not paired" rather than as "nothing to report"; that an error naming authentication outranks the inferred verdicts; the matcher against the phrasings CoreBluetooth and the ATT layer each use; and that the advice for the silent case says the code is on the camera. |

Run against real hardware, and confirmed:

* `octomancer scan` and `octomancer scan --all` against a live radio: 34 to 38
  LE devices, four of them Tentacles, the camera absent.
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
* **The macOS pairing dialog has not been seen for this camera.** That it
  appears at all when CoreBluetooth touches an encrypted Blackmagic
  characteristic is an assumption, not an observation. If no dialog ever
  appears, this is where the work is: `src/smp.cc` already implements legacy
  pairing with a supplied passkey for the dongle, and the answer may be to
  drive the bond from there rather than through CoreBluetooth.
* **The `refused` verdict has never been produced by a radio.** Its matcher is
  tested against strings written from the documentation, not against strings a
  camera caused.
* **The 90-second default is a guess.** It is long enough to read a number off
  a screen and type it, and nothing more principled than that.
