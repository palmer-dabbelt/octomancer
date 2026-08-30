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
#include "radio.h"

namespace octo {

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
  // kAuto with a radio of our own: use it. A port might be a dongle and might
  // be a breadboard, and there is no way to tell without opening it -- which
  // this function must not do, being called during argument parsing and on
  // every factory call.
  if (host_radio) return false;
  return port_present;
}

}  // namespace octo
