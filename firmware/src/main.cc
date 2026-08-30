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

#include "boxadmin.h"
#include "faultlog.h"
#include "boxclock.h"
#include "boxmsg.h"
#include "cdcpeer.h"
#include "loop.h"
#include "registry.h"
#include "scanner_zephyr.h"
#include "syncd.h"

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

  // Why the last run ended, and whether this build can print the numbers the
  // protocol is made of. Both are answered once, at boot, and told to whoever
  // attaches -- a box with no console can only report a problem to somebody
  // who is listening, and the first host to open the port is the first
  // opportunity there has ever been.
  const octo::FaultRecord last_fault = octo::take_last_fault();
  const std::string fault_line = octo::describe_fault(last_fault);
  const bool floats_ok = octo::can_format_doubles();

  peer.on_open([&daemon, &peer, &fault_line, floats_ok]() {
    daemon.peer_opened(&peer);
    auto say = [&peer](const std::string& text) {
      octo::Message msg;
      msg.verb = "say";
      msg.set("text", text);
      peer.send(octo::encode(msg));
    };
    if (!fault_line.empty()) say(fault_line);
    // The failure this catches is silent from both ends: every number in every
    // message comes out empty and nothing reports an error. Saying so is the
    // difference between one line and another evening.
    if (!floats_ok) {
      say("this build cannot format floating point -- every number in this "
          "protocol will be empty (CONFIG_PICOLIBC_IO_FLOAT)");
    }
  });
  peer.on_close([&daemon, &peer]() { daemon.peer_closed(&peer); });
  peer.on_line([&daemon, &peer](const std::string& line) {
    // Two verbs mean something only on a box -- see firmware/src/boxadmin.h.
    // Everything else, including every malformed line, is the daemon's.
    if (handle_box_admin(line, &peer)) return;
    daemon.peer_line(&peer, line);
  });

  std::string err;
  if (!peer.start(&err)) {
    // Nothing can be reported: the thing that failed is the only way to
    // report anything. The LED is the whole diagnostic, so make it say
    // something distinct rather than pretending to run.
    if (g_led.port != nullptr) gpio_pin_configure_dt(&g_led, GPIO_OUTPUT_ACTIVE);
    return 1;
  }

  // The controller, brought up here rather than in the scanner because the
  // camera client will want the same one. A radio switched on twice is a radio
  // nobody owns.
  const int rc = bt_enable(nullptr);
  if (rc != 0) {
    daemon.set_radio_state("unsupported");
  }

  std::unique_ptr<octo::Scanner> scanner;
  if (rc == 0) {
    scanner = octo::make_zephyr_scanner(
        loop.get(), &clock,
        [&daemon](const octo::Advert& a) { daemon.observe_advert(a); },
        [&daemon](const octo::Sighting& s) { daemon.observe_camera(s); },
        [&daemon](const std::string& state) { daemon.set_radio_state(state); });
    if (!scanner->start(&err)) {
      // start() has already reported the state through the handler above, so
      // the roster says "poweredOff" rather than staying "unknown" -- which is
      // the distinction octomancerd needs to tell a broken radio from one that
      // has not answered yet.
    }
  }

  daemon.start();

  if (g_led.port != nullptr) {
    gpio_pin_configure_dt(&g_led, GPIO_OUTPUT_INACTIVE);
    // Idle is an even flash; attached is lit with a short wink in it. Two
    // states that look different across the room, which is the whole job: the
    // questions a person standing over the dongle has are "is it running" and
    // "did the Mac find it", and there is no console to answer either.
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
