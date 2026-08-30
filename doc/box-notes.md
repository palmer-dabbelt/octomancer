# The box: one loop, one protocol, three transports

*Written 2026-08-29, and added to the same day when the sync daemon was
built: everything below that describes the daemon as future work has been
corrected in place, and the section "One radio, several jobs" is a thing running
it taught. No firmware exists yet. Everything below is either
measured on this machine and said so, checked against the source and cited, or
marked **(unverified)**. The Nordic has never held a camera link, never
advertised, and never been connected to — over the dongle only scanning has
ever worked, and `doc/dongle-notes.md` is careful about which line of that
table is real.*

This supersedes the planning half of `doc/standalone-notes.md`. That file's
measured facts and its research on the ESP32 fallback still stand; read it for
those and read this for the shape.

## The decision being reversed, first

`README.md` calls the split between `octomancerd` and `octomancer-sync` "the
main design decision in the project", and `doc/service-notes.md` spends a
paragraph on why conflating observing with acting is how an unattended service
does something surprising at three in the morning.

**That split is being dropped on purpose, and this paragraph exists so it is
dropped out loud rather than quietly deleted.** It was never a goal; it fell
out of reverse-engineering the camera protocol, when the safe thing was to
have a binary with no `connect` in it. The target now is a screenless box on a
rig that must do both — it hears Tentacles and it writes a camera's clock —
with one radio and one loop. A design where those are two executables cannot
run there at all. And having decided the box works that way, running a
*different* architecture on the Mac would mean the interesting code existed
twice and was debugged once.

What is actually given up should be written down. On macOS there were two
Bluetooth grants, one to listen and one to write to a camera, and the second
could be refused on its own. After this there is one prompt and one grant.
What protects a recording is no longer a binary that structurally cannot
write; it is `CamConf`'s default of writes-off for any camera nobody has
named, the gate order in `camsync.h`, and `--dry-run`. Conditional protection
where there used to be structural protection. `README.md` and
`doc/service-notes.md` both assert the old design as a virtue and will need
correcting as the churn lands.

## Three layers, and which of them exist

The daemons still divide, but along a different line: **a low-latency half that
does the timing, and a latency-tolerant half that does everything else.**

That is the whole principle, and it is worth stating before the diagram because
an earlier version of this section got it wrong. It said the split was about
which daemon owns a radio, and that the control daemon must own none. It is
not, and it must not be, for a plain reason: when the sync daemon is a dongle
sitting somewhere other than a USB port, the only way to reach it is over BLE,
and something on the Mac has to hold a radio to do that. What the control
daemon must never do is speak to a **timecode box or a camera**. Its radio, if
it has one, is pointed at a sync daemon and nothing else.

```
  LAYER 3 -- the low-latency half. One event loop, no threads.
             The only thing that ever speaks to a box or a camera.

   [Tentacle boxes] --adverts-->  SYNC DAEMON        `octomancer-sync --daemon`
   [Blackmagic cam] <---GATT--->  src/syncd.{h,cc}   as a Mac process, or as
                                                     Nordic firmware. The same
                                                     source and the same
                                                     design either way.
                                      |   ^
       state broadcast, once a        |   |   control, when somebody wants
       second, unasked. Per device:   |   |   something:
       how long ago it was seen,      |   |
       averaged offset and signal     |   |     enable / disable device ID
       strength, its pairing state,   |   |     synchronise device ID now
       and the last four exact        |   |     passcode for device ID
       measurements                   |   |
                                      v   |
             one connection per sync daemon, carrying both directions.
             src/boxmsg.h framing: one message per line, the broadcast
             arriving unasked. A unix socket when the sync daemon is a
             process on this Mac; USB CDC when it is a dongle in a port;
             BLE GATT when it is a dongle somewhere else.
                                      |   ^
                                      v   |

  LAYER 2 -- the latency-tolerant half. Mac only. `octomancerd`.

                            CONTROL DAEMON
                            Has a radio, and uses it for exactly one thing:
                            reaching a sync daemon that is not plugged in.
                            Never speaks to a timecode box or a camera.
                            Merges what the sync daemons report, keeps every
                            log, holds the permissions and the request state,
                            answers everything above it.
                                      |   ^
                                      v   |
                       one socket, one vocabulary, several
                       concurrent clients, none of them special
                                      |   ^
                                      v   |

  LAYER 1 -- user interface. Any number at once.

    octomancer CLI     octomancer tui     Octomancer.app     octomancerctl
```

**Layer 3 is the only thing that speaks to a box or a camera.** It hears the
Tentacle boxes, holds the camera link, runs the decision in `camsync.*`,
broadcasts what it has seen, and does what it is told. The same source is meant
to build as Nordic firmware and as a Mac process, and that is the whole point:
the box is debuggable without a box, and there is one implementation of the
timing rather than one per host. Only the Mac half is built today -- the files
cross-compile for cortex-m4, measured object by object below, and nothing has
been linked into firmware.

It exists: `src/syncd.{h,cc}`, started with `octomancer-sync --daemon`. The
cycle is the old `run_cycle()` with its sleeps turned into states -- and the
state worth naming is `align`, which is the one that looks like a wait and is
not. The RTC field holds whole seconds, so a write has to leave at a
particular instant to land on a boundary. The old daemon slept until then on
the thread running the cycle -- it kept answering its socket, which a second
thread served, but it could do nothing else until the write left -- and this
arms a timer and goes back to the loop.

**Layer 2 is where the complexity goes.** One connection down to each sync
daemon, carrying the state broadcast up and control messages down -- the same
connection for both, because the protocol is asynchronous in both directions
anyway and a second one would only add a way for the two halves to disagree
about whether the daemon is still there.

Everything expensive and everything that can wait lives here: merging what the
sync daemons report into one roster, **all of the logging**, the permissions,
the request state the interfaces poll, and answering however many interfaces
are open. Logging in particular is the clearest case of the principle. It is
the most complicated thing the project does -- rotation, compaction, a
per-camera history that has to survive restarts -- and it is also the thing
that cares least about being a few hundred milliseconds late. So it goes as far
from the timing as it can, which is also the only place with a filesystem.

**Layer 1 is what a person runs.** Several at once, none of them privileged,
none of them holding a radio, a camera or a lock. A command-line program that
runs for forty milliseconds and an app that runs all afternoon are the same
kind of client.

### What the sync daemon says, and what it can be told

The point of the split is that layer 3 keeps almost nothing, so the broadcast
is not a summary of a database it holds -- it is very nearly everything it
knows.

**Upward, unasked, per device:** how long ago it was last seen, the **averaged**
offset and the **averaged** signal strength, its pairing state, and a short
sliding window of the last few exact measurements.

Age rather than a timestamp, because two clocks that disagree is exactly the
thing this project exists to be careful about, and a number counted forward
from the last sighting needs no agreement about what time it is.

Averaged rather than raw, and this reverses what an earlier draft of this
section argued. It said averaging was a judgement and judgements belonged
upstairs, so the box should send the last raw pair and let the control daemon
decide. That is wrong for one decisive reason: **the averaged offset is the
number the sync daemon actually synchronises on.** `measure_bench()` in
`src/syncd.cc` votes with each device's `median_offset` and takes the median of
those; the raw last sighting is not in the arithmetic anywhere. A UI showing
the last raw
sighting would be showing a number no decision was ever made from, differing
from the real one by the jitter the averaging exists to remove, and somebody
would eventually spend an evening on why the displayed offset and the applied
offset disagree. Show the number that was used.

The window of exact measurements rides along for the other audience. A person
looking at a screen wants the average; a log, or somebody chasing a bad box,
wants the individual sightings and what they scattered by. Sending a few of
them costs almost nothing, and it is the raw material layer 2 needs to fit
anything the box cannot -- drift in particular, which needs a lever arm of
fifteen minutes or more and is refused outright from a short one (see
`src/registry.h`). The box cannot hold an hour of samples, so it cannot fit
drift; the control daemon has been receiving and logging the exact
measurements all along, and can. "Two windows" below draws the line between
what those samples may be used for and what they may not.

**Downward, when somebody wants something:**

| told | means |
|---|---|
| `enable device ID` / `disable device ID` | whether this box counts toward the bench, or this camera may be written to |
| `synchronise device ID now` | do it, rather than waiting for the schedule to decide |
| `passcode for device ID` | the six digits a camera is displaying; see below |

That is a smaller vocabulary than the eight verbs `src/syncd.cc` serves today,
and deliberately so. The current set grew from what was convenient to ask a
process on the same machine; this is what a box on the end of a serial cable
actually needs.

### The rates, which are decided

**Broadcast once per second. Keep the last four samples per device.**

Those two numbers together do more than keep the memory down, and the second
job is the interesting one.

**Memory.** Four samples of two doubles is 64 bytes a device, 320 bytes for a
five-box room. `Registry`'s Mac defaults -- an hour capped at 8192 samples --
would be 128 KB a device on a part with 256 KB in total, shared with the
controller and **(unverified: the RAM figure is from memory; see
`doc/standalone-notes.md`)**. Two thousand times smaller, and now a rounding
error rather than a design constraint.

**Bandwidth.** One message per device per second. Over a serial cable that is
nothing; over a BLE characteristic it is comfortably inside what a connection
interval will carry, which is the transport that would otherwise have set the
ceiling.

**Redundancy, which is the point.** Every broadcast carries the last four
samples, so each individual measurement goes out four times, in four
consecutive broadcasts. Losing one to a dropped packet costs nothing; losing a
measurement outright means losing four broadcasts in a row, which on a link
healthy enough to be worth using does not happen. The control daemon gets an
essentially complete record of every sighting without anything having to
acknowledge, retransmit or be asked twice -- which matters because it is the
control daemon that has to fit drift out of those sightings, and a fit is only
as good as the gaps in it.

That third property is what pins down what a "sample" is here, and it is worth
being explicit because the obvious reading breaks it.

> **Measured 2026-08-29, on the bench over a real dongle.** A Tentacle
> advertises about **seven times a second** -- two boxes went from 294 and 285
> sightings to 364 and 354 over ten seconds.
>
> So if the four retained samples were the last four *adverts*, a one-second
> broadcast would carry four of the seven that arrived, three would never be
> sent at all, and consecutive broadcasts would share **none** -- the
> redundancy would be exactly zero, which is the opposite of the intent.
>
> The four are therefore **one per broadcast interval**: the most recent
> sighting in each of the last four seconds. Then a sample rides four
> broadcasts, the property above holds, and the sub-second adverts that are not
> sent are the ones that were never going to add anything -- they are
> re-measurements of the same second.

### Two windows, and why both go over the wire

The window the average is computed over is **not** the window that is
broadcast, and they are decoupled on purpose. They answer different questions.

**The averaging window produces the clock.** What comes out of it is the
Tentacle network's real time, expressed as a skew against this machine's local
clock -- and that skew is what a camera's RTC gets written from. It is the
operational number, the one every write depends on, and it is computed on the
box out of every advert the radio actually heard.

**The broadcast window is a slice of what feeds it, shipped for the log.** Those
four samples are the most recent of the measurements the average was made of --
a slice rather than the whole of it, since the averaging window is free to be
longer -- sent up so the Mac can keep a record, plot it, and answer "what was
this box doing at 03:14" long afterwards. Nothing depends on them arriving.

Sent one second at a time, though, the slices join up: four in every broadcast,
one new one each second, so what the Mac accumulates is the continuous history
even though no single message carries more than four points of it.

The two therefore show different things, and **that is the reason both are
worth sending.** If either could be cheaply derived from the other, one of them
would be waste. In principle the Mac could compute the average itself from the
whole history of samples it has received -- and that is exactly the thing not
to do, because it would only be correct if no packet had ever been lost between
the dongle and the Mac, and over BLE that cannot be relied on. The average is
computed where the record is complete and shipped as a fact; the samples are
best-effort telemetry.

Which gives layer 2 a rule that is easy to get wrong later, so it is written
down here:

> **Never recompute the operational number from the samples you received.**
> Your copy has holes and the box's does not. Use the broadcast average for
> anything a camera's clock depends on. Use the samples for logs, plots,
> debugging, and for the things that tolerate gaps -- drift being the example,
> since a fit over fifteen minutes or more does not much care about a missing
> sighting here and there.

Sizing them is then two independent decisions. The broadcast window is four,
set by the redundancy in "The rates" above. The averaging window is whatever
makes a good clock, and is not constrained by it at all: a running average
costs one double and no history, so the box can average over far more than it
retains individually. Which estimator to use is still open.

### Pairing, when there is nobody to ask

The sync daemon does the pairing, because pairing is a radio operation and the
radio is its. The problem is that a camera pairs by displaying six digits and
waiting, and a box on a rig has no screen, no keyboard, and nobody standing in
front of it.

So the passcode travels the same way everything else does. Pairing state is
part of the per-device broadcast, with three values:

* **seen, not paired** -- the device is on the air and nothing has been
  attempted.
* **pairing, needs a passcode** -- the exchange has started and is waiting.
  This is a request, published rather than sent: the sync daemon says what it
  is waiting for and carries on, and does not care who answers.
* **paired** -- there is a bond, and it survives a reboot.

The control daemon watches for the middle one, shows it to whichever
interfaces are attached, and whichever one a person is actually looking at asks
them for the digits. The answer comes back down as `passcode for device ID`.
Nothing in the sync daemon knows that a person exists.

This is also why the state broadcast is a broadcast rather than a reply. A
request that has to be *asked for* cannot be noticed by a control daemon that
happened to connect a second later, and pairing is precisely the case where
something started without anybody watching.

### What layer 3 keeps, and where

Minimising this is a design goal rather than an economy, because the box has
NVS and nothing else, and because state that is only in one place cannot get
out of step with itself.

**In RAM, and lost on reboot:** everything about message latency and timing.
That is four samples per device, the running average, and what the write delay
has converged to. It is cheap to re-measure, it goes stale anyway, and none of
it is worth a flash write.

Four is the whole of it: **64 bytes a device, 320 bytes for a five-box room**,
against `Registry`'s Mac defaults of 128 KB a device. See "The rates, which are
decided" above for why four and not some other number -- it is set by the
redundancy the broadcast is supposed to provide, not by what fits. Anything
needing a longer arm than that -- drift being the example -- is computed by
layer 2 out of the measurements it has been receiving all along.

The one thing here that is genuinely unbounded is the *set of devices*. Four
samples each is nothing; ten thousand devices each with four samples is not.
Nothing in the room justifies worrying about it yet, and a cap on the roster is
the obvious answer when something does.

**In flash, and only this:** whether each device is enabled or disabled, and
the Bluetooth pairing state. Both are things a person decided, neither can be
re-derived by listening, and losing either one across a power cycle is a
question somebody has to answer again in a room they may not be standing in.

**Nowhere on the box:** the logs. They go up to layer 2, which has a
filesystem. See "What lives in flash" below for the flash map this implies.

### Where this is not the system yet

The diagram above is the target. It is not a description of the present, and
the difference is large enough that reading it as one would send somebody in
the wrong direction. As of 2026-08-29, checked against the source rather than
against memory:

- **Layer 2 does not exist at all.** No *daemon* merges rosters, holds the
  logs or fronts anything -- each interface does its own merging, two bullets
  down. `doc/TODO.md` records it as unstarted and that is accurate.
- **`octomancerd` uses its radio for the wrong thing.** It keeps one, which is
  right -- that is how it will reach a sync daemon that is not plugged in --
  but today it points it at the timecode boxes, scanning for them itself and
  keeping its own roster (`src/octomancerd.cc:402`). That is layer 3's job and
  the one thing layer 2 must not do. So the change is not "take the radio
  out", it is "stop listening to boxes with it, and start using it to reach
  the daemon that does".
- **The one connection between the daemons runs the wrong way.** The legacy
  `octomancer-sync` is a *client* of `octomancerd`, polling it for the bench
  and for whether the camera is on the air (`src/octomancer-sync.cc:318`,
  `:362`). The model has status flowing sync → control; today it flows
  control → sync, and `octomancerd` never dials out at all.
- **Every interface that shows the merged device list opens two sockets and
  does the merging itself.** (`octomancerctl` is the exception, and only
  because it shows nothing about cameras: it opens `octomancerd.sock` alone.)
  `octomancer` holds `octomancerd.sock` and `octomancer-sync.sock` at once
  (`src/octomancer.cc:54-55`), asks both (`:1009-1021`) and merges the answers
  with `build_device_view()` (`:1076`); so does the TUI, and
  both the CLI and `Octomancer.app` reach past
  the socket for `scan` and `pair`, running `octomancer-sync` as a subprocess
  -- the CLI `exec`s it, the app runs it as an `NSTask` -- because that binary
  is the one holding the Bluetooth grant. Layer 2 has to absorb all
  three of those paths, not one.
- **Nothing speaks the box protocol.** `octomancer-sync --daemon` serves
  `octomancer-syncd.sock`, and outside the tests there is no client of it
  anywhere. The shipped LaunchAgent still starts the legacy mode. So layer 3
  is finished, nothing starts it, and nothing talks to it.

Be exact about what that last one blocks, because it is easy to overstate, and
`doc/TODO.md` says the same. It does **not** block the hardware verification
the rest of this file is waiting on: the daemon schedules its own cycles and
writes each to the console and the log, so `octomancer-sync --daemon --radio
dongle` in a terminal with a camera switched on would settle it with no client
involved -- which is how the shared-radio measurement further down was taken.
What is missing there is a camera, not a client. What the absent layer *does*
block is everything else. The daemon cannot be asked what it thinks, told to do
anything, or configured, except by typing lines into a socket by hand. Layer 2
is not the next feature; it is the thing that makes layer 3 usable.

### Six decisions the layering forces, and what they are

Writing the model down turned up questions the diagram hides. Each is recorded
with the answer this document takes, because discovering them one at a time
while building layer 2 is how a shape gets decided by accident.

They are choices rather than findings, and two of them are expensive enough to
be worth confirming before anyone builds on them: **one language, all the way
up** rewrites the parse path of every client that exists, and **one connection
per sync daemon** puts an identity into the protocol that costs nothing now and
is awkward to add once there are clients. The other four follow from the
layering rather than from taste.

**One connection per sync daemon, and normally there is one sync daemon.** The
common case is a single local sync daemon on the Mac; the interesting case is
that plus a Nordic over USB, and later over BLE. So the count is not fixed at
one, and every roster line, alert and cycle report that layer 2 relays has to
say which sync daemon it came from. That means an identity in the protocol —
a name on `hello`, echoed on announcements — and it is far cheaper to add now
than after there are clients. There is a concrete obstacle nobody had written
down: the `--daemon` lock path is fixed at `octomancer-syncd.lock` with no flag
to change it, while `--box-socket` *is* overridable, so today only one
non-dry-run sync daemon can run per user however many sockets you name. The
lock should be keyed on the box socket path.

**One language, all the way up.** There are two line protocols in the tree
today: `src/proto.h`'s block reply — banner, lines, `end`, one command per
connection — spoken by both existing daemons, and `src/boxmsg.h`'s one message
per line, spoken by the sync daemon. Layer 2 sits between them, so it either
translates forever or it does not. It should not. `src/boxmsg.h` was written
to be one message language for all three pipes, and the two vocabularies
already disagree about the meaning of `id` -- a correlation tag in one, a
queued-request handle in the other -- while each of them separately overloads
it again as a device or camera identifier. That is the collision a permanent
translation table would hide, and it is the reason the broker below has to
assign its own handles rather than pass one through. The cost is honest: every client's
parse path gets rewritten once.

**Layer 2 is a request broker, not a relay.** The CLI and the app both issue a
command, get an id, and poll `result id=N` until it finishes. The box protocol
has no such thing: `sync` answers `ok what=sync queued=0|1` and the outcome
turns up later as an untagged `cycle` announcement, from a single pending slot
shared by every peer. So layer 2 has to assign the id, remember who asked, and
correlate. It cannot do that reliably against an untagged broadcast, so the
sync daemon should echo the requesting message's `id=` on the resulting
`cycle` line. Without that, the broker is guessing, and it will guess wrong the
first time two clients ask about different cameras.

**Permission is decided by layer 2 and kept by layer 3.** Today the sync daemon
reads `cameras.conf` itself, and only the front-end tools write it. The box has
no filesystem, so `src/syncd.h` already says permission will have to arrive
over the protocol -- but there is no verb that sets it, and on the Mac the
`default_writes` field that anticipates one is dead because a `CamConf` is
always installed. `enable device ID` / `disable device ID` is that verb.

The split of responsibility is worth being precise about, because "the
authority" and "who stores it" are different questions. Layer 2 is where the
decision is made and where a person changes it. Layer 3 writes what it was told
to flash and then obeys it without asking again -- which is the whole point,
because **the box has to keep working when the Mac is not there.** A rig that
stopped syncing because a laptop went to sleep would be worse than no box at
all. So the enabled set is pushed down, not looked up.

**`scan` and `pair` stop being subprocesses.** They are the one place layer 1
reaches past the socket entirely, launching `octomancer-sync` as a subprocess
because it holds the Bluetooth grant. That cannot survive a long-running sync
daemon holding the radio -- the CLI already has to print a note telling you to
stop the agent yourself -- and on a Nordic box there is no sibling binary to
launch at all.

Scanning stops being a command at all: the sync daemon is always listening, so
what a person wants is the state broadcast it is already sending. Pairing
becomes the published-request flow above -- a pairing state in the broadcast
and `passcode for device ID` coming back down -- rather than a verb that
starts something and waits for it. Waiting is the thing a box on the end of a
serial cable cannot do.

**`octomancerd.sock` is the surviving socket.** It has the muscle memory -- both agents
carry a launchd label -- so layer 2 keeps it and `octomancer-sync.sock` is retired
along with the mode that serves it. This is a correction to what this file
used to say: it claimed `octomancerd` would be "replaced in substance while
keeping its label and socket", which read as though there had only ever been
one socket to keep. There are three.

### What the daemon can be asked, and what it volunteers

The framing is `src/boxmsg.h`'s -- one message per line, a verb and
`key=value` fields, unknown keys ignored and unknown verbs answered. What
follows is the vocabulary `src/syncd.cc` actually implements, which is
otherwise only written down as code.

**It is not the vocabulary above**, and the difference is the direction of
travel rather than a discrepancy to be alarmed by. This set grew from what was
convenient to ask a process on the same machine, so it is question-and-answer
shaped: `status` and `devices` are polls. The target set is smaller and is
mostly the other way round -- the daemon says what it knows, unasked, and is
told three things. `doc/KNOWN_ISSUES.md` has the gap as work.

A request may carry `id=`, and every reply to it carries the same one back.
Nothing needs it today -- one line in, answers out, in order -- but a client
multiplexing several questions down one serial cable will, and adding it later
would have been a change to every verb.

| asked | answered |
|---|---|
| `ping` | `pong` |
| `hello` | `hello proto=1 role=sync version=…` |
| `status` | one `status` line: phase, radio, bench, camera, last action, when the next cycle is due |
| `devices` | a `dev` line per box, then `end what=devices n=…` |
| `sync [camera=…] [force=1]` | `ok what=sync queued=0\|1`. `force` overrules the gates that mean "there is no need" and none of the ones that mean "must not" |
| `source value=N [camera=…]` | `ok what=source value=N queued=0\|1`; the write is judged by whether the camera echoes it back |
| `announce on=0\|1` | `ok what=announce on=0\|1`, plus `effective=0` when the daemon's own announcements are off and saying yes would leave the peer waiting all night; a peer that does not want the unsolicited half |
| `forget dev=…` | `ok what=forget known=0\|1` |
| anything else | `err reason=unknown-verb verb=…` |

Volunteered, to every peer that has not asked otherwise: `bench` on a timer,
`dev`-shaped `alert` lines when a box crosses a threshold, `cam up=0\|1` when
a camera appears or goes, `radio state=…`, and a `cycle` line whenever a cycle
ends -- which is the one that says what the daemon actually did.

Deliberately absent: anything that stops the daemon. The same protocol is
going to be spoken over an unsecured BLE characteristic, where "anyone in
range may reconfigure this" is the request and "anyone in range may switch it
off" is not.

## Why there are no threads anywhere

When this was written, `std::thread` appeared at exactly two places in the
tree: the HCI reader in `hcilink.cc`, and the control server in
`octomancer-sync.cc`. Everything else that read like concurrency was a
`sleep_for`, or CoreBluetooth's private dispatch queue.

They go because they cannot come with us.

> **Re-counted 2026-08-29.** The HCI reader is gone -- `hcilink.cc` has no
> thread at all now. One remains, in `octomancer-sync.cc`'s legacy control
> server, and it goes with the mode that owns it; `doc/TODO.md` tracks that as
> part of the cutover. The line numbers this paragraph used to cite have long
> since moved, which is why it now names files rather than lines: a census
> pinned to line numbers is a census that rots.

> **Measured 2026-08-29.** The Zephyr SDK 1.0.1 `arm-zephyr-eabi` libstdc++
> has `_GLIBCXX_HAS_GTHREADS` undefined in **every** one of its multilib
> variants. `std::thread`, `std::mutex` and `std::condition_variable` do not
> exist on the target. This is not a Kconfig option; it is how the toolchain
> is built.

`doc/standalone-notes.md` guessed that "Zephyr's C++ support can provide it,
but the safer route is a thin shim". The answer is stronger than that: it
cannot provide it, so there is no route that keeps them.

The second measurement is the one that made this cheap rather than alarming.

> **Measured 2026-08-29, re-measured after the radio was de-threaded.**
>
> How, because the first version of this table did not say and it cost an hour
> to work out again:
>
> ```
> arm-zephyr-eabi-g++ -c -std=gnu++17 -Os -mcpu=cortex-m4 -mthumb \
>     -fno-exceptions -fno-rtti -Isrc -o FILE.o src/FILE.cc
> arm-zephyr-eabi-size FILE.o     # the "text" column, which is .text + .rodata
> ```
>
> The `text` column is the number below, not the `.text` section on its own --
> the two differ by about a third and mixing them makes a file look like it
> shrank when nothing changed. Files that still hold a `std::mutex` are
> compiled against a stub shim so they can be measured at all; that is a
> measurement device, not a plan.
>
> | file | bytes | file | bytes |
> |---|---|---|---|
> | `hcilink.cc` | 25040 | `camconf.cc` | 9306 |
> | `camera_hci.cc` | 18719 | `proto.cc` | 9035 |
> | `control.cc` | 16450 | `registry.cc` | 8521 |
> | `hci.cc` | 11787 | `bmd.cc` | 5394 |
> | `att.cc` | 10232 | `camsync.cc` | 5148 |
> | `devices.cc` | 9358 | `loop.cc` | 4088 |
> | `smp.cc` | 3687 | `crypto.cc` | 3318 |
> | `render.cc` | 3146 | `boxmsg.cc` | 2910 |
> | `jsonlog.cc` | 2519 | `loopfake.cc` | 2468 |
> | `logscan.cc` | 2427 | `pairing.cc` | 2120 |
> | `tentacle.cc` | 1401 | `timeutil.cc` | 1197 |
> | `syncd.cc` | 20011 | `escape.cc` | 317 |
> | `hcishare.cc` | 8024 | `scanner_hci.cc` | 2462 |
>
> Only `camdb.cc` fails to port, because it is a file-backed database -- and
> on-box logging is excluded by design anyway. Earlier failures on `gmtime_r`,
> `localtime_r` and `strerror` were an artifact of `-std=c++17` setting
> `__STRICT_ANSI__`; `-std=gnu++17` fixes them.

That table is the whole tree, which is not what the firmware links. The set the
box actually needs -- the HCI host and its sharing layer, the scanner and the
camera client on top of them, the crypto and pairing underneath, the sync
arithmetic, the loop, the message codec, the roster and the daemon that drives
all of it -- comes to **131 KB against a 408 KB slot, or 32% of it**. There is
room, and there is room by a factor of three.

> **Re-measured 2026-08-29, a third time, with `src/hcishare.cc` in it.** The
> set was 101 KB before the sync daemon and 121 KB with it. Sharing the radio
> added 11 KB: 8024 bytes of `hcishare.cc`, 336 more in `camera_hci.cc`, and
> 2462 for `scanner_hci.cc` -- which was missing from the two earlier sums and
> should not have been, because a box that cannot scan has nothing to sync to.
>
> `camconf.cc` is excluded from all three, because it reads a file and the box
> has no filesystem -- permission will have to arrive over the control protocol
> instead. The three files that deliberately do *not* cross-compile are the
> seam working as intended: `loop_posix.cc` on `poll.h`, `boxsock.cc` on
> `sys/socket.h`, and `scanbridge.cc` on `std::mutex` -- the last one being a
> file that exists only to carry another thread's work to this one, on a target
> that has no other thread.

### De-threading made the HCI host twice as big

Worth stating plainly rather than leaving to be discovered, because it is the
opposite of what "removing a thread" sounds like:

| | before | after |
|---|---|---|
| `hcilink.cc` | 12331 | 25040 |

A blocking API stores its waiting state on the caller's stack, which is free.
An asynchronous one stores it in the object, and here that means a
`std::function` per outstanding operation, a `std::deque` per queue, and a
`std::map<uint16_t, AttChannel>` whose value type contains both. Each distinct
`std::function` signature is its own set of instantiated thunks. `camera_hci.cc`
went the same way for the same reason.

Twelve kilobytes for a radio that works on the target at all is a trade worth
making, and 131 KB in a 408 KB slot means it can be made without arithmetic. But
if the firmware ever does run out of room, this is the first place to look, and
the cheapest fix is fewer distinct `std::function` types rather than less code.

`doc/standalone-notes.md` estimated 250-350 KB and said to measure before
designing around it. That was a good estimate and it was pessimistic: even
after the radio doubled in size and the daemon and the sharing layer were added
to it, the set the firmware links is 131 KB -- half the bottom of that range
and well under the top. **Flash is not the binding constraint.**

RAM at run time still is, and is still unmeasured
**(unverified: the heap cost of `std::string`/`std::map` in `Registry` under a
real bench has never been profiled. What would settle it: build the firmware
and read the thread analyzer's high-water mark.)**

### Nothing blocks

The model is fully event-based: never wait inside a call, only enqueue and
dequeue. That is a stronger rule than it first appears, and it is load-bearing
for a reason that is easy to miss.

The example that made the rule worth stating is worth keeping even though the
code is gone. `src/camera_hci.cc` — a portable POSIX file compiled into
`libocto.a`, not part of the Mac backend — used to have **its own** mutex and
condition variable, with two waits whose predicates only an `hcilink` callback
could satisfy: `await_state`, and the encryption step. Delete the reader
thread and leave those waits in place, and the single remaining thread parks
on a condition variable with nothing left alive to signal it. Not a slow path:
a guaranteed deadlock, in a file that neither the first design pass nor the
obvious grep for `std::thread` had looked at.

An event-based model dissolves this rather than patching it, and that is what
happened — `src/camera_hci.cc` has no mutex, no condition variable and no
`await_state` today, and what were waits are states in the sync machine. Do
not go looking for the deadlock in the current file; look at why it was
invisible. This is also why `src/loop.h` deliberately offers **no** primitive
that waits inside a call: the shape is available in C++ whether or not we
provide a helper, and not providing one is what stops it being written again.

CoreBluetooth is not an obstacle to this. It delivers on a private dispatch
queue, which is already an event source; the queue writes a byte to the
loop's wake pipe and the work is picked up on the loop's own thread. That is
what `Loop::wake()` is for, and it is the only method on the loop that is safe
to call from another thread or an interrupt.

### The loop

`src/loop.{h,cc}` is written and tested, and the whole radio path now runs on
it: `hcilink.cc`, `camera_hci.cc`, `scanner_hci.cc` and `octomancer-zoom` have
no threads left between them. The backend is two virtuals — what time is it, and wait until something happens — so all
the ordering arithmetic is tested once instead of once per platform.
`loop_posix.cc` is `poll(2)`; the Zephyr backend will be `k_poll()` and is not
written; `loopfake.cc` is a variable holding the time.

That last one is why the abstraction earns its keep. With time as a number the
test sets, a whole sync cycle — an hour of drift, a missed window, a camera
that never answers — runs in no wall-clock time with no radio attached. The
alternative is a suite that is slow when it passes and flaky when it fails.

> **Measured 2026-08-29.** With no threading shim on the include path,
> `loop.cc` cross-compiles to 4088 bytes and `loopfake.cc` to 2468, by the
> command in the sizing block above. `loop_posix.cc` fails on `poll.h`, which
> is the seam working as intended.

`tests/test_loop.cc` pins sixteen properties, and the ones worth naming are
the ones that are wrong by default: I/O is dispatched before a timer due at
the same instant (a timer coinciding with an arriving packet is nearly always
that packet's timeout, and running it first reports a failure for something
that arrived); a repeating timer that missed four periods while a handler ran
long fires **once** and realigns, rather than firing four times (a beacon
catching up on skipped broadcasts is a burst of radio nobody asked for, and a
sync cycle doing it is four connections to a camera); `POLLHUP` is reported
alongside readability and never instead of it, because a peer that closed its
write side has still sent bytes that `server.cc` depends on reading; and a
handler that adds a hundred sources does not disturb two other sources ready
in the same tick, because `tick()` copies the handler out before invoking it
and `Server`'s accept handler really does add a source from inside one.

### Testing a radio without a radio

`src/hciport.h` has said since it was written that `Port` is virtual "so a test
can substitute one without a dongle -- the HCI host above is a state machine,
and a state machine that can only be exercised through real hardware is one
that gets exercised rarely". It was exercised never, for four years of
afternoons, and the reason was structural rather than lazy: the host needed its
reader thread to make progress, and a thread needs real time to run in. A test
could substitute the port and still had nothing to drive it with.

The loop removes the second half of that. `tests/test_hcilink.cc` is the
pattern, and it is worth copying rather than admiring:

* the port is two byte vectors, one for each direction;
* the clock is a variable;
* the controller is a function that reads what the host wrote and appends the
  bytes a real one would have sent back;
* nothing sleeps, so the whole suite is instantaneous, and nothing is flaky
  because nothing races.

Twenty-two properties are pinned that way, including several that are
inconvenient to produce on a bench at all: a controller that accepts a command
and then goes silent, a peer that disconnects with a request outstanding, the
dongle being unplugged mid-transaction, and a controller that grants no ACL
credits. Waiting for those to happen naturally is how they end up untested.

The same three pieces -- a fake byte pipe, a fake clock, a scripted peer -- are
what the sync daemon, the control daemon and the box protocol should be tested
with. None of them needs a radio either.

One honest limit. All of this tests what *this program* does with the bytes it
is given. It says nothing about whether a real nRF52840 answers the way the
script assumes, and it cannot: the script was written from the specification
and from this program's own expectations, so a shared misreading would be
invisible to it. That is the half of the doubt `doc/dongle-notes.md` still
carries.

## The order of work

USB first, Bluetooth second. The reasoning is not about difficulty, it is
about instrumentation: USB CDC is a byte pipe that can be watched from a
terminal, so the protocol and the box's behaviour get debugged over a
transport that cannot itself be the thing that is broken. Only then is BLE
added, at which point a failure is known to be the radio's.

1. The loop, and de-threading the radio path. **Done.**
2. The protocol codec, and the persistence record formats. Portable, tested.
   **Codec done; the record formats are not.**
3. The Mac sync daemon and control daemon -- layers 3 and 2. The churn. **The
   sync daemon is done** -- `src/syncd.{h,cc}`, cross-compiling for cortex-m4, thirty-four
   properties pinned against a fake camera on a clock that is a variable, and
   run against the room over a real dongle. The control daemon is not started.
4. Standalone firmware over USB.
5. BLE control, and the status broadcast.
6. MCUboot and A/B update.

Step 4 depends on something that has never been tested: **connecting, pairing
and writing a clock over the dongle**. Scanning is the only thing that has
ever worked. That is a real prerequisite risk and it is best answered from the
Mac, where there is a debugger and `--trace`, before any of it is firmware.

Half of that risk is now retired, and it is worth being exact about which
half. `tests/test_hcilink.cc` drives the whole HCI host -- bring-up,
connecting, discovery, an ATT request that times out, flow control, a peer
that vanishes -- against a controller made of canned bytes and a clock that is
a variable. So the *host's* arithmetic has been exercised. What has still
never happened is a real controller answering it. When the dongle is finally
pointed at a camera, a failure is now much more likely to be the controller's
behaviour or the camera's than this program's bookkeeping, and that is a
different and much cheaper thing to debug.

### What de-threading the radio path actually changed

The reader thread is gone from `src/hcilink.cc`, and with it four mutexes and
two condition variables. What replaced each of them is a queue, because a
queue is what each lock was standing in for:

| Was | Is |
| --- | --- |
| `command_mu_`, one command at a time | a command queue, one in flight |
| `att_request_mu_`, one ATT request across *all* connections | one in flight **per connection**, queued per connection |
| `cv_` waiting on `acl_credits_` | fragments queued against the controller's credits, capped at 64 |
| `mu_` guarding every field | nothing; there is one thread |

The per-connection change is a real improvement rather than a translation: ATT
permits one outstanding request per direction per connection, and the old
single mutex made a slow camera stall a fast one.

Two things it cost. `hci::Link::open()` no longer returns a link that is ready
to use -- bring-up is half a dozen round trips, so readiness arrives as a
callback -- and `CameraLink` in `src/camera.h` can no longer be implemented
over the dongle, because that interface blocks by contract and there is
nothing left to block on. The replacement is `octo::HciCamera` in
`src/camhci.h`. Nothing that ever worked was lost by that: `doc/dongle-notes.md`
records that the dongle's camera path has never been run against hardware.
Asking for it now prints why rather than returning a link that would hang.

### Four ways the event-loop model goes wrong, all found by reading

The tests for the HCI host are good and they caught nothing on this list. Every
one of these compiles, passes, and is wrong later or elsewhere -- which is what
makes them worth writing down before the daemons and the firmware are written
in the same style.

**A `std::function` that captures the `shared_ptr` owning it never dies.** The
natural way to write "do these commands in order" is a lambda held in a
`shared_ptr` that captures that same `shared_ptr` so it can call itself. The
count never reaches zero. It is not a crash and not a wrong answer; it is a few
hundred bytes leaked per scan on a device meant to run for days. Two of these
were written here before either was noticed. The fix is not a `weak_ptr`, which
merely moves the puzzle: hold the position in a plain struct and recurse
through a member function that takes it as an argument, so the only owner is
the completion handler currently in flight and the chain frees itself when it
ends. `Link::run_sequence` and `HciCamera::try_connect` are the two.

**A reference held across a call that can destroy what it refers to.** In a
blocking design the dangerous calls are obvious because they are the ones that
block. Here every call can synchronously fail the whole object -- a write to a
dead port fails the link, which retires every queue and every channel at once
-- so a reference or an iterator taken before such a call is invalid after it,
including one taken from a `std::map` that has since been swapped out. The rule
that works is: look it up again afterwards, and check it is still the thing you
had. `Link::pump_att` does that explicitly and says why.

**A deferred continuation that captures `this` and nothing cancels it.** A
timer scheduled on the loop outlives the object that scheduled it unless
somebody cancels it. Anything posted with `after()` or `every()` that captures
`this` therefore needs its id stored and cancelled in the destructor -- or, for
work that has no id to hold, a `shared_ptr<bool>` liveness flag the closure
checks. Both appear in `hcilink.cc`: timers are tracked and cancelled,
`defer()` uses the flag. `HciCamera`'s scan timer was written without either
and would have called into a freed camera.

**Reporting success for work that was queued and then thrown away.** "I
accepted this" and "this will happen" stop being the same statement the moment
there is a queue. `queue_acl` pushed fragments, pumped them, and returned true
-- while the pump could have failed the link and dropped every one of them on
the way out. The caller then believes bytes are in flight that no longer exist.
Anything that returns a bool after touching a queue has to re-check the state
it may itself have changed.

Two of these -- the leak and the dropped fragments -- are invisible to any test
that asserts on outcomes, because the outcome is right. They are only visible
by reading the code and asking who owns what. Budget for that pass; it found
four real defects here in an afternoon and the suite found none of them.

### The rule that makes callers safe

A completion handler is never invoked before the call that registered it has
returned. An immediate failure -- not connected, link closed, malformed
argument -- is posted to the loop like any other result rather than called on
the spot.

This costs a turn of the loop and buys the caller not having to reason about
being re-entered from inside its own call. Without it, `connect()` failing
early calls its completion while `connect()`'s own frame is still live, and
that completion is entitled to call `connect()` again. Every asynchronous
interface in this program should hold to it; `tests/test_hcilink.cc` pins it
for the HCI host.

## One radio, several jobs, and finding that out cost a live run

The first time the daemon was started with a dongle plugged in, it reported the
radio powering off. Nothing was wrong with the dongle.

**One dongle is one HCI link, and the scanner and the camera each opened their
own.** macOS does not refuse the second open -- a `cu.*` device hands out
another descriptor without complaint -- so two `hci::Link`s read the same byte
stream, each sees the other's replies as corruption, and the link closes.
`lsof` showed one process holding the port twice, which is what made it obvious
and would not have been obvious from the log.

The first fix was a workaround: give each radio one job, so the dongle either
listened or drove the camera but never both. That kept the Mac working and left
the box with nothing, since the box has one radio and must do both.

**The real fix is `src/hcishare.h`.** The program that runs opens one
`hci::Link` and hands out `SharedLink::User` subscriptions; the scanner takes
one and the camera takes another. Three things are genuinely contended and are
arbitrated there rather than fought over:

| contended | resolution |
|---|---|
| whether to scan | reference counted; the radio scans if anybody wants it |
| passive or active | union — active wins, and the passive user loses nothing |
| the scan across a connection | restored after the connection is **up**, not only after it fails |

The third is not a detail. `Link::connect` has to stop scanning, because a
controller cannot scan and initiate at once, and by itself it puts the scan
back only when the attempt failed. On a desk that was invisible. On a rig it
means the daemon goes deaf to the Tentacle broadcast for the length of a
connect-pair-write — which is to say, for exactly the window in which the
reference clock is the thing being used. Routing `connect` through a `User` is
what makes the restore unconditional.

There is a related trap underneath it, which the tests pin because nothing
about it is visible in a log. `Link`'s own restore starts the scan **passive**,
whatever it was before. A shared layer that trusted its own record of the
parameters would hand the camera back a silently downgraded scan, and the
symptom would be a camera that stopped advertising a name it had always
advertised. So after any connection attempt the parameters are re-applied
rather than assumed.

> **Measured 2026-08-29, on the dongle at `/dev/cu.usbmodem212101`.**
> `octomancer-sync --daemon --radio dongle` — the configuration that used to
> fail — ran four cycles: one open file descriptor on the port, no
> `poweredOff`, and a bench of −3.56 s from two boxes with a 3.7 ms spread that
> held across three separate 20-second camera scans. Queried over the box
> socket **during** a camera scan, both Tentacles showed `age=0.1` and their
> sample counts climbed by about seven a second each — so the listener was
> genuinely still hearing the room while the other half of the radio was
> looking for a camera. `--radio corebluetooth` still works and still hears
> more boxes (four, at a 9 ms spread), because it is a better antenna in a
> worse position for timing.

What is still **(unverified)** is the same thing that was unverified before:
the camera is switched off, so no connection has been made over the dongle, and
the connect-restores-the-scan behaviour above is pinned by
`tests/test_hcishare.cc` against a scripted controller and by nothing else.

What is *not* arbitrated, and is written down rather than policed: ATT, SMP,
advertising and raw commands are reached through `User::link()` and have
exactly one owner in every program that exists. Two things doing ATT on one
link would collide and nothing would notice.

## What lives in flash

Only two things, and they are the two a person decided rather than anything
measured: **whether each device is enabled or disabled**, and **the Bluetooth
pairing state** -- which is the bonds, plus enough to know that a device has
been seen and not yet paired. Neither can be re-derived by listening, and both
are answers somebody gave in a room they may not be standing in again.

Everything about timing stays in RAM and is lost on reboot: what each box's
offset was, how the last few sightings scattered, what the write delay has
converged to. It is cheap to re-measure, it goes stale anyway, and none of it
is worth a flash write.

**No logs on the box** — the drift log is the scientific value of this project
and it belongs where there is a filesystem, drained to the Mac over the control
protocol. That is also the general rule the layering follows: logging is the
most complicated thing here and the least urgent, so it lives as far from the
timing as it can get.

The consequence is accepted deliberately: the Blackmagic RTC write bias has to
be re-measured every boot. `doc/protocol-notes.md` records that bias at −75 s
before a power cycle and 0 after one on the same body, so no fixed value would
have held anyway; a measurement per boot is closer to the truth than a stored
number. A default gets baked into firmware and the box tunes from there.

The policy question this raises belongs on the testable side and is not
cosmetic: **between boot and a converged measurement, does the box write a
clock or refuse?** A wrong clock silently written to a recording camera is
worse than no clock, so the states are `unknown`, `provisional` and
`converged`, and only the last permits an unattended write.

## The flash map, which the board already ships

This was expected to need designing. It does not: `fstab-stock.dtsi` in
Zephyr's own `raytac_mdbt50q_cx_40_dongle` board directory is a partition
table already laid out for exactly this, on the assumption that the Nordic
nRF5 bootloader stays where it is.

| Region | Range | Size | Purpose |
|---|---|---|---|
| `mcuboot` | `0x00000`–`0x10000` | 64 KB | the shim |
| `image-0` | `0x10000`–`0x76000` | 408 KB | slot A |
| `image-1` | `0x76000`–`0xDC000` | 408 KB | slot B |
| *(unallocated)* | `0xDC000`–`0xF0000` | 80 KB | spare |
| `storage` | `0xF0000`–`0xF4000` | 16 KB | NVS: bonds and roster |
| nRF5 bootloader | `0xF4000`–`0xFE000` | 40 KB | recovery DFU |
| MBR / settings | `0xFE000`–`0x100000` | 8 KB | Nordic's own |

Sums to exactly 1 MB. The chain is Nordic bootloader → MCUboot → application,
so the button-and-plug DFU stays as the recovery path and everything else is a
push. 131 KB of application code in a 408 KB slot leaves room for Zephyr, the
controller and USB several times over.

Two things to know before building it. **MCUboot is not in the workspace**:
`third_party/.west/config` filters the manifest down to eight modules and
`mcuboot` is not among them, so it needs adding and a `west update`. And the
upstream DTS has a cosmetic inconsistency — the node is named
`partition@dc000` while its `reg` says `0xf0000`. The `reg` is what counts.

### How the fallback works, which is the part that is not obvious

Two slots alone do not give you a safe update; what does is a **confirm flag**.

* The new image is written into the spare slot and marked *pending-test*.
* On reboot MCUboot verifies it, swaps it in, and boots it **unconfirmed**.
* The new firmware must prove itself and call `boot_write_img_confirmed()`.
  If it never does, **the next reboot swaps the old image back**.
* A hardware watchdog is what guarantees that next reboot happens. Firmware
  that hard-faults or wedges never feeds it, so it resets and reverts with
  nobody present.

So the mechanism is not "detect that the new firmware is broken" — which is
the hard version of the problem, and the one that has no good answer. It is
"the new firmware is on probation until it says otherwise, and silence means
revert."

The health check writes itself for this application: USB enumerated, the
controller answered `Reset` and `Read Local Version`, and the scanner produced
at least one advertising report. Too lenient and a broken image confirms
itself; too strict and a good image reverts forever in a room with no
Tentacles in it. That is a real tension and the advertising-report condition
is the one to argue about **(unverified: whether a box in an empty room is a
situation worth surviving. It probably is.)**

## The control channel is deliberately unsecured

The request is explicit: like a Tentacle, where anything in range can see and
set. This section is here so that is a decision and not an oversight.

It means anyone within radio range can read the roster, change settings, and
trigger camera pairing. On a rig that is the point — it is why the box is
usable without a Mac. The honest statement of the risk is that BLE range is
larger than the room, and "in range" is not the same as "present".

The place this stops being merely permissive is firmware update. Anyone who
can push an image can push any image. So when the A/B mechanism reaches BLE it
sits behind a physical button press on the box, and only the USB path is
unconditional. That is the cheapest mitigation that does not violate the
request, because it distinguishes reconfiguring a box from replacing what it
is.

## What is untested, plainly

* Connecting, pairing and writing a clock over the dongle **against real
  hardware**. Never done. The host side of all three is now tested against a
  scripted controller; the controller has never answered for itself.
* `octo::HciCamera` end to end. Its parts are the same logic the blocking
  version had, and the GATT discovery walk it performs is now a chain of
  continuations rather than two nested loops -- which is exactly the sort of
  rewrite that compiles and is wrong. Something drives it now: the sync daemon
  scans through it every cycle, and that scan has run against a real dongle in
  a room with 37 LE devices in it. Everything past the scan -- connect,
  discover, subscribe, pair, write -- still waits for a camera to be switched
  on.
* The sync daemon **against a camera**. Thirty-four properties are pinned
  against a fake one, which is a statement about this program's arithmetic and
  not about a Blackmagic body. The first real cycle is still ahead.
* Whether the nRF52840's controller will scan, advertise, hold a central link
  to a camera and a peripheral link to a Mac all at once **(unverified: this
  design requires it. What would settle it: the Kconfig for concurrent roles,
  and then an actual four-way test.)**
* RAM at run time, as above.
* The BLE status broadcast, whose byte layout is not written. The control
  protocol's is: `src/boxmsg.h` and the vocabulary table above are what the
  sync daemon actually serves.
* **Layer 2, entirely.** Which is the one that matters most, because until it
  exists nothing a person runs can reach the sync daemon at all, and every
  other item on this list is waiting behind that.

The loop, the message codec, the HCI host, the shared radio and the sync
daemon are the parts of this document that exist and run -- steps 1 and 3 less
the control daemon, and the codec half of step 2; the persistence record
formats are still unwritten. Everything from the control daemon onwards is still
description.
