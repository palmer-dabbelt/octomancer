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

The daemons still divide, but along a different line: **a tight loop doing
timecode messaging, a loose loop doing configuration, and the programs a
person actually looks at.**

```
  LAYER 3 -- the tight loop. One radio, one event loop, no threads.

    [Tentacle boxes] --adverts-->  SYNC DAEMON           (Mac: octomancer-sync
    [Blackmagic cam] <---GATT--->  src/syncd.{h,cc}       --daemon. Later: the
                                   owns the radio         Nordic, same source)
                                       |   ^
       bench, cycle, cam, radio,       |   |  ping, hello, status, devices,
       alert, dev, status              |   |  sync, source, announce, forget
       (status, upward)                |   |  (control changes, downward)
                                       v   |
              one connection per sync daemon, carrying both directions.
              src/boxmsg.h framing: one message per line, announcements
              arrive unasked. A unix socket today; USB CDC, then BLE
              GATT, for a box that is not this process.
                                       |   ^
                                       v   |

  LAYER 2 -- the loose loop. Owns no radio. Mac only.

                             CONTROL DAEMON
                             merges what the sync daemons report into one
                             roster, drains their logs to disk, keeps the
                             request state the interfaces poll, holds the
                             permissions, answers everything above it
                                       |   ^
                                       v   |
                        one socket, one vocabulary, several
                        concurrent clients, none of them special
                                       |   ^
                                       v   |

  LAYER 1 -- user interface. Any number at once.

     octomancer CLI     octomancer tui     Octomancer.app     octomancerctl
```

**Layer 3 owns the radio, and is the only thing that does.** It hears the
Tentacle boxes, holds the camera link, runs the decision in `camsync.*`, emits
announcements, and answers the control protocol. The same source builds as
Nordic firmware and as a Mac process, and that is the whole point: the box is
debuggable without a box.

It exists: `src/syncd.{h,cc}`, started with `octomancer-sync --daemon`. The
cycle is the old `run_cycle()` with its sleeps turned into states -- and the
state worth naming is `align`, which is the one that looks like a wait and is
not. The RTC field holds whole seconds, so a write has to leave at a
particular instant to land on a boundary; the old daemon slept until then and
stopped answering for a second every hour, and this arms a timer and goes back
to the loop.

**Layer 2 owns no radio.** One connection down to each sync daemon, carrying
status up and control changes down -- the same connection for both, because
the protocol is asynchronous in both directions anyway and a second socket
would only add a way for the two halves to disagree about whether the daemon
is still there. Above that it merges what it is told into one roster, drains
the logs to a filesystem the box does not have, holds the permissions, and
answers however many interfaces are open.

**Layer 1 is what a person runs.** Several at once, none of them privileged,
none of them holding a radio, a camera or a lock. A command-line program that
runs for forty milliseconds and an app that runs all afternoon are the same
kind of client.

### Where this is not the system yet

The diagram above is the target. It is not a description of the present, and
the difference is large enough that reading it as one would send somebody in
the wrong direction. As of 2026-08-29, checked against the source rather than
against memory:

- **Layer 2 does not exist at all.** Nothing in the tree merges rosters,
  drains a log, or fronts anything. `doc/TODO.md` records it as unstarted and
  that is accurate.
- **`octomancerd` is not layer 2 wearing a different hat. It owns a radio.**
  It builds a scanner, keeps its own roster, and serves it
  (`src/octomancerd.cc:402`). Making it layer 2 means taking the scanner out,
  not adding a link to the top of it.
- **The one connection between the daemons runs the wrong way.** The legacy
  `octomancer-sync` is a *client* of `octomancerd`, polling it for the bench
  and for whether the camera is on the air (`src/octomancer-sync.cc:318`,
  `:362`). The model has status flowing sync → control; today it flows
  control → sync, and `octomancerd` never dials out at all.
- **Every interface opens two sockets and does the merging itself.**
  `octomancer` holds `octomancerd.sock` and `octomancer-sync.sock` at once
  (`src/octomancer.cc:54-55`), asks both (`:1009-1021`) and merges the answers
  with `build_device_view()` (`:1076`); so does the TUI, and
  `Octomancer.app` additionally launches
  `octomancer-sync` as a subprocess for scanning and pairing, because that
  binary is the one holding the Bluetooth grant. Layer 2 has to absorb all
  three of those paths, not one.
- **Nothing speaks the box protocol.** `octomancer-sync --daemon` serves
  `octomancer-syncd.sock`, and outside the tests there is no client of it
  anywhere. The shipped LaunchAgent still starts the legacy mode. So layer 3
  is finished, running, and invisible to everything a person runs -- which is
  also why the hardware verification the rest of this file is waiting on has
  not happened.

That last point is the one worth holding onto. Layer 2 is not the next feature;
it is the thing that makes layer 3 reachable.

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
already disagree about the meaning of `id` — a correlation tag in one, a
queued-request handle in the other — which is exactly the collision a
permanent translation table would hide. The cost is honest: every client's
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

**Permission belongs to layer 2.** Today the sync daemon reads `cameras.conf`
itself, and only the front-end tools write it. The box has no filesystem, so
`src/syncd.h` already says permission will have to arrive over the protocol —
but there is no verb that sets it, and on the Mac the `default_writes` field
that anticipates one is dead because a `CamConf` is always installed. Making
layer 2 the authority, pushing permission down as configuration, is what gives
the Mac and the box the same shape.

**`scan` and `pair` become verbs.** They are the one place layer 1 reaches
past the socket entirely, launching `octomancer-sync` as a subprocess because
it holds the Bluetooth grant. That cannot survive a long-running sync daemon
holding the port — the CLI already has to offer to stop the agent first — and
on a Nordic box there is no sibling binary to launch at all. Pairing
especially, because the passkey has to reach whoever owns the radio.

**`octomancerd.sock` is the surviving socket.** It has the launchd label and
the muscle memory, so layer 2 keeps it and `octomancer-sync.sock` is retired
along with the mode that serves it. This is a correction to what this file
used to say: it claimed `octomancerd` would be "replaced in substance while
keeping its label and socket", which read as though there had only ever been
one socket to keep. There are three.

### What the daemon can be asked, and what it volunteers

The framing is `src/boxmsg.h`'s -- one message per line, a verb and
`key=value` fields, unknown keys ignored and unknown verbs answered. What
follows is the vocabulary `src/syncd.cc` actually implements, which is
otherwise only written down as code.

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
| `source value=N [camera=…]` | `ok what=source`; the write is judged by whether the camera echoes it back |
| `announce on=0\|1` | `ok what=announce`; a peer that does not want the unsolicited half |
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
making, and 99 KB in a 408 KB slot means it can be made without arithmetic. But
if the firmware ever does run out of room, this is the first place to look, and
the cheapest fix is fewer distinct `std::function` types rather than less code.

`doc/standalone-notes.md` estimated 250–350 KB and said to measure before
designing around it. Even after the radio doubled in size, the set the firmware
links is well under half that. **Flash is not the binding constraint.** RAM at run time still is, and is still unmeasured
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
   sync daemon is done** -- `src/syncd.{h,cc}`, cross-compiling for cortex-m4, thirty-two
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

Only two things: bonds, and the roster of Tentacle devices seen on the network
with whether each is active. **No logs on the box** — the drift log is the
scientific value of this project and it belongs where there is a filesystem,
drained to the Mac over the control protocol.

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
push. 99 KB of application code in a 408 KB slot leaves room for Zephyr, the
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
* The sync daemon **against a camera**. Thirty-two properties are pinned
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
daemon are the parts of this document that exist and run -- steps 1 through 3,
less the control daemon. Everything from the control daemon onwards is still
description.
