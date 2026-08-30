// The radio choice, alone in a translation unit.
//
// Split out of radio.cc for the reason the note in that file gives about the
// camera factory: a static library is linked an object at a time, and radio.cc
// names make_corebluetooth_scanner. Anything that wants to know *which* radio
// would be chosen therefore had to link a CoreBluetooth backend to find out --
// which put the rule out of reach of a unit test, which is why the change of
// behaviour on 2026-08-30 was found by a bench going quiet rather than by the
// suite. There is no I/O and no factory here, so tests/test_radio.cc links it
// and nothing else.
#include <cstdlib>
#include <cstring>
#include <string>

#include "radio.h"

namespace octo {

namespace {

bool truthy(const char* v) {
  if (!v || !*v) return false;
  return std::strcmp(v, "0") != 0 && std::strcmp(v, "no") != 0 &&
         std::strcmp(v, "false") != 0;
}

}  // namespace

// The options themselves, and the parsing of them, live here rather than
// beside the factories for the same reason choose_dongle does: radio.cc
// names make_corebluetooth_scanner, so a test that only wanted to say which
// radio to use had to link a CoreBluetooth backend to say it. Nothing below
// touches a radio, a port or a file.

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
  } else if (text == "fake" || text == "none") {
    *out = RadioKind::kFake;
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
    case RadioKind::kFake: return "fake";
  }
  return "auto";
}

bool radio_options_from_env(std::string* err) {
  RadioOptions& opts = radio_options();
  if (const char* v = std::getenv("OCTOMANCER_RADIO")) {
    if (*v && !parse_radio_kind(v, &opts.kind)) {
      if (err) {
        *err = std::string("OCTOMANCER_RADIO=") + v +
               " is not one of auto, corebluetooth, dongle, fake";
      }
      return false;
    }
  }
  if (const char* v = std::getenv("OCTOMANCER_DONGLE")) {
    if (*v) opts.device = v;
  }
  if (const char* v = std::getenv("OCTOMANCER_FAKE")) {
    // Setting the bench selects the fake radio. Requiring both this and
    // OCTOMANCER_RADIO=fake would mean a spec that silently did nothing, which
    // is the failure that costs the most time here: the output of a run
    // against no bench looks like the output of a run against a real one that
    // heard nothing.
    opts.fake = v;
    if (opts.kind == RadioKind::kAuto) opts.kind = RadioKind::kFake;
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

bool have_host_radio() {
#ifdef OCTO_HAVE_COREBLUETOOTH
  return true;
#else
  return false;
#endif
}

// The whole rule, with nothing to look up. See radio.h for why `auto` no
// longer prefers a dongle it merely found.
bool choose_dongle(RadioKind kind, bool named, bool host_radio,
                   bool port_present) {
  if (kind == RadioKind::kDongle) return true;
  if (kind == RadioKind::kCoreBluetooth) return false;
  // Before the `named` check below, deliberately: OCTOMANCER_DONGLE may well
  // be set in a shell that is now running a fake, and a fake radio that opened
  // a real serial port would be the worst of both.
  if (kind == RadioKind::kFake) return false;
  // Naming a port is asking for it, whatever `kind` says: there is no reason
  // to give a path to a program and mean something else by it.
  if (named) return true;
  // kAuto with a radio of our own: use it, and leave the port alone. Not
  // because the port might be a breadboard -- see radio.h -- but because a
  // dongle is a second radio with its own sync daemon on it, and reaching
  // through it from here would be one program driving two radios while
  // another program drove neither.
  if (host_radio) return false;
  return port_present;
}

}  // namespace octo
