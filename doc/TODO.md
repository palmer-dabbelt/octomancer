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
2. **The control and state daemon.** Mac only. Aggregates any number of sync
   daemons and serves the window, the TUI and the command line. Replaces the
   `octomancerd` / `octomancer-sync` split, which `doc/box-notes.md` records as
   an artifact of reverse engineering rather than a design goal. Nothing speaks
   the box protocol yet except the tests and whatever is typed into a socket
   by hand.

   Most of this item is not writing a new daemon. It is *un*building things
   that are in the way -- a daemon that owns a radio it should not, a
   connection pointing the wrong way, two protocols that cannot be bridged
   without inventing state. Those are enumerated and numbered under "The three
   layers, and where the code is not them yet" in `doc/KNOWN_ISSUES.md`, which
   is the order to work through.

   This has been moved ahead of the boot-time retune, which it used to sit
   behind, because it is the blocker for the whole layering. `octomancer-sync
   --daemon` is real: it listens on `octomancer-syncd.sock` (`src/boxsock.cc`)
   and speaks `src/boxmsg.h`. Nothing a person runs is a client of it.
   `octomancer` and the window each open the other two sockets themselves and
   merge the answers (`src/octomancer.cc`, `ui/main.mm`), and the shipped
   LaunchAgent starts the legacy mode instead.

   Be exact about what that does and does not block, because it is easy to
   overstate. It does *not* block the hardware verification the rest of `doc/`
   is waiting on: the daemon schedules its own cycles and writes them to the
   console and the log, so `octomancer-sync --daemon --radio dongle` in a
   terminal with a camera switched on would settle it with no client involved
   at all -- which is exactly how the four-cycle shared-radio measurement in
   `doc/KNOWN_ISSUES.md` was taken. What is missing there is a camera, not a
   daemon. What the absent layer *does* block is everything else: the daemon
   cannot be asked what it thinks, cannot be told to do anything, and cannot be
   configured, except by typing lines into a socket by hand. The retune below
   is a change inside that daemon which adds three states somebody has to watch
   and eventually override, and building it before there is anything to watch
   it with is the wrong order.
3. **Retiring the legacy `octomancer-sync`.** The cutover, once the daemon
   above exists. Three things go together: the blocking non-daemon mode, the
   control socket it serves (`octomancer-sync.sock`, in `src/control.cc`), and
   the thread that serves it, in `src/octomancer-sync.cc`. That thread is the
   last `std::thread` left in `src/`, checked by grep on 2026-08-29. Retiring
   it is therefore what finally makes the program single-threaded, and so
   portable to a libstdc++ that has no threads at all. Then
   `launchd/com.dabbelt.octomancer-sync.plist.in`'s `ProgramArguments` change
   from `--poll 60` to `--daemon`.

   The two modes take different lock files on purpose -- `octomancer-sync` and
   `octomancer-syncd` (`src/octomancer-sync.cc:119`, `:129`) -- so the new one
   can be run beside the old while it is being trusted. The cutover is what
   ends that arrangement, and it must not be done before item 5 below. On a Mac
   with no dongle the daemon says "no camera backend: listening and answering,
   not syncing" when it starts and "no camera backend on this host -- listening
   only" at every cycle, and it syncs nothing -- so flipping the plist first
   would silently stop a dongle-free Mac from setting any clock. (The similar
   "listening only, with no camera backend" is a *different* message, from the
   branch where a dongle was found and would not open. Worth keeping apart:
   they describe opposite situations.)
4. **The boot-time delay retune.** Nothing is stored, so the Blackmagic write
   delay is measured afresh every boot, starting from a default baked into the
   firmware. `unknown` → `provisional` → `converged`, described in
   `doc/box-notes.md`. The daemon has the states' natural home now -- it is
   the thing that judges every write -- but does not implement them.
5. **A camera backend for the Mac.** `src/camasync.h` is the interface the
   daemon drives; the only implementation is the dongle's. CoreBluetooth's is
   `CameraLink`, which blocks by contract. Until this exists, a Mac daemon
   with no dongle listens and answers and syncs nothing, and says so.
6. **Standalone firmware over USB.** `hciport_zephyr.cc`, `loop_zephyr.cc` and
   a CDC console. USB before BLE, so the protocol is debugged over a transport
   that cannot itself be the thing that is broken.
7. **BLE control, and the status broadcast.** Deliberately unsecured, like the
   Tentacles.
8. **MCUboot and A/B firmware update.** Needs `+mcuboot` in
   `third_party/.west/config` and a `west update` first. The flash map already
   ships with the board.

One thing that is not on this list but blocks part of it, in
`doc/KNOWN_ISSUES.md`: the GATT discovery walk inside `HciCamera` was rewritten
and has never run. What *has* now run is its scan, which the sync daemon drives
over a real dongle every cycle -- so `HciCamera` is no longer code that nothing
calls.
