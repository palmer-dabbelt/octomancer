# The dongle

*Written 2026-08-27, before the hardware arrived; first touched by a real
dongle on 2026-08-29. Everything below is either built and tested without a
radio, or explicitly marked as unverified. What the hardware has actually
answered so far is one paragraph, at the end.*

## Why there is a second radio

`doc/zoom-bta1-notes.md` ends with an experiment that could not be run. In
timecode mode the Zoom F6 scans and connects outward, so anything that wants to
feed it timecode has to be a **peripheral**, advertising on the F6's terms. On
macOS that means `CBPeripheralManager`, and `CBPeripheralManager`:

* will not emit manufacturer data at all;
* gives no control over the scan response;
* moves 128-bit service UUIDs into Apple's proprietary "overflow area", where a
  DA14580 cannot see them;
* and never says what it actually transmitted.

Eight advertisement variants were tried. The F6 answered none of them, and
there was no way to find out whether the bytes we thought we were sending had
ever left the machine. That last point is the real one: the investigation was
not slow because the answer was hard, it was slow because the instrument could
not be read.

HCI is the layer underneath all of that. A controller does what the host tells
it and reports what happened, and the host is now this project.

## What the dongle is not

It is not a replacement for CoreBluetooth. Machines without a dongle carry on
exactly as before, and the choice is made at run time -- though not by one rule
any more, which is the correction below. `make_ble_scanner()` and
`make_camera_link()` kept their names and their signatures, so nothing that
still calls them had to change shape. Not everything still calls them: the sync
daemon's dongle path goes through `make_hci_scanner_on()` instead, because the
factory would open a second port on a radio the daemon already has open. The
two factories no longer sit in one file: `make_ble_scanner()` is in
`src/radio.cc` and `make_camera_link()` is in `src/radio_camera.cc`. A static
library is linked an object at a time, so a single file defining both drags
CoreBluetooth's camera half into `octomancerd`, which asks only for a scanner
and then fails to link on a symbol nobody wanted.

**The two halves answer different questions, since 2026-08-29.** The scanner
takes whichever radio is present, because the dongle can scan. The blocking
camera link takes CoreBluetooth unless the dongle was *explicitly asked for* --
it tests `dongle_requested()` rather than `dongle_selected()` -- because for a
blocking camera link CoreBluetooth is the only thing that works, and `--radio
auto` means "pick something that works".

**Why it used to test the weaker question, and what that cost.** The price was
not a worse choice of radio, it was a restart loop. The shipped
`octomancer-sync` LaunchAgent runs the blocking path with no `--radio` flag,
that path exits 1 on a null link, and launchd's `KeepAlive` brought it straight
back every ten seconds, forever. Plugging in a dongle -- which `README.md`
invites -- was enough to start it, and the only visible symptom was an agent
that always looked like it was running.

**The refusal is still loud when the dongle is insisted on**, because a null
link is reported everywhere as "no radio" and that would be a lie: the dongle
is right there and its scanner half works. What is missing is a *blocking*
camera client for it, and there will not be one. The dongle's camera half is
`src/camhci.h`, on the event loop, and `octomancer-sync --daemon` is what
drives it -- which is the way to write a clock over the dongle, and what the
refusal now says to do.

It is also not custom firmware. The dongle runs a stock Zephyr `hci_uart`
image, so there is no embedded code in this repository to maintain, and every
decision worth making is made in portable C++ that `make check` can exercise on
a machine with no radio in it.

## Getting an image onto it

`tools/flash-dongle.sh` documents and automates this; `--check` says what is
missing.

```
tools/flash-dongle.sh --check
tools/flash-dongle.sh --setup                     # ~2 GB, once
tools/flash-dongle.sh --build
tools/flash-dongle.sh --package third_party/build-hci/zephyr/zephyr.hex
tools/flash-dongle.sh --flash  third_party/build-hci/zephyr/hci_uart_dfu.zip
```

`third_party/zephyr` is a submodule pinned at a Zephyr release, so the image
is a function of this repository's commit and nothing else: west reads every
module revision out of that pinned tree's own manifest, and the compiler is a
released Zephyr SDK identified by the version the tree asks for. Two builds of
one commit produce the same bytes, which is the only reason it is worth
building rather than downloading.

`--setup` does the parts that are easy to get wrong, once: a virtualenv with
west in it, a workspace filtered down to the modules an nRF52840 actually
needs -- the full manifest carries every vendor HAL there is, and all but one
of them is megabytes of silicon this project will never run on -- and the
`arm-zephyr-eabi` toolchain. It is safe to re-run and takes a second when
there is nothing to do. Nothing it fetches is tracked; `.gitignore` has the
list.

Pushing the package over the bootloader needs a tool that speaks Nordic's
*secure* DFU -- a protobuf init packet and an acknowledged transport -- and on
a current macOS neither pip package will do it.

`pip install nrfutil` resolves, installs, and is Python 2 all the way down:
`iteritems`, then `xrange`, then integer division returning floats. Patching
the first two only reveals the third.

`pip install adafruit-nrfutil` installs cleanly and runs, and speaks the
*older* nRF5 SDK protocol. Against this bootloader it sends a legacy init
packet, waits, times out -- **and then exits 0, printing "done"**. That was
the worst hour of this: a tool reporting success on a device it had not
touched. It is no longer accepted, and `--flash` now warns if no
`Open DFU Bootloader` is on the bus before it starts.

What works is Nordic's current `nrfutil`, a native binary fetched from Nordic;
`nrfutil install nrf5sdk-tools` adds the DFU commands, which turn out to be
pc-nrfutil 6.1.7 -- the Python 3 release pip declines to offer because it
predates the interpreter. `--setup` fetches it, so none of this has to be
found out twice. The tell for which protocol a package uses is the init packet
inside the zip: 69 bytes is secure DFU, 14 bytes is the legacy one that will
be ignored.

DFU mode depends on the board, and this file described Nordic's while we were
holding a Raytac. The Raytac has **one** button: unplug, hold it, plug in while
still holding, keep holding about a second after it seats. Letting go as it
goes in just starts the existing firmware. Nordic's PCA10059 has two, and the
sideways one is RESET, pressed while it stays plugged in. `README.md` has both,
side by side, and is the copy to trust; describing a board we did not own is
one of the four things that cost the day recorded at the end of this file.
Either way the LED settles into a slow pulse when the bootloader is listening.

If you would rather not install a Zephyr toolchain, a prebuilt `hci_uart` image
will do -- but it has to be built for **your** board, not for
`nrf52840dongle`. Do not read that as a generic target: it is Nordic's, the
regulator settings differ, and an image built for the wrong one flashes,
verifies, reports success and then never appears on USB again. That is the
failure at the end of this file, and reaching for somebody's prebuilt image is
the easiest way back into it.

**It has to be `hci_uart` and not `hci_usb`, which is a trap, because the
wrong one is the one with the better name.** `hci_usb` builds a USB Bluetooth
class device -- `CONFIG_SERIAL=n`, `CONFIG_USBD_BT_HCI=y` -- with no serial
port on it anywhere. Linux binds that with `btusb` and hands you an `hci0`;
macOS has no driver for the class and hands you nothing at all. This project
talks to a serial port on both systems, so `hci_usb` yields a dongle nothing
here can reach. `hci_uart` carries the same raw HCI over a UART, and the
dongle's own board file in Zephyr aims that UART at CDC ACM -- an ordinary
serial port with H:4 packets on it, which is what `hcilink.cc` is expecting.

## Using it

A dongle has to be asked for. Plugging one in changes nothing about what any
already-running program uses, which is deliberate — see "Two radios, and which
program knows" in `doc/box-notes.md`: a dongle is a second radio with its own
sync daemon, and a Mac process reaching through it would be collapsing the two
back into one.

```
octomancerd                       # this Mac's radio, whatever is plugged in
octomancerd --radio dongle        # drive the dongle's radio from here instead
octomancerd --dongle /dev/cu.usbmodem1101   # naming a port is asking for it
```

Everything below is about that second form: a Mac process driving a dongle's
radio over HCI. It is how the dongle is exercised while there is no firmware to
run a sync daemon on it, and it is scaffolding rather than the finished shape.
In the finished shape nothing here runs on the Mac at all — the dongle runs the
same `octomancer-sync` source as its own firmware, and octomancerd speaks the
box protocol to it.

| Setting | Flag | Environment |
|---|---|---|
| Which radio | `--radio auto\|corebluetooth\|dongle` | `OCTOMANCER_RADIO` |
| Which port | `--dongle PORT` | `OCTOMANCER_DONGLE` |
| Packet trace | `--hci-trace` | `OCTOMANCER_HCI_TRACE` |
| Pairing passkey | `--passkey NNNNNN` | `OCTOMANCER_PASSKEY` |

The environment variables exist because the agents are started by launchd,
where there is no command line to edit. They are read before the flags, so a
flag still wins.

`--radio auto` uses the dongle when one is plugged in. Naming a port with
`--dongle` implies `--radio dongle`: asking for a specific port and then
silently falling back to CoreBluetooth would hide a typo.

## Two things that are genuinely different

### Device identifiers change

CoreBluetooth hands out an opaque per-host UUID for each device. HCI hands out
the real Bluetooth address. Both are stable enough to key a registry on, but
they are **not the same string**, so a bench learned over one radio is not
recognised over the other. Expect the camera database and any configured
camera identifier to need re-learning when you switch.

This is not a wrapper that could have been papered over: there is no mapping
between the two without connecting to every device and comparing.

A device using a resolvable private address changes its address every fifteen
minutes or so, and nothing can make that stable. Such devices are reported with
`(private)` appended to their identifier, so a registry full of one-sighting
entries explains itself.

### The camera has to pair

`doc/ble-write-failure-report.md` records that the Blackmagic control
characteristics worked "against an already-paired camera" — reads, writes and
notifications all succeeded immediately, and no bonding prompt appeared. They
worked because the Mac had bonded with that camera long ago and CoreBluetooth
kept the key.

The dongle has no keychain and no screen. It arrives at the camera as a
stranger with an address the camera has never seen, so **the camera will
display a six-digit passkey and wait for it**. That is why `src/crypto.cc` and
`src/smp.cc` exist.

Supply the passkey with `--passkey` or `OCTOMANCER_PASSKEY`. **There is no
terminal prompt; it was never implemented.** `RadioOptions::prompt_for_passkey`
in `src/radio.h` is declared and defaulted to true and is read by nothing, so
the sentence that used to be here — that a terminal would ask — was false for
as long as it stood. What actually happens when no passkey was given is that
`octomancer-sync` never installs a passkey provider at all -- it installs one
only when a value was given -- `HciCamera` supplies one that always answers
false, and the exchange fails at
Passkey Entry with "no passkey was supplied". The pairing is abandoned and the
reason is reported; it does not hang and it does not fail quietly.

Half of the old sentence was sound. Under launchd there is nobody to ask, so
an unattended `octomancer-sync` has to be given the value or it cannot write a
clock, and that half is why `--passkey` exists. What was wrong is the other
half: it promised a person at a terminal something better, and there is nothing
better. Everyone passes `--passkey`.

There is no bond storage yet, so a camera pairs afresh on every connection.
That costs a passkey each time. It also means there is never a stale key that
silently stops working, which is the failure mode bond storage introduces.

## The Zoom bench

`octomancer-zoom` is the experiment macOS could not run.

```
octomancer-zoom --scan 30           # everything on the air, decoded
octomancer-zoom --dump C0:1A:...    # connect and print the whole attribute table
octomancer-zoom --serve             # advertise the profile and host it
octomancer-zoom --sweep             # ...cycling the variants we are unsure about
```

`--dump` repeats in one command the confirmation pass that produced the profile
in the notes. `--serve` hosts that profile — the service, all three
characteristics with the properties the real adapter reports, and a Device
Information service answering `ZOOM` / `F6` / `v_0.21` — and logs every single
thing the F6 does to it, verbatim.

The default advertisement is the firmware's own template and nothing else:

```
02 01 06                                    flags, LE only
11 07 aeab75f30c56b98601427dcd9415985e      the service UUID, little-endian
```

That second line is byte-for-byte the advertising template found inside
`F6SYSTEM.BIN`. `tests/test_hci.cc` pins it, so a change that breaks it breaks
the build.

`--sweep` cycles the things the notes leave genuinely open, twenty-five seconds
each:

1. the template above, alone;
2. plus a short name (`US`, `USB`, `UltraSy`) — the full `UltraSync BLUE` is
   16 bytes of structure and will not fit alongside an 18-byte UUID in a
   31-byte budget, which is exactly the trade macOS resolved silently and
   wrongly;
3. the full name with the UUID dropped, in case the F6 matches on name;
4. the name in the **scan response** instead, which macOS gave no way to do at
   all;
5. manufacturer data with Atomos's company identifier, in case the F6 filters
   on it.

**What is still unknown is what bytes the F6 expects once it has connected.**
The GATT layout is only plumbing; the protocol carried over Server TX Data has
never been observed. So `--serve` logs writes rather than answering them, and
`--tx HEX` sends whatever you ask for. This is a bench, not a driver.

## What is tested, and what is not

Four test binaries run with no hardware attached:

| Test | What it pins |
|---|---|
| `test_hci` | command framing, event parsing, ACL, advertising data. The Zoom advertising template is checked against the bytes found in Zoom's firmware. |
| `test_att` | a full GATT discovery sequence driven against a server built to the real Zoom profile, plus L2CAP reassembly. |
| `test_crypto` | AES-128 against FIPS-197, AES-CMAC against RFC 4493. |
| `test_smp` | a complete legacy pairing exchange between the initiator and an independently written responder, agreeing on a key. |

What none of them can check is whether real hardware agrees. In particular:

* **The SMP key-derivation functions are not checked against the
  specification's own worked examples.** `test_crypto` checks their
  construction — field order, widths, the fixed constants, that every input
  reaches the output — and `test_smp` checks that two independent
  implementations agree. Both would still pass if the whole construction were
  consistently wrong. AES and CMAC underneath them *are* pinned to published
  vectors.
* **LE Secure Connections is not implemented.** The AuthReq goes out with the
  SC bit clear, so a peer that can do both will choose legacy. A peer that
  *requires* Secure Connections is refused with a message that says so. The
  crypto for it is already written and tested (`f4`, `f5`, `f6`, `g2` in
  `crypto.h`); what is missing is the public-key exchange, which needs the
  controller's `LE Read Local P-256 Public Key` and `LE Generate DHKey`
  commands wired through `hcilink`. If a Blackmagic camera turns out to insist
  on it, that is the work.
* **Almost nothing has been run against an nRF52840.** See below for the one
  thing that has. The framing, the event parsing and the AD decoder have still
  never seen a packet from a real controller.

## Notes for when the hardware arrives

* The nRF52840 has no public Bluetooth address assigned to it. `Read BD_ADDR`
  comes back all zeros, and every attempt to advertise or connect from a
  "public" address is then rejected with a status that says nothing about
  addresses. `Link::init()` handles this by installing a random static address
  — but if something refuses to advertise, this is the first place to look.
* The dongle appears twice on macOS: `/dev/cu.usbmodem*` and `/dev/tty.usbmodem*`.
  Only the `cu.` one is ever used. Opening the `tty.` twin blocks forever
  waiting for carrier detect, with no error and no output.
* `--hci-trace` prints every packet in both directions. This is the facility
  whose absence made the Zoom investigation so slow; use it early.
* An initiator left running blocks every later scan with a bare "command
  disallowed". `Link::connect()` cancels on timeout for this reason.

## What the hardware has actually said

The dongle is a **Raytac MDBT50Q-CX-40**, and it works. On 2026-08-29, running
Zephyr's `hci_uart` built for `raytac_mdbt50q_cx_40_dongle/nrf52840`:

```
radio: /dev/cu.usbmodem212101, HCI version 13, LMP subversion 0xffff,
       address C7:49:78:1A:39:60 (random static)
E9:03:67:A7:D8:F9   rssi -49  "BMPCC"
    manufacturer 0x043f = 0202e00113
```

That last one is a Blackmagic camera -- `0x043f` is their company identifier --
seen over our own Bluetooth host for the first time.

| Verified against hardware | How |
|---|---|
| A dongle is found without being named | `list_candidate_ports()` picked it out of `/dev` unaided |
| The transport carries H:4 both ways | `Reset` answered; `Read Local Version` and `Read BD_ADDR` decoded |
| The zero-address workaround is needed, and works | `Read BD_ADDR` returned zeros; `Link::init()` installed a random static address, and scanning then worked |
| Command framing and event parsing | dozens of advertising reports parsed without a desync |
| The AD decoder | flags, 16- and 128-bit service UUIDs, service data, manufacturer data, tx power and names all decoded |
| Private addresses are called out | resolvable private addresses are marked `(private)` |
| Silence is reported as silence | before flashing, the timeout named the unanswered command instead of hanging |
| One radio does both jobs at once | `octomancer-sync --daemon --radio dongle` ran four cycles with the scanner and the camera client on a single `hci::Link` (`src/hcishare.h`); one file descriptor on `/dev/cu.usbmodem212101` throughout, and no spurious `poweredOff` |
| The Tentacle clock survives a camera scan | a bench of -3.56 s from two boxes at 3.7 ms spread held across three separate 20-second camera scans, and the roster still answered `age=0.1` over the box socket while a scan was running |

Still untested against hardware: connecting, pairing, and writing a clock.
Scanning is receive only.

That list has not got shorter, but the doubt behind it has. As of 2026-08-29
the HCI host has no reader thread: it runs on the event loop in `src/loop.h`,
and `tests/test_hcilink.cc` drives it against a controller made of canned bytes
with the clock held as a variable. Bring-up including the zero-address
workaround, connecting, an ATT request that goes unanswered, a notification
arriving mid-request, ACL flow control, a peer that disconnects with a request
outstanding, and the dongle being unplugged are all exercised with no hardware
present and no wall-clock time spent.

So the table above is still the only column of this document backed by a real
controller. What has changed is that a failure on the day this is finally
pointed at a camera is much more likely to be the controller's behaviour or
the camera's than this host's own bookkeeping.

## The day it cost, and why

Between the dongle arriving and that scan, every image flashed cleanly and
then did nothing at all -- no USB, no LED, invisible on the bus. Four things
were wrong, and each one hid the next.

**`hci_usb` is the wrong sample.** It builds a USB Bluetooth *class* device
(`CONFIG_SERIAL=n`, `CONFIG_USBD_BT_HCI=y`) with no serial port on it. Linux
binds that with btusb; macOS has no driver for it, and this project speaks to a
serial port on both. `hci_uart` is the one whose board file puts HCI on CDC
ACM.

**Neither pip DFU tool works.** `pip install nrfutil` is Python 2 all the way
down -- `iteritems`, then `xrange`, then integer division returning floats.
`pip install adafruit-nrfutil` installs and runs and speaks the *older* nRF5
protocol, so against this bootloader it times out **and then exits 0 printing
"done"**. Nordic's current `nrfutil` binary is what works; its bundled DFU
commands are pc-nrfutil 6.1.7, the Python 3 release pip declines to offer
because it predates the interpreter. The tell is the init packet in the zip:
69 bytes is secure DFU, 14 bytes is the legacy one that gets ignored.

**The DFU button is per-board.** One-button dongles want the button held for a
second *after* the plug seats. Nordic's two-button PCA10059 wants its sideways
RESET pressed. The instructions here described the second for a board we do
not own.

**And the one that cost the day: the board target must match the dongle.**
Every nRF52840 dongle is the same chip, so an image built for the wrong board
flashes, verifies, and reports success -- and then never appears again, which
looks exactly like an empty USB port. The difference that killed it was the
power regulators:

```
nordic/nrf52840dongle           reg0 okay,     reg1 DCDC
raytac/mdbt50q_cx_40_dongle     reg0 disabled, reg1 LDO
```

Nordic's board enables the high-voltage regulator and switches the core supply
to DC/DC, which needs external inductors the Raytac module does not have. The
supply collapses during board init -- before USB, before any LED, before
anything that could report it.

What made this hard to see is that every cheap explanation was eliminated and
the answer was still not visible. It is not the HCI firmware: Zephyr's plain
`cdc_acm` sample, 60 KB with no Bluetooth in it, was equally dead. It is not a
bad write: the DFU trace shows every byte transferred and the bootloader's own
CRC agreeing. It is not a rejected image: a bootloader that refuses one stays
in DFU and says so. It is not the offset: `--info` reported the application at
0x00001000, exactly where it was linked. An app that toggled *every* GPIO
blinked nothing, which was the last clue -- no pin can toggle if the core has
already browned out.

Two things would have found it faster. `tools/flash-dongle.sh --info` asks the
bootloader where it lives: Nordic puts it at 0xE0000, Raytac at 0xF4000, and
Raytac's own partition file carries the comment `Nordic nRF5 bootloader
<0xf4000 0xa000>`. The hardware had been saying it was not a Nordic dongle the
whole time. And `max_size` in a DFU trace is the protocol's chunk size, not the
application region -- an hour went into trying to infer the layout from it.

## The first evening the firmware ran

Three faults, found in the space of one probe, and only one of them was in
code. Written down because all three are the same shape: a thing that worked
correctly and answered wrongly, on the one machine the test suite cannot run
on.

**Picolibc prints no floats unless asked.** It ships printf in two flavours
and links the integer-only one by default, and that one does not fail on
`%f` -- it prints nothing for it and returns success. So `set_double`
produced `offset=` with no number after it, for every offset, spread, uptime
and drift figure the box has, while `set_int` worked perfectly and the build
was clean and silent. The fix is `CONFIG_PICOLIBC_IO_FLOAT=y`; the check is
`can_format_doubles()` in `src/boxmsg.h`, which the firmware runs at boot and
says out loud. Confirm from the symbol table rather than by reading the
config: `nm zephyr.elf | grep vfprintf` resolves to `__d_vfprintf` when float
support is in and `__l_vfprintf` when it is not.

**Picolibc's `localtime_r` works, and answers UTC.** There is no timezone
database on a dongle and no TZ for it to read, so it returns a correct
conversion into the wrong zone. A Tentacle broadcasts a *local* time of day,
so every box on the bench read seven hours out -- a confident number, in the
right units, from code that did what it was told. The `time` verb now carries
a zone and `firmware/src/boxclock.cc` defines `localtime_r` from it, which is
the same move that file already makes for `gettimeofday` and for the same
reason: Zephyr will happily supply a version backed by something nothing sets.

**A crashed box looks exactly like a bad cable.** Zephyr's default fatal
handler halts the CPU with interrupts locked. On a USB device the pull-up
stays asserted through that, so the host goes on listing the device while
nothing is left running behind it -- and `open()` blocks forever waiting for
control transfers nobody will answer. There is no error, no disconnect, and
no console, because `prj.conf` switches the console off so the CDC port can
carry the box protocol alone.

Recognise it by: the device present in `ioreg` with an unchanged `sessionID`,
`open()` on `/dev/cu.usbmodem*` hanging rather than failing, and
`octomancer dongle` reporting a greeting followed by silence.

`firmware/src/faultlog.cc` is the answer. It catches the fault, writes the
reason and the PC into `__noinit` RAM -- which survives `SYSRESETREQ` -- and
reboots, so the dongle comes back and the first thing it tells the next host
is what killed the last run. Consecutive faults are counted, so a crash loop
reads as one rather than as a series of unrelated deaths, and a box faulting
during boot slows its own reboots enough that a `dfu` command can still be got
into it.

### What is pinned to hardware and what is not

| Claim | How it is checked |
| --- | --- |
| Float printf is linked | `nm zephyr.elf` shows `__d_vfprintf` |
| The fault handler is ours | `nm zephyr.elf` shows `k_sys_fatal_error_handler` defined |
| The fault record survives a reset | `g_retained` is inside the `noinit` section, by address |
| `localtime_r`/`gettimeofday` are ours | `addr2line` resolves both into `boxclock.cc` |
| The build is reproducible | two clean builds, byte-identical `zephyr.bin` |

Not checked without hardware, and stated as such: that the fault handler
actually fires and reboots, that the retained record reads back correctly
across a real reset, that the dedicated CDC workqueue prevents the wedge, and
that a real Tentacle's offsets from the dongle agree with the Mac's. Each of
those needs a box on a cable and none of them has been seen yet.

## The watchdog, and what it is for

`firmware/src/faultlog.cc` covers a box that crashes. It does not cover a box
that *stops*, and those are different: a workqueue that cannot run throws
nothing and faults nothing, so no fatal handler is ever reached. The machine
sits there enumerated and blinking with a port that will not open. Only a
watchdog catches that.

The nRF52840 watchdog has eight reload registers and reloads only when **every
allocated one has been fed**, which is exactly the primitive for "several
separate things must all still be alive". Two are used:

| Channel | Fed when |
| --- | --- |
| `loop` | the loop ran the feeder at all -- the check is a constant, the *calling* is the evidence |
| `wire` | the CDC workqueue answered a poke within four seconds |

The second exists because Zephyr's CDC ACM does not call our handler from an
interrupt. It calls it from a workqueue, so "the loop is going round" and "the
USB side is going round" are two claims and a box can lose either alone. A
thread that is idle by design cannot be watched by asking whether it has run
lately -- the honest answer is no -- so `src/watchdog.h`'s `ProbeLiveness`
asks it to run and requires an answer. Enabling the transmit interrupt makes
the driver queue our handler whether or not there is anything to send.

**Once started it cannot be stopped or reconfigured until the chip resets.**
No turning it off for a moment, no lengthening the timeout because something
is taking a while. So it starts last, after the radio is up and the daemon is
running, and nothing before that point is watched -- which is the right trade,
because a box that never finishes booting is not one a reset would help. The
timeout is twelve seconds, deliberately generous: the two mistakes are not
equally bad. A reset that fires on a working dongle takes the radio off the
air in the middle of a shoot; one that fires ten seconds late clears a wedge
that would otherwise have lasted until a person noticed.

A watchdog reset leaves no record of its own, because nothing is running at the
moment it happens -- the part allows about sixty microseconds between the
timeout and the reset. So the feeder writes down which check is failing every
time it runs, into the same retained memory as the fault record, and the reset
cause comes from `hwinfo`. The next boot says *which* thing stopped, not merely
that something did.

### Still not verified on hardware

The whole of the above is reasoned from the datasheet, the driver source and
the built ELF. What is checked: `CONFIG_WATCHDOG` and `CONFIG_HWINFO` are on,
`WDT_NRFX` and `HWINFO_NRF` are the drivers selected, the `watchdog0` alias
exists on this board, and `start_watchdog` and `note_watchdog_state` are linked
in. `src/watchdog.h`'s judgement half is covered by `tests/test_watchdog.cc`,
including the case that matters most -- a slow answer is not a failure, because
a false positive here reboots a working dongle.

Not checked: that the timer actually fires, that a starved channel actually
resets the part, that the retained note survives a watchdog reset (it survives
`SYSRESETREQ`; a watchdog reset is not quite the same path), and that
`RESET_WATCHDOG` comes back from `hwinfo` on this silicon. Every one of those
needs a dongle and a deliberate wedge.

## The image that never enumerated, and the guard that came out of it

The second image flashed to this dongle did not come up. Not a wedge -- the
earlier failure at least kept the device on the bus -- but absent entirely: no
`/dev/cu.usbmodem*`, nothing under `ioreg -p IOUSB`, and no flicker when polled
twice a second for ten seconds, so not a fast reboot loop either.

The board target was checked first, because
[the regulator trap](#) above has cost a day before and looks exactly like
this. It was correct: `CONFIG_BOARD_TARGET="raytac_mdbt50q_cx_40_dongle/nrf52840"`.

What narrows it is init ordering. `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT`
brings USB up from a `SYS_INIT` at `APPLICATION` level -- *before* `main()`. So
a device that never appears on the bus failed before that, which rules out
everything in `main()`: the watchdog, the fault report, the daemon, the radio.
A config diff against the last image known to enumerate left three lines, and
of everything in that image only one adds code before USB comes up:
`CONFIG_USBD_CDC_ACM_WORKQUEUE`, which starts a cooperative-priority thread
from a `POST_KERNEL` `SYS_INIT`.

**That is an inference, not a proof, and it is recorded as the suspect rather
than the cause.** A board that is failing to enumerate cannot be instrumented,
which is the whole difficulty; the option is dropped because the reason for
wanting it -- catching a starved CDC workqueue -- is now covered better by the
watchdog, which catches it *and says so*.

### A box that cannot boot puts itself in DFU

The expensive part of all this was never the debugging. It was that every
attempt cost a person walking to the desk to hold a button down while plugging
the dongle in, because an image that does not reach USB leaves no way in at
all.

`octo_boot_guard` in `firmware/src/faultlog.cc` ends that. It counts
consecutive boots that never got anywhere, in memory that survives a reset, and
a box that has failed four times in a row stops trying and waits in its
bootloader instead -- where it can be reflashed over the cable. A bad image now
costs a reflash rather than a trip to the desk.

It is registered at `PRE_KERNEL_1` with priority 0, which puts it at
`__init_PRE_KERNEL_1_start` -- the first init entry in the image, ahead of every
driver. That placement is the whole point: it is what makes the guard cover a
hang in driver init, which is the case that motivated it. Verified in the ELF
rather than assumed.

The count is cleared when a run has been up for thirty seconds
(`mark_run_settled`), and again on the way into DFU, so a dongle sitting in its
bootloader that is simply replugged gets a fresh set of attempts rather than
going straight back. A power cycle clears it too, for free: the memory does not
survive losing power.

What this does not cover: an image that hangs without ever faulting *and*
without ever resetting. The guard counts boots, so something has to end the
boot for it to see anything. A hang before the watchdog starts is still a
button.

## Be reachable first

The single most useful thing learned from all of the above, and it is an
ordering rule rather than a fix.

A box with no console can only explain itself to a host that has managed to
open its port. `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` brings USB up from a
`SYS_INIT` at `APPLICATION` level, before `main()` runs -- so the box is
*capable* of being a serial port well before it does anything interesting. An
image that then faults while bringing the radio up never finishes enumerating,
cannot say that is what it was doing, and from the far end is indistinguishable
from a dead cable.

So `main()` now becomes a port and starts the loop, and everything that might
not survive -- the radio, the scanner, the sync cycle, the watchdog, even the
floating-point self-check -- is deferred by two seconds onto that loop. A
couple of seconds of being nothing but a serial port is the difference between
a box that fails and a box that fails *and says so*.

Safe mode is the second half. The boot guard already counts consecutive boots
that did not last; when that count is above one, the deferred work is skipped
entirely. So the sequence a broken image produces is:

1. first attempt: normal start, dies doing whatever it dies doing
2. second attempt: safe mode -- a port, a greeting, and the fault record from
   the first attempt, with none of the code that failed having run
3. after four: the bootloader, reflashable over the cable

That is three chances to learn something, where before there was a dongle
that was simply absent from the bus.

### What the bootloader window is worth knowing

Measured on this dongle, by watching `/dev/cu.usbmodem*` at fifty hertz across
a software-triggered reboot:

```
  0.00s  the application's port
  1.85s  (none)              -- the reboot
  2.81s  the bootloader's port
 14.96s  (none)              -- the bootloader gives up
 16.12s  the application's port again
```

So there are **about twelve seconds** to get a transfer started, and the
bootloader's serial number differs from the application's, so the port to use
is the one that was not there before.

Twelve seconds is not obviously tight, and the reason it is: `nrfutil` against
a port it cannot open takes **forty seconds** to give up, at almost no CPU --
it is retrying, not starting. So an attempt that misses the opening does not
fail fast, it fails long after the window has closed, and reports
`FileNotFoundError` on a path that certainly existed when the script looked.
That reads as a lost race and gives no hint that the tool was the slow part.

`--replace` does the whole handoff as one operation for this reason: ask the
dongle into DFU, poll at twenty hertz for a port that was not there before, and
go straight to the flasher with it. Nothing may be added between those steps.
An earlier version reached the flasher through an `ioreg` check that took about
a second and worked; a later one had an explicit `sleep 1` and did not. It is
that marginal, and it will sometimes lose.

**The button is still the reliable route**, and that is what it is for: DFU
entered by holding the button stays open indefinitely rather than for twelve
seconds. Use `--replace` for the ordinary case and the button when it matters.
