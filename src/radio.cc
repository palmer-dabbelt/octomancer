#include "radio.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "hciport.h"

namespace octo {
namespace {

bool truthy(const char* v) {
  if (!v || !*v) return false;
  return std::strcmp(v, "0") != 0 && std::strcmp(v, "no") != 0 &&
         std::strcmp(v, "false") != 0;
}

}  // namespace

// Deliberately does not open the port: this is asked during argument parsing
// and on every factory call, and opening one to answer it would reset the
// controller out from under a running scan.
bool dongle_selected() {
  const RadioOptions& opts = radio_options();
  if (opts.kind == RadioKind::kDongle) return true;
  if (opts.kind != RadioKind::kAuto) return false;
  if (!opts.device.empty()) return true;  // named: take the caller's word
  return !hci::list_candidate_ports().empty();
}

bool dongle_requested() {
  const RadioOptions& opts = radio_options();
  return opts.kind == RadioKind::kDongle || !opts.device.empty();
}

RadioOptions& radio_options() {
  static RadioOptions opts;
  return opts;
}

bool parse_radio_kind(const std::string& text, RadioKind* out) {
  if (!out) return false;
  if (text == "auto") {
    *out = RadioKind::kAuto;
  } else if (text == "corebluetooth" || text == "mac" || text == "apple") {
    *out = RadioKind::kCoreBluetooth;
  } else if (text == "dongle" || text == "hci" || text == "nrf") {
    *out = RadioKind::kDongle;
  } else {
    return false;
  }
  return true;
}

const char* radio_kind_name(RadioKind kind) {
  switch (kind) {
    case RadioKind::kAuto: return "auto";
    case RadioKind::kCoreBluetooth: return "corebluetooth";
    case RadioKind::kDongle: return "dongle";
  }
  return "auto";
}

bool radio_options_from_env(std::string* err) {
  RadioOptions& opts = radio_options();
  if (const char* v = std::getenv("OCTOMANCER_RADIO")) {
    if (*v && !parse_radio_kind(v, &opts.kind)) {
      if (err) {
        *err = std::string("OCTOMANCER_RADIO=") + v +
               " is not one of auto, corebluetooth, dongle";
      }
      return false;
    }
  }
  if (const char* v = std::getenv("OCTOMANCER_DONGLE")) {
    if (*v) opts.device = v;
  }
  if (truthy(std::getenv("OCTOMANCER_HCI_TRACE"))) opts.trace = true;
  if (const char* v = std::getenv("OCTOMANCER_PASSKEY")) {
    if (*v) {
      char* end = nullptr;
      long n = std::strtol(v, &end, 10);
      if (end == v || *end != '\0' || n < 0 || n > 999999) {
        if (err) {
          *err = std::string("OCTOMANCER_PASSKEY=") + v +
                 " is not a six-digit number";
        }
        return false;
      }
      opts.passkey = static_cast<int>(n);
    }
  }
  return true;
}

std::string describe_radio() {
  const RadioOptions& opts = radio_options();
  if (dongle_selected()) {
    if (!opts.device.empty()) return "dongle at " + opts.device;
    std::vector<std::string> ports = hci::list_candidate_ports();
    if (!ports.empty()) return "dongle at " + ports.front();
    return "dongle (none found)";
  }
#ifdef OCTO_HAVE_COREBLUETOOTH
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
