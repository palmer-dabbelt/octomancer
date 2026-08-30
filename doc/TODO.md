# What has not been built at all

This file is for work that does not exist yet. What exists but has not been
watched running is in `doc/KNOWN_ISSUES.md`, and keeping the two apart is the
point of having both.

Everything below is one project: making the Nordic dongle a standalone box that
does the sync itself, reachable from the Mac over USB or BLE. The design, the
reasoning and the order of work are in **`doc/box-notes.md`**; this is only the
list, so that the two do not drift.

Done so far: the event loop (`src/loop.{h,cc}`), the message codec
(`src/boxmsg.{h,cc}`), the radio path de-threaded onto the loop
(`src/hcilink.*`, `src/camhci.h`, `src/camera_hci.cc`, `src/scanner_hci.cc`),
the sync daemon itself (`src/syncd.{h,cc}`, with `src/boxsock.{h,cc}` for the
socket and `src/scanbridge.{h,cc}` for the one place another thread's work
becomes this thread's), and one HCI link shared between the scanner and the
camera (`src/hcishare.{h,cc}`) -- which is what makes a one-radio box possible
at all.

Still to write, roughly in order:

1. **The persistence record formats** — bonds, and the roster of Tentacle
   devices seen on the network. Portable, tested, no filesystem: the box has
   NVS and nothing else, and nothing is logged to it.
2. **The boot-time delay retune.** Nothing is stored, so the Blackmagic write
   delay is measured afresh every boot, starting from a default baked into the
   firmware. `unknown` → `provisional` → `converged`, described in
   `doc/box-notes.md`. The daemon has the states' natural home now -- it is
   the thing that judges every write -- but does not implement them.
3. **A camera backend for the Mac.** `src/camasync.h` is the interface the
   daemon drives; the only implementation is the dongle's. CoreBluetooth's is
   `CameraLink`, which blocks by contract. Until this exists, a Mac daemon
   with no dongle listens and answers and syncs nothing, and says so.
4. **The control and state daemon.** Mac only. Aggregates any number of sync
   daemons and serves the window, the TUI and the command line. Replaces the
   `octomancerd` / `octomancer-sync` split, which `doc/box-notes.md` records as
   an artifact of reverse engineering rather than a design goal. Nothing speaks
   the box protocol yet except a test and whatever is typed into a socket by
   hand.
5. **Standalone firmware over USB.** `hciport_zephyr.cc`, `loop_zephyr.cc` and
   a CDC console. USB before BLE, so the protocol is debugged over a transport
   that cannot itself be the thing that is broken.
6. **BLE control, and the status broadcast.** Deliberately unsecured, like the
   Tentacles.
7. **MCUboot and A/B firmware update.** Needs `+mcuboot` in
   `third_party/.west/config` and a `west update` first. The flash map already
   ships with the board.

One thing that is not on this list but blocks part of it, in
`doc/KNOWN_ISSUES.md`: the GATT discovery walk inside `HciCamera` was rewritten
and has never run. What *has* now run is its scan, which the sync daemon drives
over a real dongle every cycle -- so `HciCamera` is no longer code that nothing
calls.
