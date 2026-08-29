# What has not been built at all

This file is for work that does not exist yet. What exists but has not been
watched running is in `doc/KNOWN_ISSUES.md`, and keeping the two apart is the
point of having both.

Everything below is one project: making the Nordic dongle a standalone box that
does the sync itself, reachable from the Mac over USB or BLE. The design, the
reasoning and the order of work are in **`doc/box-notes.md`**; this is only the
list, so that the two do not drift.

Done so far: the event loop (`src/loop.{h,cc}`), the message codec
(`src/boxmsg.{h,cc}`), and the radio path de-threaded onto the loop
(`src/hcilink.*`, `src/camhci.h`, `src/camera_hci.cc`, `src/scanner_hci.cc`).

Still to write, roughly in order:

1. **The persistence record formats** — bonds, and the roster of Tentacle
   devices seen on the network. Portable, tested, no filesystem: the box has
   NVS and nothing else, and nothing is logged to it.
2. **The boot-time delay retune.** Nothing is stored, so the Blackmagic write
   delay is measured afresh every boot, starting from a default baked into the
   firmware. `unknown` → `provisional` → `converged`, described in
   `doc/box-notes.md`.
3. **The sync daemon.** The tight loop — timecode messaging, forwarding
   announcements, speaking the control protocol — built to run unchanged on the
   Mac and on the box. This is also the first thing that will drive
   `octo::HciCamera`, which has never been run.
4. **The control and state daemon.** Mac only. Aggregates any number of sync
   daemons and serves the window, the TUI and the command line. Replaces the
   `octomancerd` / `octomancer-sync` split, which `doc/box-notes.md` records as
   an artifact of reverse engineering rather than a design goal.
5. **Standalone firmware over USB.** `hciport_zephyr.cc`, `loop_zephyr.cc` and
   a CDC console. USB before BLE, so the protocol is debugged over a transport
   that cannot itself be the thing that is broken.
6. **BLE control, and the status broadcast.** Deliberately unsecured, like the
   Tentacles.
7. **MCUboot and A/B firmware update.** Needs `+mcuboot` in
   `third_party/.west/config` and a `west update` first. The flash map already
   ships with the board.

Two things that are not on this list but block parts of it, both in
`doc/KNOWN_ISSUES.md`: nothing drives the dongle's camera half today, and the
GATT discovery walk inside `HciCamera` was rewritten and has never run.
