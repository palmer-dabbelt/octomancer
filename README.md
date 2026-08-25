# octomancer

Synchronise a Blackmagic camera's timecode with a Tentacle Sync, using a Mac as
the proxy in the middle.

Two halves. `octomancerd` is a C++ background service that watches the Tentacle
Sync bench and tells you when a box has drifted; the Python in `scripts/` is the
research tool, and is where the camera side still lives.

## The service

```
./autogen.sh          # only from a git checkout
./configure --prefix=$HOME/.local
make && make check
```

Nothing but the C++ standard library, CoreBluetooth and AppKit -- there are no
third-party dependencies to install, and `make check` needs no hardware.

```
make install          # octomancerd + octomancerctl
make install-agent    # run it at login as a LaunchAgent
make install-app      # the menu-bar app, into ~/Applications
```

Check it works before installing anything:

```
./octomancerd --probe 15
```

which listens for fifteen seconds and prints what it heard:

```
octomancer  5 boxes, 5 live  radio poweredOn  up 15s  60 adverts
bench -6.205s vs this Mac,  spread +2.0ms across 5 live boxes

BOX               AGE  RSSI  TIMECODE             OFFSET     MEDIAN      DRIFT  RESOLUTION
BMPCC              0s   -43  22:19:02:16.038     -6.208s    -6.205s        ~4m  frame+us
Krysta             4s   -60  22:18:59.337        -6.205s    -6.207s        ~4m  microsecond
```

Once the agent is running, ask it what it can see:

```
octomancerctl              # one report
octomancerctl watch        # redraw until interrupted
octomancerctl json | jq .  # for everything that isn't this program
```

The service is **passive**: it never connects to a device and never writes to
one, so it cannot disturb the Tentacle app, a camera, or a recording. It also
never sets anyone's clock -- that stays in the Python daemon, because a service
that runs unattended should not also be able to act unattended.

It notifies you when a box drifts more than a minute from this Mac, which is
the signal to re-jam it in the Tentacle app. That judgement is made on a median
rather than a single reading, with hysteresis and three-observation
confirmation, so a box parked near the threshold cannot spam you.

`doc/service-notes.md` covers the architecture, the wire protocol, the
threading, and why drift is refused rather than estimated from short samples.

## The research scripts

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

# set the camera's clock from this Mac (this is the one that works)
.venv/bin/python scripts/set_rtc.py --name <address>
.venv/bin/python scripts/set_rtc.py --name <address> --bias 75 --no-calibrate

# prove the RTC write lands, by writing a deliberately wrong clock
.venv/bin/python scripts/timecode_probe.py --name <address> --rtc-test

# try to set the clock
.venv/bin/python scripts/timecode_probe.py --name <address> --method both

# prove whether the camera obeys control writes at all
.venv/bin/python scripts/timecode_probe.py --name <address> --control-test

# try writing timecode straight to the Timecode characteristic
.venv/bin/python scripts/timecode_probe.py --name <address> --tc-char-test

# just show the packet bytes, no Bluetooth
.venv/bin/python scripts/timecode_probe.py --dry-run

# read timecode from every Tentacle Sync box in range (passive, no pairing)
.venv/bin/python scripts/tentacle_scan.py 45
.venv/bin/python scripts/tentacle_scan.py 30 --raw

# pick a Tentacle to sync against, and see how far this Mac is from it
.venv/bin/python scripts/tentacle_ref.py 30

# record raw adverts for offline analysis
.venv/bin/python scripts/tentacle_capture.py 150 -o cap.jsonl
```

## Keeping the camera on Tentacle time

`scripts/octomancer_sync.py` ties the two halves together. It runs until
Ctrl-C, reads Tentacle time passively, and corrects the camera's clock when
that is both needed and allowed:

```
.venv/bin/python scripts/octomancer_sync.py                     # auto-detect
.venv/bin/python scripts/octomancer_sync.py --dry-run --poll 20
```

It will not touch the clock while the camera is **recording**, while the error
is already inside `--tolerance` (default 1 s), or once it looks like an
**external timecode source** owns the camera -- nothing in the protocol reports
that, so it is inferred from writes that do not take. The camera is polled once
a minute by default, because connecting is slow and intrudes on the operator.

Every cycle is logged to `octomancer-sync.jsonl`, including the ones where
nothing happened, so there is drift data to tune the tolerance and poll
interval against later.

Two things it learns rather than assumes: the camera's RTC offset (-75 s before
a power cycle, 0 after one, so a fixed value never converges), and whether the
Tentacle bench agrees with itself.

`scripts/sync_report.py` turns that log into the numbers you'd tune against:

```
.venv/bin/python scripts/sync_report.py octomancer-sync.jsonl
```

It reports drift only from free-running stretches between writes, and only
from stretches long enough to mean anything -- the camera reports whole frames,
so a half-minute sample yields four figures of pure quantisation noise. Expect
it to say "leave it running" until there's an hour or so of log.

`scripts/test_packets.py` checks the packet encoder against the six worked
examples printed on p105 of the Blackmagic documentation, and needs no hardware:

```
.venv/bin/python scripts/test_packets.py
```

## What we know so far

Tested against a **Pocket Cinema Camera 6K Pro**:

* Reading timecode over BLE works well — time-of-day, continuous, ~7.5 Hz.
* **Setting the Real Time Clock (group 7.0) over BLE works**, and on a camera in
  Time of Day mode the timecode follows it. So the Mac *can* put wall-clock time
  on the camera with no cable, to about ±1 s.
* Two corrections are needed to land the right time: write **UTC** and let the
  camera apply its own timezone, and add **~75 s**, because the clock lands that
  far behind the value written — repeatably, with no spread. `set_rtc.py`
  measures that offset and corrects it rather than assuming it.
* Setting the timecode *directly* still doesn't work: the undocumented 9.4
  parameter is accepted by GATT and ignored.

Tested against a bench of **five Tentacle Sync boxes**:

* **Reading their timecode works, passively.** They broadcast it in the BLE
  advertising payload as service data under UUID `FDAC` — no connection, no
  pairing, and every box in the room can be read at once.
* The timecode is **plain binary, not BCD** — the opposite of Blackmagic.
* Two payload formats: frame-resolution `HH MM SS FF`, and a microsecond-of-day
  counter on the Track E, which is the better sync reference.

`doc/tentacle-notes.md` has the byte layouts, the evidence for each, and what's
still unidentified.
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
