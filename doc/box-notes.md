# The box: one loop, one protocol, three transports

*Written 2026-08-29. No firmware exists yet. Everything below is either
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

## The real split, which is about tempo

The daemons still divide, but along a different line: **a tight loop doing
timecode messaging, and a loose loop doing configuration.**

```
  [Tentacle boxes] --adverts--> SYNC DAEMON  (Nordic firmware, or a Mac process)
  [Blackmagic cam] <--GATT----> the same sync daemon
                                       |
                     control protocol over USB CDC / BLE GATT / unix socket
                                       v
                            CONTROL/STATE DAEMON  (Mac only)
                               aggregates N sync daemons
                                       ^
                                       | unix socket
                        octomancer CLI, TUI, Octomancer.app
```

The **sync daemon** owns a radio. It scans for Tentacles, holds the camera
link, runs the decision in `camsync.*`, emits announcements, and answers the
control protocol. The same source builds as Nordic firmware and as a Mac
process, and that is the whole point: the box is debuggable without a box.

The **control/state daemon** owns no radio. It holds links to any number of
sync daemons — local over a unix socket, a Nordic over USB, later a Nordic
over BLE — merges their rosters into one picture, drains their logs to disk,
and answers the CLI, the TUI and the app.

The binaries keep their names. Renaming them would churn launchd labels,
socket paths and muscle memory for no gain, so `octomancer-sync` becomes the
sync daemon and `octomancerd` is replaced in substance while keeping its label
and socket.

## Why there are no threads anywhere

`std::thread` appears at exactly two places in the tree: `hcilink.cc:47`, the
HCI reader, and `octomancer-sync.cc:2062`, the control server. Everything else
that reads like concurrency is a `sleep_for`, or CoreBluetooth's private
dispatch queue.

They go because they cannot come with us.

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
> | `camera_hci.cc` | 18383 | `proto.cc` | 9035 |
> | `control.cc` | 16450 | `registry.cc` | 8521 |
> | `hci.cc` | 11787 | `bmd.cc` | 5394 |
> | `att.cc` | 10232 | `camsync.cc` | 5148 |
> | `devices.cc` | 9358 | `loop.cc` | 4088 |
> | `smp.cc` | 3687 | `crypto.cc` | 3318 |
> | `render.cc` | 3146 | `boxmsg.cc` | 2910 |
> | `jsonlog.cc` | 2519 | `loopfake.cc` | 2468 |
> | `logscan.cc` | 2427 | `pairing.cc` | 2120 |
> | `tentacle.cc` | 1401 | `timeutil.cc` | 1197 |
> | `escape.cc` | 317 | | |
>
> Only `camdb.cc` fails to port, because it is a file-backed database -- and
> on-box logging is excluded by design anyway. Earlier failures on `gmtime_r`,
> `localtime_r` and `strerror` were an artifact of `-std=c++17` setting
> `__STRICT_ANSI__`; `-std=gnu++17` fixes them.

That table is the whole tree, which is not what the firmware links. The set the
box actually needs -- the HCI host and its camera client, the crypto and
pairing under them, the sync arithmetic, the loop, the message codec, and the
roster -- comes to **99 KB against a 408 KB slot, or 24% of it**. There is
room, and there is room by a factor of four.

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

`src/camera_hci.cc` — a portable POSIX file compiled into `libocto.a`, not
part of the Mac backend — has **its own** mutex and condition variable, with
two waits whose predicates only an `hcilink` callback can satisfy:
`await_state` at `:366` and `ensure_encrypted` at `:440`. Delete the reader
thread and leave those waits in place, and the single remaining thread parks
on a condition variable with nothing left alive to signal it. That is not a
slow path; it is a guaranteed deadlock, in a file that neither the first
design pass nor the obvious grep for `std::thread` had looked at.

An event-based model dissolves this rather than patching it. `await_state` and
`ensure_encrypted` stop being waits at all and become states in the sync
machine. This is also why `src/loop.h` deliberately offers **no** primitive
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
3. The Mac sync daemon and control daemon. The churn.
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
  rewrite that compiles and is wrong. Nothing drives it yet.
* Whether the nRF52840's controller will scan, advertise, hold a central link
  to a camera and a peripheral link to a Mac all at once **(unverified: this
  design requires it. What would settle it: the Kconfig for concurrent roles,
  and then an actual four-way test.)**
* RAM at run time, as above.
* Every byte layout in the protocol and the broadcast, which are not yet
  written.

The loop, the message codec and the HCI host are the parts of this document
that exist and run. Everything below the "order of work" heading past step 2
is still description.
