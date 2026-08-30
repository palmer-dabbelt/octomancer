// Which radio a program uses, and how it is allowed to change.
//
// The property being pinned is a statement about the system's shape, not a
// defensive one: *a dongle is a second radio, with its own sync daemon on it,
// and the Mac's programs use the Mac's radio.* The two are independent. The
// sync daemon running here does not know whether a dongle is plugged in, or
// whether one is reachable over the air; only octomancerd knows a dongle
// exists, and it reaches the sync daemon on it over the box protocol rather
// than reaching through it to a radio. doc/box-notes.md has the layering.
//
// It is worth stating that way round, because the first version of this file
// argued the same conclusion from the wrong premise -- that a serial port
// might be a printer, so `auto` cannot safely assume a dongle. True, and
// beside the point: even a port that announced itself as a dongle in
// unambiguous letters would still not be this process's radio to take. The
// weaker argument invites somebody to add a USB vendor-id check and
// reintroduce the whole problem believing they have fixed it.
//
// The rule got written because collapsing the two radios into one caused a
// bench outage on 2026-08-30. `--radio auto` meant "a dongle if one is plugged
// in", so a board left in a USB port from an unrelated experiment moved
// octomancerd off CoreBluetooth on its next restart, took the port
// exclusively, reported `"radio":"unknown","devices":0,"adverts":0`, and said
// nothing about having changed its mind. Five timecode boxes went from being
// heard 871,832 times to not existing.
//
// The rule that replaced it is one function with no I/O in it, which is the
// only reason it can be tested at all: the old one walked /dev, so the
// interesting case -- a host radio and a port, both present -- could not be
// arranged on a machine that did not happen to be in that state.
#include "harness.h"
#include "radio.h"

using octo::choose_dongle;
using octo::RadioKind;

namespace {

// Named for how it reads at the call site: choose_dongle(kind, named,
// host_radio, port_present) is four bare booleans otherwise, and three of
// them are true in the case that matters.
constexpr bool kNamed = true, kUnnamed = false;
constexpr bool kHostRadio = true, kNoHostRadio = false;
constexpr bool kPort = true, kNoPort = false;

// The shape of the system, stated as plainly as it can be. A Mac with its own
// radio uses it, and a dongle in a USB port is somebody else's radio.
void test_auto_keeps_the_host_radio_when_a_port_appears() {
  CHECK_EQ(choose_dongle(RadioKind::kAuto, kUnnamed, kHostRadio, kPort), false);
}

// And the same machine with nothing plugged in, which is the case that always
// worked and must keep working.
void test_auto_uses_the_host_radio_with_no_port() {
  CHECK_EQ(choose_dongle(RadioKind::kAuto, kUnnamed, kHostRadio, kNoPort),
           false);
}

// A Linux box, or any build without CoreBluetooth: the dongle is the only
// radio there is, so `auto` still finds it. This is the half of the old
// behaviour worth keeping, and the reason the fix is not simply "never
// auto-select a dongle".
void test_auto_uses_the_dongle_when_there_is_no_host_radio() {
  CHECK_EQ(choose_dongle(RadioKind::kAuto, kUnnamed, kNoHostRadio, kPort),
           true);
}

// No radio of any kind. The answer is still false -- there is no dongle to
// select -- and the caller reports "this host has no radio" rather than
// failing to open a port that is not there.
void test_auto_with_nothing_at_all_selects_nothing() {
  CHECK_EQ(choose_dongle(RadioKind::kAuto, kUnnamed, kNoHostRadio, kNoPort),
           false);
}

// Asking for the dongle gets the dongle, including on a Mac, and including
// when no port is currently visible: the failure a person wants in that case
// is "the dongle you asked for is not there", not a silent fallback onto a
// different radio.
//
// This mode is how the dongle's camera path gets exercised before there is
// firmware to run a sync daemon on the dongle itself, so it is transitional
// rather than wrong -- but it is a Mac process reaching through a dongle to a
// radio, which is not the shape the system is heading for. It matters that it
// is unconditional: a transitional mode that sometimes silently did something
// else would be worse than not having it.
void test_asking_for_the_dongle_always_gets_it() {
  CHECK_EQ(choose_dongle(RadioKind::kDongle, kUnnamed, kHostRadio, kPort),
           true);
  CHECK_EQ(choose_dongle(RadioKind::kDongle, kUnnamed, kHostRadio, kNoPort),
           true);
  CHECK_EQ(choose_dongle(RadioKind::kDongle, kUnnamed, kNoHostRadio, kNoPort),
           true);
}

// Naming a port is asking for it. There is no reason to hand a program a path
// and mean something else by it, so --dongle/OCTOMANCER_DONGLE outranks a
// `kind` that was merely left at its default.
void test_naming_a_port_is_asking_for_it() {
  CHECK_EQ(choose_dongle(RadioKind::kAuto, kNamed, kHostRadio, kNoPort), true);
  CHECK_EQ(choose_dongle(RadioKind::kAuto, kNamed, kNoHostRadio, kNoPort),
           true);
}

// The other direction, which is the one somebody reaches for after being
// bitten by this: --radio corebluetooth refuses the dongle however loudly it
// is present, and even if a port was named.
void test_corebluetooth_refuses_the_dongle() {
  CHECK_EQ(choose_dongle(RadioKind::kCoreBluetooth, kUnnamed, kHostRadio,
                         kPort),
           false);
  CHECK_EQ(choose_dongle(RadioKind::kCoreBluetooth, kNamed, kHostRadio, kPort),
           false);
}

// A property rather than a case: under `auto`, with a host radio present, the
// answer does not depend on what is in /dev at all.
//
// This is the independence the layering rests on, expressed as something that
// cannot be eroded by degrees. A later change that makes the answer depend on
// the port again -- however good the reason, and "we can now tell a real
// dongle from a breadboard" is the good reason to watch for -- has to fail
// here.
void test_a_port_cannot_influence_auto_on_a_host_with_a_radio() {
  for (int named = 0; named <= 1; ++named) {
    const bool with_port =
        choose_dongle(RadioKind::kAuto, named != 0, kHostRadio, kPort);
    const bool without_port =
        choose_dongle(RadioKind::kAuto, named != 0, kHostRadio, kNoPort);
    CHECK_EQ(with_port, without_port);
  }
}

// The fake radio is never a dongle, whatever else is set. It matters that this
// is checked rather than assumed: OCTOMANCER_DONGLE may well still be set in a
// shell that is now running a fake bench, and a fake run that quietly opened a
// real serial port -- taking it exclusively, from whatever else wanted it --
// would be the worst of both.
void test_a_fake_bench_never_reaches_for_a_port() {
  for (int named = 0; named <= 1; ++named) {
    for (int host = 0; host <= 1; ++host) {
      for (int port = 0; port <= 1; ++port) {
        CHECK_EQ(choose_dongle(RadioKind::kFake, named != 0, host != 0,
                               port != 0),
                 false);
      }
    }
  }
}

// have_host_radio() is a fact about the build, not the machine, so the only
// thing to assert is that it agrees with the macro the rest of the tree
// compiles against. Without this the suite would pass on a Mac built without
// CoreBluetooth while every program on it chose the wrong radio.
void test_have_host_radio_matches_the_build() {
#ifdef OCTO_HAVE_COREBLUETOOTH
  CHECK_EQ(octo::have_host_radio(), true);
#else
  CHECK_EQ(octo::have_host_radio(), false);
#endif
}

}  // namespace

int main() {
  test_auto_keeps_the_host_radio_when_a_port_appears();
  test_auto_uses_the_host_radio_with_no_port();
  test_auto_uses_the_dongle_when_there_is_no_host_radio();
  test_auto_with_nothing_at_all_selects_nothing();
  test_asking_for_the_dongle_always_gets_it();
  test_naming_a_port_is_asking_for_it();
  test_corebluetooth_refuses_the_dongle();
  test_a_port_cannot_influence_auto_on_a_host_with_a_radio();
  test_a_fake_bench_never_reaches_for_a_port();
  test_have_host_radio_matches_the_build();
  return octotest::report("test_radio");
}
