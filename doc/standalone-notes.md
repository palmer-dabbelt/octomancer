# Standalone octomancer

*Written 2026-08-27, as a handoff. No firmware exists yet; this is the plan,
the reasoning behind it, and the things already checked. Where something is a
recollection rather than a verified fact it is marked **(unverified)**.*

> **Superseded in part, 2026-08-29.** `doc/box-notes.md` is now the design:
> two daemons split by tempo rather than by capability, one control protocol
> over USB, BLE and a unix socket, and a single-threaded event loop with no
> blocking in it. Four things below have since been *measured*, and the answers
> were not the ones guessed here.
>
> * **Size.** The set the firmware links is about 99 KB, not 250–350 KB. See
>   the sizing block in `doc/box-notes.md`, which also gives the exact command
>   — the figures are the `size` *text* column, `.text` plus `.rodata`, and
>   mixing that up with the `.text` section alone moves everything by a third.
> * **Threads.** The toolchain's libstdc++ has no `std::thread` in any
>   multilib, so the shim below is not "the safer route", it is the only route
>   — and in the end it was not taken either. The radio was de-threaded
>   instead, which is why the item at "`hcilink.cc` uses `std::thread`" below
>   no longer describes the tree.
> * **The A/B flash map** did not need designing; the board already ships one.
> * **The port abstraction paid off somewhere unplanned.** `hciport.h` was
>   written to make the HCI host testable and never was, because the host
>   needed its reader thread to make progress. Removing the thread is what
>   finally made `tests/test_hcilink.cc` possible.
>
> An earlier version of this note said "the ESP32 fallback, the hardware
> question and what gets reused all still stands", which named three of the
> twelve headings below and left a reader to guess at the rest. Guessing is the
> failure mode this note is supposed to prevent, so here is every section:
>
> * **The hardware question, and why it is settled** — stands, and the ESP32
>   research in it is untouched. The board it named was wrong; that is
>   corrected in place below, with what the error cost.
> * **What already exists and gets reused**, and **One camera at a time** —
>   stand.
> * **The architecture** — the diagram is superseded. See the note under that
>   heading.
> * **The host-stack decision** and **Three roles at once** — stand.
>   `doc/box-notes.md` settles the shape of the daemons above the host, not the
>   host itself: `CONFIG_BT_HCI_RAW=y` with our own stack on top is still the
>   bet.
> * **Control, over two transports** — superseded. There are three transports
>   and the protocol now exists. See the note under that heading.
> * **Bond storage is now required** — stands, and `doc/box-notes.md` agrees
>   from the other end: bonds and the Tentacle roster are the only two things
>   that live in the box's flash.
> * **Firmware update** — the two DFU paths and the MCUboot chain still
>   describe the plan; the flash-budget arithmetic in it is superseded by the
>   measured 99 KB and by the partition table the board already ships, both
>   above. One thing in it is wrong rather than superseded: it describes
>   entering DFU by holding "SW1, the small side button next to the USB
>   connector, not the one on the end", which is Nordic's two-button PCA10059.
>   The Raytac has one button. `README.md` has the procedure that works on the
>   board we actually own, and `doc/dongle-notes.md` records that describing a
>   board we did not own was part of the day that was lost.
> * **Constraints worth knowing before designing** — stands, except for log
>   storage. `doc/box-notes.md` puts no log on the box at all: the drift log is
>   drained to the Mac over the control protocol, because that is where there
>   is a filesystem to keep it on.
> * **Toolchain, and a trap that already cost an hour** — superseded as a
>   description of this machine. Zephyr, the SDK and a working `nrfutil` are
>   now all under `third_party/` — the SDK and `nrfutil` fetched by
>   `tools/flash-dongle.sh --setup`, the Zephyr tree a pinned submodule that
>   `--setup` refuses to run without — and
>   `doc/dongle-notes.md` has the current procedure. The trap itself is still
>   real; the workaround written below is not the one that works.
> * **The order to do this in** — step 1 is half done (scanning over the dongle
>   works; connecting, pairing and writing a clock have still never been run
>   against hardware), step 2 is done, and the rest stands.

The goal: a box that does what the Mac does — read Tentacle timecode off the
air, decide whether a Blackmagic camera's clock needs setting, and set it —
with no Mac present. USB-C for power, for writing flash images, and for
control; Bluetooth for control and firmware update as well.

## The hardware question, and why it is settled

The dongle already bought is a **Raytac MDBT50Q-CX-40** — an nRF52840 module,
*not* Nordic's own PCA10059 — and it is not a peripheral: it is a complete SoC
with 1 MB of flash and 256 KB of RAM **(unverified: the RAM figure is from
memory. The 1 MB is not a guess any more — the partition table the board ships
sums to exactly that, see `doc/box-notes.md`)**. Running the whole program on
it is the plan. It is enclosed, it has a USB-C connector and nothing else, and
it is Nordic silicon, which is where we would rather be anyway.

**This section said PCA10059 until 2026-08-29, and that is the mistake that
cost a day.** Every nRF52840 dongle is the same chip, so an image built for
Nordic's board flashes onto the Raytac, verifies, reports success — and then
never appears on USB again, which is indistinguishable from an empty port.
Nordic's board file enables the high-voltage regulator and switches the core
supply to DC/DC, which needs external inductors the Raytac module does not
have, so the supply collapses during board init, before USB and before any LED
could say so. `doc/dongle-notes.md` has the whole diagnosis, and
`tools/flash-dongle.sh` defaults to `raytac_mdbt50q_cx_40_dongle/nrf52840`
because of it. Anything below that reasons from the board rather than the chip
should be read with that in mind.

An ESP32 was considered and is the fallback. What the research found, so it
does not have to be repeated:

* ESP-IDF exposes its controller over a **virtual HCI** interface —
  `esp_vhci_host_send_packet()` / `esp_vhci_host_register_callback()`, with an
  official `controller_vhci_ble_adv` example. Set the host to "Disabled" and
  the controller to "Enabled" in menuconfig and you get raw HCI in-process,
  which is exactly the seam this project already has. So the ESP32 port would
  be about as cheap as the Nordic one. Espressif's *vendor-specific* commands
  are reserved for their own host; standard HCI is fair game.
* ESP32-S3 allows **10 BLE instances total** (ADV + SCAN + connections), with
  `CONFIG_BT_ACL_CONNECTIONS` in the range 1–9, default 4. Far more than we
  need — see "one camera at a time" below.
* The boards worth buying if it ever comes to that: **LilyGO T-Display-S3**
  (ESP32-S3, 16 MB flash, 8 MB PSRAM, 1.9" 320×170 IPS, two buttons, ~$20–25)
  or **M5Stack CoreS3 SE** (ESP32-S3, 16 MB / 8 MB PSRAM, 2" *capacitive
  touch*, microSD, USB-C with OTG and CDC, ~$39, battery sold separately).

The ESP32's real advantage is a screen, and a screen matters for exactly one
thing: entering the six-digit passkey a Blackmagic camera displays when a
stranger tries to pair with it. The plan below solves that over the control
channel instead, which is why the screenless dongle is viable at all. If that
turns out to be miserable in practice, the CoreS3 SE is the answer, and the
port is mostly the same code.

## What already exists and gets reused

The split this project has always had turns out to be the whole port. Nothing
below has a radio in it, and nothing below is Mac-specific:

| Source | Lines | What it does |
|---|---|---|
| `src/tentacle.*` | ~220 | decodes the Tentacle advertising payload |
| `src/bmd.*` | ~520 | the Blackmagic SDI protocol, as bytes |
| `src/camsync.*` | ~880 | every decision about whether and when to write |
| `src/hci.*` | ~1370 | H4 framing, command and event codec, advertising data |
| `src/att.*` | ~1070 | L2CAP and ATT, both client and server roles |
| `src/crypto.*` | ~460 | AES-128, AES-CMAC, and the pairing key derivations |
| `src/smp.*` | ~500 | legacy pairing, as a state machine |
| `src/hcilink.*` | ~1170 | the stateful host: connections, credits, demux |

That is roughly 6,000 lines that should cross to the firmware unchanged, and —
this is the point — it is the part `make check` already exercises on a machine
with no radio in it. The dongle work is meant to *stay* in that shape: new
logic gets a host-side test, and only the glue is untestable.

What has to be replaced is small: `src/hciport_posix.cc` (a serial port), the
CoreBluetooth backends, and the process/filesystem assumptions in
`octomancerd.cc` and `octomancer-sync.cc`.

### One camera at a time

Worth stating because it collapses a lot of imagined difficulty:
`src/camera.h` is a **blocking, single-connection** interface — `connect(id)`,
subscribe, write, `disconnect()`. The daemon is a plain loop. So the firmware
needs one central connection at a time plus a scanner, not a fan-out. Whatever
the controller's connection limit is, we are nowhere near it.

## The architecture

**The diagram is superseded by `doc/box-notes.md`, 2026-08-29.** It draws the
inside of one box with a control service and a console hanging off it, and a
Mac only as a GATT client at the end of a wire. What is missing from it is the
Mac side having any structure at all. What settled instead is three layers: a
*sync daemon* that owns the radio — the same source built as Nordic firmware or
as a Mac process — a *control daemon* on the Mac that owns no radio and
aggregates several of them, and the interfaces above that. That
diagram is in `doc/box-notes.md` and is the one to read. The bottom half of
this one, our host speaking raw HCI in-process to the Zephyr controller, is
unchanged.

```
   Tentacle boxes  ──adverts──▶  ┌──────────────────────────────┐
                                 │  scanner  ──▶  camsync       │
   Blackmagic cam  ◀──GATT────   │     ▲            │           │
                                 │     │            ▼           │
   phone / Mac     ◀──GATT────   │  control svc   camera link   │
                                 │     ▲                        │
   USB-C           ◀──CDC─────   │  control console             │
                                 └──────────────────────────────┘
                                        │ raw HCI (in-process)
                                 ┌──────────────────────────────┐
                                 │  Zephyr controller           │
                                 └──────────────────────────────┘
```

### The host-stack decision

Zephyr can be used two ways here, and the choice determines everything else.

**Chosen: `CONFIG_BT_HCI_RAW=y` and our own host.** This is the same
configuration the stock `hci_uart` image uses — its `prj.conf` sets exactly
that, and the controller is exposed as raw HCI — except that instead of
bridging those packets out over the UART, the host runs on-chip.
`bt_enable_raw()` hands you a FIFO of incoming buffers and `bt_send()` pushes
outgoing ones **(unverified: check the current Zephyr API; the `hci_uart`
sample is the reference)**. The transport shim is then a new
`src/hciport_zephyr.cc` implementing the existing `octo::Port` byte-pipe by
prepending and stripping the H4 type byte. `hcilink.cc` and everything above
it does not change.

*This note said `hci_usb` in five places until 2026-08-29, and every one of
them was wrong.* `hci_usb` builds a USB Bluetooth *class* device
(`CONFIG_SERIAL=n`, `CONFIG_USBD_BT_HCI=y`) with no serial port on it at all;
Linux binds that with `btusb`, macOS has no driver for the class, and this
project talks to a serial port on both. `hci_uart` is what is actually
flashed, and the dongle's board file aims its UART at CDC ACM. The trap is
that the wrong sample is the one with the better name — see
`doc/dongle-notes.md`.

Why this way: one codebase, one set of tests, and the Mac build stays the
development environment for the hard parts. The alternative throws away
`att.cc`, `smp.cc` and `hcilink.cc` and rewrites the app against Zephyr's
`bt_gatt_*` API — a third radio backend, and the only one that cannot be
tested without hardware.

**The risk this takes on** is that our SMP becomes the only path to writing a
camera clock, and it has never met a camera. Specifically: it is legacy
pairing only. If Blackmagic insists on LE Secure Connections we must implement
it. That is tractable — `f4`, `f5`, `f6` and `g2` are already written and
tested in `crypto.cc`, and the nRF52840's controller supports the
`LE Read Local P-256 Public Key` and `LE Generate DHKey` commands via
`CONFIG_BT_CTLR_ECDH` **(unverified)** — but it is real work.

**This risk is resolved before any firmware is written.** The dongle in stock
`hci_uart` mode, driven from the Mac, answers it: if `octomancer-sync` can pair
with and write to a camera over the dongle, the firmware inherits a stack that
demonstrably works. If it cannot, we learn exactly why while we still have a
debugger, a screen and a packet trace. **Do that first.**

If our host proves to be the wrong bet, the fallback is Zephyr's own host,
which brings qualified SMP with Secure Connections, bonding in the settings
subsystem, and MCUmgr DFU for free — at the cost of a divergent code path.

### Three roles at once

The firmware is simultaneously an **observer** (scanning Tentacle adverts), a
**central** (connecting to a camera), and a **peripheral** (advertising its own
control service). Zephyr's open controller supports all three concurrently
**(unverified — confirm `CONFIG_BT_CTLR` role settings and that scanning
continues while a central link is up)**.

Note the pleasing accident: the peripheral half needs a GATT **server**, and
`att::ServerBuilder` and `att::Server` already exist — they were written for
`octomancer-zoom`, to host the profile the Zoom F6 was expected to connect to.
They get a second use here.

## Control, over two transports

**Superseded by `doc/box-notes.md`, 2026-08-29, except for the passkey.** There
are three transports and not two — USB CDC, a BLE characteristic and a unix
socket — because the same protocol turned out to be the right thing to speak to
a Mac-side daemon as well. The protocol itself is no longer a plan: the framing
is `src/boxmsg.h`, and `src/syncd.cc` answers `ping`, `hello`, `status`,
`devices`, `sync`, `source`, `announce` and `forget` over it today. The
transports mostly are, though. Only the unix socket exists; the serial port and
the characteristic are still the future tense, which is the point of having
written one message language for all three. Read the verb table in
`doc/box-notes.md` rather than the command table below.

What survives from here is the reason the channel exists at all: **no verb in
that protocol carries a passkey yet**, and until one does, a screenless box
cannot answer the six digits a camera puts on its own display. On the Mac that
is `--passkey` on a command line, which a box does not have.

The device has no screen and one button, so everything is done over a control
channel. The same command set is exposed twice:

* **USB CDC**, which is also how the dongle is powered. A line-oriented
  console, so a terminal is enough and no host tool is required.
* **A BLE control service**, so it can be configured on a rig without
  unplugging it.

Both should speak the same commands, and the parser should be one piece of
portable code with a host-side test. Commands the device actually needs:

| Command | Why |
|---|---|
| `status` | what it is doing, what it can hear, when it last wrote |
| `boxes` | Tentacle boxes in range, decoded |
| `cameras` | cameras seen, and which are enabled |
| `enable <id>` / `disable <id>` | `cameras.conf` has no filesystem here; writes stay off by default, as on the Mac |
| `passkey <id> <nnnnnn>` | **the reason this channel exists at all** |
| `log` | drain the drift log |
| `dfu` | reboot into firmware update |

The passkey flow: the camera displays six digits, the device reports "camera X
is asking for a passkey" over both channels and in the status LED, and someone
types `passkey X 123456`. With bond storage, once per camera, ever.

### Bond storage is now required

The Mac build pairs afresh on every connection and accepts the cost. A
screenless box cannot: it would demand a passkey every cycle forever. So the
firmware needs to keep LTKs across reboots, in Zephyr's settings/NVS, keyed by
peer address. This is new work with no Mac equivalent — and it is worth doing
carefully, because a stale bond that silently stops working is the nastiest
failure mode in this whole design. Make it inspectable (`bonds` command) and
clearable.

## Firmware update

Two paths, and they are not equally easy.

**Over USB** is nearly free. The dongle ships with Nordic's Open Bootloader,
which does USB CDC DFU — that is what `tools/flash-dongle.sh --flash` already
drives with `nrfutil`, and how the stock `hci_uart` image gets on there in the
first place. Entering it is SW1 (the small side button next to the USB
connector, **not** the one on the end) held while plugging in.

**Over Bluetooth** needs more. The dongle's stock bootloader is USB-only
**(unverified — Nordic's secure bootloader has ble/usb/uart variants and the
dongle ships the USB one)**, so BLE DFU means putting **MCUboot** in the chain:
Nordic bootloader → MCUboot → application. MCUboot can itself be installed
through the existing USB bootloader, so no debugger is needed.

The flash budget is the thing to check before committing to this. The
bootloader region occupies the top of flash **(unverified: 0xE0000–0x100000,
128 KB including settings pages)**, leaving roughly 892 KB from 0x1000. MCUboot
is perhaps 48 KB, and dual-slot then means two slots of ~420 KB. Our
application — a C++ BLE host with `std::string` and `std::map` — plausibly
fits in 250–350 KB, but that has never been measured. **Measure it before
designing around it.**

The reasoning was right, and the arithmetic was wrong everywhere it mattered.
The one guess that held up was the slot size — "two slots of about 420 KB"
against a measured 408 KB, which is close enough to have been useful. Measured on
2026-08-29: the application is about 99 KB, and the map did not have to be
designed at all — Zephyr's own board directory ships one, with the nRF5
bootloader at `0xF4000` where a Raytac keeps it rather than the `0xE0000`
guessed above, MCUboot at the bottom and two slots of 408 KB. It is in
`doc/box-notes.md`; use that and not the arithmetic here.

Because we control both ends, the BLE side does not need MCUmgr: a small image
transfer service on our own ATT server can write into slot 1 with Zephyr's
`flash_img_buffered_write()` and then set the pending flag with
`boot_request_upgrade()` — both plain C APIs that do not require Zephyr's BLE
host, which matters given the `BT_HCI_RAW` decision above. Sign images and
refuse unsigned ones; MCUboot does the verification.

## Constraints worth knowing before designing

* **No battery, no RTC.** Power is USB only, and there is no
  battery-backed clock. This is fine: the Tentacle boxes are the time
  reference and are re-read every cycle, so the local clock only has to bridge
  seconds. But at boot the device has no idea what time it is and must say so
  rather than guess.
* **No external flash.** Whatever is left of the 1 MB is all the log storage
  there is. The drift log is the entire scientific value of this project, so
  it needs a compact binary circular format and a way to drain it — not the
  JSONL the Mac writes. **Settled differently, 2026-08-29:** because the
  scientific value is the reason to keep it, it is kept where there is a
  filesystem. `doc/box-notes.md` puts no log on the box at all and drains it
  to the Mac over the control protocol; flash holds bonds and the Tentacle
  roster and nothing else.
* **256 KB of RAM, shared with the controller.** The C++ core leans on
  `std::string`, `std::map` and `std::vector`. This is the single most likely
  thing to force a rewrite of otherwise-portable code. Measure early.
* ~~**`hcilink.cc` uses `std::thread`.**~~ **Done differently, 2026-08-29.**
  It no longer uses one: there is no shim and no thread, only the event loop.
  Zephyr's C++ support could not have provided `std::thread` anyway — see the
  note at the top.
* **Identifiers are addresses**, as with the dongle on the Mac — see
  `doc/dongle-notes.md`. A bench learned on the Mac is not recognised by the
  firmware.

## Toolchain, and a trap that already cost an hour

Nothing embedded is installed on this machine: no `west`, no Zephyr tree, no
ARM toolchain, no `nrfutil`. `cmake`, `ninja` and Homebrew are present.

**The trap:** the global git configuration rewrites every GitHub URL to SSH —

```
url.ssh://git@github.com.insteadof https://github.com
```

— and there is no SSH key in the agent environment. So *every* https clone
from GitHub fails with `Permission denied (publickey)`, including the one
`west init` does internally, and including ones whose URL you never typed.
(`git push` fails here too, but for the plainer reason that `origin` is an SSH
remote and the key is absent; a normal shell with an agent pushes fine.)
Either add a key, or override for the clone:

```
git -c url."https://github.com/".insteadOf=ssh://git@github.com clone ...
```

An attempt to install Zephyr on 2026-08-27 died on exactly this and left
`~/zephyrproject` holding only a Python venv and an empty `.west` stub —
about 70 MB, no Zephyr source, safe to delete. Homebrew did newly install
`gperf`, `ccache`, `libmagic` and ccache's dependencies (`blake3`, `hiredis`,
`xxhash`); `cmake`, `ninja`, `python@3.14`, `dtc` and `wget` were already
there. That venv also holds a working `nrfutil` 5.2.0 and `intelhex`, which
are the DFU tools, if it is not deleted first.

## The order to do this in

1. **Test the existing stack against real hardware.** Stock `hci_uart` on the
   dongle, `octomancer-zoom --scan 10` from the Mac, then a real camera pair
   and clock write with `octomancer-sync`. Everything below is built on the
   assumption that this works. **Half done, 2026-08-29:** the scan works and
   decoded a Blackmagic camera's adverts, and `doc/dongle-notes.md` has the
   table of exactly which lines that verified. Connecting, pairing and writing
   a clock have still never been run against hardware, so the SMP risk above is
   still open.
2. **Measure.** Build the portable core for ARM and find out what it costs in
   flash and RAM. If it does not fit, that changes the design, not the
   schedule. **Done, 2026-08-29:** about 99 KB of flash, in a slot of 408 KB.
   It fits. RAM has not been measured.
3. **Portable work, testable on the Mac today:** the thread/time shim, the
   control-command parser, the compact log format, the bond store as an
   abstract interface. All of it gets a test in `tests/`.
4. **Zephyr glue:** `hciport_zephyr.cc`, the app main loop, USB CDC console,
   LED status.
5. **The BLE control service**, on the existing `att::Server`.
6. **MCUboot and the image transfer service**, last, because it is the part
   that can brick the device and the part most easily done over USB instead.
