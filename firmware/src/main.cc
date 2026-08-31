// octomancer-sync, as firmware.
//
// The fourth host for src/syncd.h and the one it was shaped for. That header
// says the same object "runs as a process on this Mac and as firmware on the
// Nordic", and this file is the second half of that claim: it constructs a
// loop, a roster, a radio and a pipe, hands them to a SyncDaemon, and runs.
// Every decision about what to do with a Tentacle reading is in code that
// `make check` already exercised on a machine with no radio in it.
//
// What is different about this host, and only this host:
//
//   * There is no filesystem, so there is no cameras.conf and no camera
//     database. set_config(nullptr) is a supported state -- see
//     src/camconf.h's default, which is the same one.
//   * There is no wall clock. firmware/src/boxclock.h holds what the host
//     told us, and until it does, offsets are not computed against anything.
//   * There is no console. The CDC port carries the box protocol and nothing
//     else, so the daemon's chatter goes out as `say` messages on the same
//     wire -- which is what src/boxmsg.h means by "a terminal and a cable
//     should be enough to see what the box is doing".
//   * There is no camera client yet, so set_camera is never called. A daemon
//     with no camera "still hears boxes, still serves the roster, and still
//     says why it is not syncing anything" -- src/syncd.h. That is exactly
//     what this is for now, and doc/box-notes.md records what is missing.
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include <memory>
#include <string>
#include <vector>

#include "boxadmin.h"
#include "faultlog.h"
#include "hwwatchdog.h"
#include "boxclock.h"
#include "boxmsg.h"
#include "blepeer.h"
#include "cdcpeer.h"
#include "loop.h"
#include "registry.h"
#include "naming.h"
#include "scanner_zephyr.h"
#include "syncd.h"
#include "watchdog.h"

namespace {

// The port. The board points its console, its shell and its HCI-to-host UART
// at this same node; this firmware points nothing at it but the box protocol,
// because two writers on one wire is a protocol that cannot be parsed.
const struct device* cdc_uart() {
  return DEVICE_DT_GET(DT_NODELABEL(board_cdc_acm_uart));
}

// A sign of life, because there is no console to print one on. Slow while
// nobody is attached, quick once a host is: the two questions a person
// standing over the dongle actually has are "is it running" and "did the Mac
// find it".
const struct gpio_dt_spec g_led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {});

// The roster, sized for 256 KB of RAM rather than for a Mac.
//
// A Sample is sixteen bytes and the default cap is 8192 of them, which is 128
// KB for one device -- more memory than this whole machine has, for one box on
// a bench that usually has five. The host keeps the long history; this keeps
// enough to answer what the offset is now.
//
// The visible cost is drift: src/registry.h will not report ppm from an arm
// shorter than min_drift_span, and 240 samples of a box advertising about once
// a second is nothing like fifteen minutes. So the box reports offsets and the
// Mac reports drift, which is the division doc/box-notes.md already draws --
// "the storage limitations are on octomancer-sync".
constexpr size_t kBoxMaxSamples = 240;
constexpr double kBoxWindow = 900.0;

// The watchdog, in seconds. Generous on purpose: it is here to catch a machine
// that has stopped, not to police latency, and the two mistakes are not equally
// bad. A reset that fires on a working dongle takes the radio off the air in
// the middle of somebody's shoot; one that fires ten seconds late clears a
// wedge that would otherwise have lasted until a person noticed. See
// firmware/src/hwwatchdog.h for why this cannot be adjusted afterwards.
constexpr double kWatchdogTimeout = 12.0;

// How often the USB side is asked to prove it can still run, and how long it
// may take to answer. The patience is well under the watchdog's own timeout so
// that a stuck wire is reported as a stuck wire rather than arriving at the
// same moment as the reset.
// How long the box spends being nothing but a serial port before it tries
// anything that might not survive. Long enough for USB enumeration to finish
// and for a host that is already watching to open the port; short enough that
// nobody waiting on a dongle notices.
constexpr double kQuietStart = 2.0;

constexpr double kWireProbePeriod = 3.0;
constexpr double kWirePatience = 4.0;

}  // namespace

int main() {
  auto loop = octo::make_loop();
  octo::BoxClock clock(loop.get());
  // So that wall_now() and std::chrono agree with it -- see the header.
  octo::install_box_clock(&clock);

  octo::Policy policy;
  policy.max_samples = kBoxMaxSamples;
  policy.window = kBoxWindow;
  octo::Registry registry(policy, loop->now());

  octo::SyncdOptions opt;
  // No camera client on this side yet, so there is nothing to hold open and
  // nothing to permit. Both are stated rather than left at a default, because
  // both become live decisions the moment the camera half lands.
  opt.hold = false;
  opt.default_writes = false;

  octo::SyncDaemon daemon(loop.get(), &registry, opt);
  daemon.set_wall_clock([&clock]() { return clock.wall(); });

  octo::CdcPeer peer(loop.get(), cdc_uart());
  // The same protocol over the air. Started later -- it needs the controller
  // up -- but constructed here so that everything with the program's lifetime
  // is in one place.
  octo::BlePeer air(loop.get());

  // Console chatter, onto the only wire there is. Never load-bearing -- a
  // client is free to ignore every `say` line -- but it is the difference
  // between watching the box work and guessing.
  daemon.on_say([&peer](const std::string& text) {
    octo::Message msg;
    msg.verb = "say";
    msg.set("text", text);
    peer.send(octo::encode(msg));
  });

  // The host telling us what time it is. Until this arrives the roster holds
  // readings with no offset against them, which is the honest state -- see
  // firmware/src/boxclock.h.
  daemon.on_settime([&clock](const octo::SyncDaemon::WallTime& t) {
    // Zone first. set() is what makes the clock known, and the registry starts
    // taking offsets the instant it is -- so adopting the instant before the
    // zone leaves a window whose readings are wrong by the whole offset.
    if (t.has_zone) clock.set_zone(t.zone);
    clock.set(t.wall);
  });

  // What this run needs to tell the first person who asks.
  //
  // A box with no console can only report a problem to somebody who is
  // listening, and the first host to open the port is the first opportunity
  // there has ever been -- possibly days after the thing being reported. So
  // these are gathered at boot and kept, rather than announced into a void.
  std::vector<std::string> boot_notes;

  // Why the last run ended. Empty when it ended cleanly, which is most of the
  // time and is worth staying quiet about.
  const std::string fault_line = octo::describe_fault(octo::take_last_fault());
  if (!fault_line.empty()) boot_notes.push_back(fault_line);

  // Safe mode: come up as a port and nothing else, when the last attempt did
  // not last.
  //
  // This is the answer to the problem that made the first evenings expensive.
  // A box that dies during startup cannot be asked anything -- it is not up
  // long enough to be asked -- so the only evidence is its absence, and every
  // theory about it costs somebody a trip to the desk with a paperclip. USB
  // comes up before main() does, though, which means a box that skips
  // everything risky can be a perfectly good port with a story to tell.
  //
  // So the second attempt in a row does exactly that: no radio, no scanner, no
  // cycle, no watchdog. Whatever killed the last run is not run again, and the
  // fault record from it is sitting in the greeting.
  const bool safe_mode = octo::boot_attempts() > 1;
  if (safe_mode) {
    boot_notes.push_back(
        "safe mode: the radio and the sync cycle are switched off because the"
        " last start did not last. Nothing here has run the code that failed."
        " Reflash, or power-cycle to try a normal start.");
  }

  // Whether this build can print the numbers the protocol is made of. The
  // failure it catches is silent from both ends: every number in every message
  // comes out empty and nothing reports an error. Saying so is the difference
  // between one line and another evening.
  if (!octo::can_format_doubles()) {
    boot_notes.push_back(
        "this build cannot format floating point -- every number in this"
        " protocol will be empty (CONFIG_PICOLIBC_IO_FLOAT)");
  }

  peer.on_open([&daemon, &peer, &boot_notes]() {
    daemon.peer_opened(&peer);
    for (const std::string& note : boot_notes) {
      octo::Message msg;
      msg.verb = "say";
      msg.set("text", note);
      peer.send(octo::encode(msg));
    }
  });
  peer.on_close([&daemon, &peer]() { daemon.peer_closed(&peer); });
  peer.on_line([&daemon, &peer](const std::string& line) {
    // Two verbs mean something only on a box -- see firmware/src/boxadmin.h.
    // Everything else, including every malformed line, is the daemon's.
    octo::PeerStats stats;
    stats.dropped_tx = peer.dropped_tx();
    stats.dropped_rx = peer.dropped_rx();
    stats.long_lines = peer.long_lines();
    if (handle_box_admin(line, &peer, stats)) return;
    daemon.peer_line(&peer, line);
  });

  // ...and exactly the same over Bluetooth. Nothing here decides which link is
  // in charge, and nothing here should: src/syncd.h keeps a list of peers and
  // announces to all of them, so a box with a cable and a radio link simply
  // has two. Whether to use the radio at all is a question only the host can
  // answer, because only the host knows whether it also has the cable -- see
  // want_bluetooth() in src/dongle.h.
  air.on_open([&daemon, &air]() { daemon.peer_opened(&air); });
  air.on_close([&daemon, &air]() { daemon.peer_closed(&air); });
  air.on_line([&daemon, &air](const std::string& line) {
    octo::PeerStats stats;
    stats.dropped_tx = air.dropped_tx();
    stats.dropped_rx = air.dropped_rx();
    stats.long_lines = air.long_lines();
    if (handle_box_admin(line, &air, stats)) return;
    daemon.peer_line(&air, line);
  });

  std::string err;
  if (!peer.start(&err)) {
    // Nothing can be reported: the thing that failed is the only way to
    // report anything. The LED is the whole diagnostic, so make it say
    // something distinct rather than pretending to run.
    if (g_led.port != nullptr) gpio_pin_configure_dt(&g_led, GPIO_OUTPUT_ACTIVE);
    return 1;
  }

  // Say something to whoever is attached right now, and keep it for whoever
  // attaches later. Both, because the interesting failures happen before a
  // person has got round to plugging the cable in.
  auto say_now = [&peer](const std::string& text) {
    octo::Message msg;
    msg.verb = "say";
    msg.set("text", text);
    peer.send(octo::encode(msg));
  };

  // Two checks, because there are two things that can stop separately. The
  // loop going round is the obvious one. The other is the USB side, which
  // Zephyr's CDC ACM runs on a workqueue rather than in an interrupt, and
  // which can therefore wedge while the loop is perfectly healthy -- a dongle
  // that still enumerates, still blinks, and will not open.
  octo::WatchdogPolicy guard;
  octo::ProbeLiveness wire(kWireProbePeriod, kWirePatience);
  // The loop's own check is a constant. That is not a tautology: it runs from
  // a loop timer, so the check being *called at all* is the evidence, and a
  // loop that has stopped stops feeding whatever it returns.
  guard.watch("loop", []() { return true; });
  guard.watch("wire", [&wire, &peer, &loop]() {
    bool poke = false;
    const bool ok = wire.poll(loop->now(), peer.wire_ticks(), &poke);
    if (poke) peer.probe_wire();
    return ok;
  });

  // ---------------------------------------------------------------------
  // Everything past this point is deferred, and the ordering is the point.
  //
  // Be reachable first. A box with no console can only explain itself to a
  // host that has managed to open its port, so the one thing worth doing
  // before anything else is becoming a port -- and then, only once the loop is
  // running and a host could be talking to it, doing the work that might not
  // survive. Radio, scanner, cycle, watchdog and even the self-check are all
  // work that might not survive.
  //
  // Done in the obvious order this cost two evenings: an image that faults
  // while bringing the radio up never enumerates properly, so it cannot say
  // that is what it was doing, and from the far end it is indistinguishable
  // from a dead cable. A couple of seconds of being nothing but a serial port
  // is the difference between a box that fails and a box that fails *and says
  // so*.
  // ---------------------------------------------------------------------
  std::unique_ptr<octo::Scanner> scanner;

  loop->after(kQuietStart, [&]() {
    // The self-check first, because it is the cheapest and its failure mode is
    // the most confusing: every number in every message silently empty.
    if (!octo::can_format_doubles()) {
      boot_notes.push_back(
          "this build cannot format floating point -- every number in this"
          " protocol will be empty (CONFIG_PICOLIBC_IO_FLOAT)");
      say_now(boot_notes.back());
    }

    if (safe_mode) {
      daemon.set_radio_state("off");
      return;
    }

    // The controller, brought up here rather than in the scanner because the
    // camera client will want the same one. A radio switched on twice is a
    // radio nobody owns.
    const int rc = bt_enable(nullptr);
    if (rc != 0) {
      daemon.set_radio_state("unsupported");
      say_now("the radio would not start");
    } else {
      std::string radio_err;
      scanner = octo::make_zephyr_scanner(
          loop.get(), &clock,
          [&daemon](const octo::Advert& a) { daemon.observe_advert(a); },
          [&daemon](const octo::Sighting& s) { daemon.observe_camera(s); },
          [&daemon](const std::string& state) { daemon.set_radio_state(state); });
      // Reachable over the air as well, now that there is a controller. After
      // the scanner rather than before it: listening is this box's job and
      // being talked to is a convenience, so if only one of them can be had,
      // it should be the listening.
      std::string air_err;
      if (!air.start(&air_err)) {
        say_now("the radio would not advertise -- this box is reachable only"
                " over USB");
      }
      if (!scanner->start(&radio_err)) {
        // start() has already reported the state through the handler above, so
        // the roster says "poweredOff" rather than staying "unknown" -- which
        // is the distinction octomancerd needs to tell a broken radio from one
        // that has not answered yet.
      }
    }

    daemon.start();
    // Only a run that actually tried the job may declare itself a success. A
    // safe-mode run staying up proves nothing about the code it skipped, and
    // letting it clear the count would hide a box that cannot start normally.
    octo::arm_settle_timer();

    std::string wdt_err;
    if (!octo::start_watchdog(loop.get(), &guard, kWatchdogTimeout, &wdt_err)) {
      // Not fatal. A dongle with no watchdog still does its job; it just
      // cannot get itself out of a wedge, which is the state it was in before
      // there was one at all. Said out loud rather than silently accepted.
      boot_notes.push_back("no watchdog: " + wdt_err);
      say_now(boot_notes.back());
    }
  });

  if (g_led.port != nullptr) {
    gpio_pin_configure_dt(&g_led, GPIO_OUTPUT_INACTIVE);
    // Idle is an even flash; attached is lit with a short wink in it. Two
    // states that look different across the room, which is the whole job: the
    // questions a person standing over the dongle has are "is it running" and
    // "did the Mac find it", and there is no console to answer either.
    // Ask the room what it is called, while anything in it is unnamed.
    //
    // A Tentacle keeps its clock in the advertisement and its name in the scan
    // response, so a passive radio -- which is what this was, always -- knows
    // the time exactly and cannot name a single box. Every device this dongle
    // reported was listed by its hardware address for that reason.
    //
    // Checked on a slow timer rather than per advertisement: the decision is
    // damped anyway (src/naming.h) and building a snapshot is the expensive
    // part of asking.
    octo::Loop* const lp = loop.get();
    loop->every(5.0, [&registry, &scanner, lp, &clock]() {
      if (!scanner) return;
      const octo::Snapshot snap = registry.snapshot(lp->now(), clock.wall());
      int unnamed = 0;
      for (const octo::DeviceSnapshot& d : snap.device) {
        if (d.live && octo::is_placeholder_name(d.name)) ++unnamed;
      }
      static bool active = false;
      static double changed_at = 0.0;
      const bool want =
          octo::want_active_scan(active, unnamed, lp->now() - changed_at);
      if (want == active) return;
      active = want;
      changed_at = lp->now();
      scanner->set_active(active);
    });

    loop->every(0.25, [&peer]() {
      static int tick = 0;
      ++tick;
      const int period = peer.attached() ? 8 : 4;
      const int phase = tick % period;
      const bool lit = peer.attached() ? phase != 0 : phase < 2;
      gpio_pin_set_dt(&g_led, lit ? 1 : 0);
    });
  }

  loop->run();
  return 0;
}
