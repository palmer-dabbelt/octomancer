# octomancer

Synchronise a Blackmagic camera's timecode with a Tentacle Sync, using a Mac as
the proxy in the middle.

Right now this is a spike, not an app: one script that probes what a Blackmagic
camera will and won't let us do over Bluetooth LE.

## Setup

```
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

macOS will ask for Bluetooth permission the first time. Until it's granted the
scan hangs rather than failing, so grant it before wondering why nothing
happens.

## Use

```
# what's in range? (a service-uuid match is real, a name guess often isn't)
.venv/bin/python scripts/timecode_probe.py --scan-only
.venv/bin/python scripts/timecode_probe.py --scan-only --all

# watch a camera's timecode and telemetry without writing anything
.venv/bin/python scripts/timecode_probe.py --name <address> --watch 20

# try to set the clock
.venv/bin/python scripts/timecode_probe.py --name <address> --method both

# prove whether the camera obeys control writes at all
.venv/bin/python scripts/timecode_probe.py --name <address> --control-test

# try writing timecode straight to the Timecode characteristic
.venv/bin/python scripts/timecode_probe.py --name <address> --tc-char-test

# just show the packet bytes, no Bluetooth
.venv/bin/python scripts/timecode_probe.py --dry-run
```

`scripts/test_packets.py` checks the packet encoder against the six worked
examples printed on p105 of the Blackmagic documentation, and needs no hardware:

```
.venv/bin/python scripts/test_packets.py
```

## What we know so far

Tested against a **Pocket Cinema Camera 6K Pro**:

* Reading timecode over BLE works well — time-of-day, continuous, ~7.5 Hz.
* **Setting timecode over BLE does not work.** Both the documented Real Time
  Clock (group 7.0) and the undocumented timecode parameter (9.4) are accepted
  by GATT and then ignored.
* This isn't our bug: a white balance write over the identical path takes effect
  and is echoed back. Group 7 is simply absent on this body.
* The Timecode characteristic is **read-only**: it advertises `notify` alone,
  and writes to it are rejected with GATT `Write Not Permitted` whatever the
  payload. That closes the other BLE pipe -- and it's a firmer no than the SDI
  tunnel's silent ignore, since no encoding could change the outcome.
* This body runs time-of-day timecode in **local time**, not UTC.

`doc/protocol-notes.md` has the packet format, the UUIDs, the several places the
official documentation disagrees with the hardware, and where to go next.

One trap worth repeating: **Tentacle Sync boxes are named after the camera they
are attached to**, so a device advertising as `BMPCC` is quite likely a Tentacle.
Match on the service UUID and confirm the manufacturer string.
