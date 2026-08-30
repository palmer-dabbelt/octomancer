#include "radio.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "hciport.h"

namespace octo {

// Deliberately does not open the port: this is asked during argument parsing
// and on every factory call, and opening one to answer it would reset the
// controller out from under a running scan.
//
// list_candidate_ports() is only reached when the answer actually turns on it,
// which on a Mac is never -- so the common path no longer walks /dev at all.
bool dongle_selected() {
  const RadioOptions& opts = radio_options();
  const bool named = !opts.device.empty();
  if (opts.kind == RadioKind::kDongle || named) return true;
  if (opts.kind != RadioKind::kAuto) return false;
  if (have_host_radio()) return false;
  return choose_dongle(opts.kind, named, false,
                       !hci::list_candidate_ports().empty());
}

bool dongle_requested() {
  const RadioOptions& opts = radio_options();
  return opts.kind == RadioKind::kDongle || !opts.device.empty();
}

std::string describe_radio() {
  const RadioOptions& opts = radio_options();
  if (opts.kind == RadioKind::kFake) {
    return "a fake bench -- no radio is in use (" +
           (opts.fake.empty() ? std::string("the standard bench")
                              : opts.fake) +
           ")";
  }
  if (dongle_selected()) {
    if (!opts.device.empty()) return "dongle at " + opts.device;
    std::vector<std::string> ports = hci::list_candidate_ports();
    if (!ports.empty()) return "dongle at " + ports.front();
    return "dongle (none found)";
  }
#ifdef OCTO_HAVE_COREBLUETOOTH
  // Naming the second radio, when there is one. Silence here is what made the
  // 2026-08-30 outage take a morning to find: the daemon had quietly changed
  // radios and every line it printed afterwards was the same as before.
  //
  // The wording is program-neutral, and has been got wrong twice. First it
  // said "pass --radio dongle to use it", which recommends collapsing the two
  // radios back into one. Then it said the dongle was "octomancerd's to
  // reach", which reads absurdly when the program printing it *is*
  // octomancerd -- this function is shared by every binary here and cannot
  // refer to any of them.
  //
  // So it states the fact and leaves the reader to it: there is a second
  // radio, it is not being driven from here, and there is a flag if driving it
  // from here is what you actually meant. The flag is mentioned as a
  // parenthesis rather than as advice, because it is scaffolding -- see
  // "Two radios, and which program knows" in doc/box-notes.md.
  if (opts.kind == RadioKind::kAuto) {
    std::vector<std::string> ports = hci::list_candidate_ports();
    if (!ports.empty()) {
      return "CoreBluetooth; a dongle at " + ports.front() +
             " is a second radio and is left alone (--radio dongle drives it"
             " directly)";
    }
  }
  return "CoreBluetooth";
#else
  return "no radio: this host has no CoreBluetooth and no dongle";
#endif
}

// The scanner factory, keeping the name and signature it always had; all that
// has changed is that there is now something to choose between.
//
// The camera factory is deliberately in radio_camera.cc rather than here. A
// static library is linked an object at a time, and octomancerd asks for a
// scanner while never linking CoreBluetooth's camera half -- so one object
// defining both factories drags that half into a program with no use for it,
// and the build fails on a symbol nobody asked for.

std::unique_ptr<Scanner> make_ble_scanner(Scanner::AdvertHandler on_advert,
                                          Scanner::SightingHandler on_camera,
                                          Scanner::StateHandler on_state) {
  if (radio_options().kind == RadioKind::kFake) {
    return make_fake_scanner(std::move(on_advert), std::move(on_camera),
                             std::move(on_state));
  }
  if (dongle_selected()) {
    return make_hci_scanner(std::move(on_advert), std::move(on_camera),
                            std::move(on_state));
  }
#ifdef OCTO_HAVE_COREBLUETOOTH
  return make_corebluetooth_scanner(std::move(on_advert), std::move(on_camera),
                                    std::move(on_state));
#else
  // Not a Mac and no dongle. Returning nullptr is what every caller already
  // handles as "this host has no radio".
  (void)on_advert;
  (void)on_camera;
  (void)on_state;
  return nullptr;
#endif
}

}  // namespace octo
