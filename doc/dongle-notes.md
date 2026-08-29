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
exactly as before; the choice is made at run time and defaults to whichever
radio is actually present. `octomancerd` and `octomancer-sync` did not change
shape — `make_ble_scanner()` and `make_camera_link()` kept their names and
their signatures, and `src/radio.cc` is the only thing that had to learn there
were two possibilities.

It is also not custom firmware. The dongle runs a stock Zephyr `hci_uart`
image, so there is no embedded code in this repository to maintain, and every
decision worth making is made in portable C++ that `make check` can exercise on
a machine with no radio in it.

## Getting an image onto it

`tools/flash-dongle.sh` documents and automates this; `--check` says what is
missing.

```
tools/flash-dongle.sh --check
tools/flash-dongle.sh --build                     # needs a Zephyr SDK
tools/flash-dongle.sh --package build-hci/zephyr/zephyr.hex
tools/flash-dongle.sh --flash  build-hci/hci_uart_dfu.zip
```

DFU mode is the small side button (SW1, next to the USB connector — not the one
on the end) held while plugging in. The red LED pulses slowly when the
bootloader is listening.

If you would rather not install a Zephyr toolchain, any prebuilt `hci_uart`
image for the `nrf52840dongle` board works; skip `--build` and start at
`--package`.

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

Nothing needs to be configured when there is exactly one dongle plugged in:

```
octomancerd                       # picks the dongle if one is there
octomancerd --radio corebluetooth # or force the Mac's own radio
octomancerd --dongle /dev/cu.usbmodem1101
```

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

Supply the passkey with `--passkey` or `OCTOMANCER_PASSKEY`. On a terminal you
will be prompted if you do not. Under launchd there is nobody to prompt, so an
unattended `octomancer-sync` needs the value configured or it will not be able
to write a clock — it will say so rather than failing quietly.

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

A dongle was plugged into a Mac on 2026-08-29 and `octomancer-zoom --scan 10
--trace` was run against it. The trace is the whole result:

```
hci -> 01030c00
octomancer-zoom: Reset: no answer from the controller
```

That is a `Reset` going out and nothing coming back, and it is the expected
answer for this dongle, because this dongle has never been flashed. It came up
as `/dev/cu.usbmodemC499F7F00D5B1`, and `ioreg` gives its USB product string as
`nRF52 USB CDC BLE Demo` from vendor `0x1915` -- Nordic's own demonstration
application, which offers a serial port and does not speak HCI over it.

So what this proves is smaller than it looks, and worth stating exactly:

| Verified against hardware | How |
|---|---|
| A dongle is found without being named | `list_candidate_ports()` picked the port out of `/dev` unaided |
| The port opens and is written | the `Reset` reached the wire; `--trace` printed it |
| Silence is reported as silence | the timeout said which command went unanswered, rather than hanging |

Everything past the first byte remains unverified. Nothing has yet parsed an
event, because nothing has yet sent us one.

The next step is flashing, and until that happens, a dongle in this state is
indistinguishable at the port from a broken one -- both are a serial device
that will not answer a `Reset`. If you meet that message, check the USB product
string before suspecting the code:

```
ioreg -l -w 0 | grep -A4 'Nordic'
```

`Zephyr HCI UART sample` is a flashed dongle. Anything else is not.
