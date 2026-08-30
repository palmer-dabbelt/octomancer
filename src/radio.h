// Which radio, and how to ask for it.
//
// There are two now: CoreBluetooth, which is what every Mac already has, and
// an nRF52840 dongle driven over HCI. Neither replaces the other. The dongle
// can do things CoreBluetooth will not -- put exact bytes in an advertisement,
// say what it actually transmitted, act as a peripheral on someone else's
// terms -- and CoreBluetooth needs no hardware anyone has to remember to plug
// in. So the choice is made at run time, per process, and defaults to
// whichever is actually there.
//
// This deliberately does not change the shape of scanner.h or camera.h. The
// factories keep their names and their signatures; only the implementation
// behind them is chosen here. A program that never thinks about radios carries
// on not thinking about them.
#ifndef OCTO_RADIO_H
#define OCTO_RADIO_H

#include <memory>
#include <string>

#include "camera.h"
#include "scanner.h"

namespace octo {

namespace hci {
class SharedLink;
}  // namespace hci

enum class RadioKind {
  // Whatever this host has, preferring the one that is definitely a radio.
  // On a Mac that is CoreBluetooth; a dongle under `auto` is only reached
  // where there is no host radio at all. See choose_dongle() for why.
  kAuto,
  kCoreBluetooth,
  // Drive a dongle's radio directly over HCI from this process. Transitional:
  // in the finished system a dongle runs its own sync daemon and octomancerd
  // speaks the box protocol to it, rather than any Mac process reaching
  // through it. Still the only way to exercise the dongle's camera path until
  // there is firmware.
  kDongle,
  // No radio at all: a bench of boxes and a camera that are not there, built
  // from OCTOMANCER_FAKE. Never selected by `auto` -- it has to be asked for,
  // because a program that silently invented its own devices would be worse
  // than one that found none. See src/fakebench.h.
  kFake,
};

struct RadioOptions {
  RadioKind kind = RadioKind::kAuto;
  // The dongle's serial port. Empty means "find one", which is right on a
  // machine with a single dongle and wrong on a machine with two.
  std::string device;
  // Log every HCI packet in both directions. This is the facility whose
  // absence made the Zoom investigation so slow: with CoreBluetooth there was
  // no way to see what had been transmitted.
  bool trace = false;

  // The six-digit passkey a camera displays while pairing. Negative means
  // "not supplied", which is different from zero: 000000 is a passkey a peer
  // might genuinely be showing, and guessing it would pair with whatever
  // accepts it.
  //
  // Only the dongle needs this. CoreBluetooth puts the prompt on screen and
  // remembers the bond afterwards; over HCI the bond is ours to establish, and
  // this program does not yet keep one between runs -- so a camera pairs
  // afresh each time it is connected to.
  int passkey = -1;

  // Whether to ask on the terminal when a passkey is wanted and none was
  // given. False under launchd, where there is nobody to ask.
  bool prompt_for_passkey = true;

  // The synthetic bench, when `kind` is kFake. Empty means the standard one.
  // See FakeBench::parse for the grammar.
  std::string fake;
};

// Process-wide, set once during argument parsing and read by the factories.
// A global because the factories are called from three places that have no
// business knowing about each other, and threading an options struct through
// them would be a change to every signature for the benefit of one setting.
RadioOptions& radio_options();

bool parse_radio_kind(const std::string& text, RadioKind* out);
const char* radio_kind_name(RadioKind kind);

// Reads OCTOMANCER_RADIO, OCTOMANCER_DONGLE and OCTOMANCER_HCI_TRACE into the
// options above. Environment rather than only flags because the agents are
// started by launchd, where there is no command line to edit.
//
// Returns false with a reason for a value it cannot parse, rather than
// ignoring it: a typo in OCTOMANCER_RADIO should not silently select the
// wrong radio.
bool radio_options_from_env(std::string* err);

// True when the factories will use the dongle. Exposed because the two
// factories live in separate translation units -- see the note in radio.cc --
// and both have to make the same decision.
bool dongle_selected();

// The rule dongle_selected() applies, with the two things it has to go and
// look up passed in instead. Pure, so it can be tested; see tests/test_radio.
//
//   kind          what --radio / OCTOMANCER_RADIO said (kFake is never a
//                 dongle, and is refused here so a fake run cannot end up
//                 opening a serial port)
//   named         a specific port was given (--dongle, OCTOMANCER_DONGLE)
//   host_radio    this build has a radio of its own -- CoreBluetooth
//   port_present  some cu.usbmodem*/ttyACM* exists
//
// The interesting case is kAuto with both a host radio and a port, and the
// answer is *no* -- because a dongle is not a radio this process should be
// choosing between. It is a second radio in the *system*, running a sync
// daemon of its own, and the only program with any business knowing it exists
// is octomancerd. See "Two radios, and which program knows" in
// doc/box-notes.md.
//
// That distinction is worth being exact about, because the first version of
// this rule reached the same answer by the wrong route. It argued that a
// serial port might be a printer -- true, and beside the point. If `auto` had
// been reliably able to tell a dongle from a debug probe it would still be
// wrong to take it: the Mac's sync daemon uses the Mac's radio, and the
// dongle's uses the dongle's, and neither knows about the other. Reasoning
// from "we cannot be sure it is a dongle" invites somebody to add a USB
// vendor-id check later and reintroduce the whole problem while believing
// they have fixed it.
//
// `auto` used to mean "a dongle if one is plugged in". That collapsed the two
// radios into one and handed it to whichever program asked first, so plugging
// a dongle into a USB port moved octomancerd off CoreBluetooth on its next
// restart and took the port exclusively -- which is what happened on
// 2026-08-30, and cost a morning's bench.
//
// A dongle can still be driven directly with --radio dongle, and that mode is
// not going away: it is how the dongle's camera path is exercised before there
// is firmware to run a sync daemon on it. It is a transitional arrangement
// rather than the shape of the system.
bool choose_dongle(RadioKind kind, bool named, bool host_radio,
                   bool port_present);

// Whether this build has a radio that needs nothing plugged in. Compiled in
// rather than asked at run time: it is a question about the binary.
bool have_host_radio();

// Whether the dongle was *asked for*, rather than merely found.
//
// Since the rule above, these two agree on any host that has a radio of its
// own, and differ only where there is none -- a Linux box with a dongle and
// nothing else selects it without being asked. Both are kept because they are
// different questions and the answer stops coinciding the moment a second
// host radio backend exists.
bool dongle_requested();

// What the factories would use right now, for the logs and for `--version`:
// "nRF52840 at /dev/cu.usbmodem1101" or "CoreBluetooth". Does not open
// anything.
std::string describe_radio();

// The dongle backends, called by the factories in radio.cc. Exposed because
// the Zoom experiment tool wants a link of its own, on its own terms.
std::unique_ptr<Scanner> make_hci_scanner(Scanner::AdvertHandler on_advert,
                                          Scanner::SightingHandler on_camera,
                                          Scanner::StateHandler on_state);

// The camera that is not there. Never reached unless it was asked for.
std::unique_ptr<CameraLink> make_fake_camera_link();

// The bench that is not there. Never reached unless it was asked for.
std::unique_ptr<Scanner> make_fake_scanner(Scanner::AdvertHandler on_advert,
                                           Scanner::SightingHandler on_camera,
                                           Scanner::StateHandler on_state);

// The same scanner, over a radio somebody else owns.
//
// This is the one a program with more than one job for the dongle wants. The
// factory above opens the port itself, which is correct for a program that
// only listens and wrong for one that also drives a camera: two Links on one
// serial port is not refused and presents as a radio that powered off. See
// src/hcishare.h, which exists because of exactly that.
//
// The SharedLink must outlive the Scanner.
std::unique_ptr<Scanner> make_hci_scanner_on(hci::SharedLink* radio,
                                             Scanner::AdvertHandler on_advert,
                                             Scanner::SightingHandler on_camera,
                                             Scanner::StateHandler on_state);

// There is deliberately no make_hci_camera_link().
//
// CameraLink blocks by contract, and the dongle's camera half no longer can:
// it waits on a reader thread that no longer exists, and a thread is the one
// thing the box cannot have. Its replacement is octo::HciCamera in
// src/camhci.h, which is the same logic on the loop with completions instead
// of return values, and which the sync daemon will use. Until that daemon
// exists, asking for a camera over the dongle says so rather than returning a
// link that would hang -- see make_camera_link() in radio_camera.cc.
//
// Nothing is lost by this that ever worked: doc/dongle-notes.md records that
// connecting, pairing and writing a clock over a dongle have never been run
// against hardware. Scanning has, and scanning still goes through
// make_hci_scanner above.

#ifdef OCTO_HAVE_COREBLUETOOTH
// The CoreBluetooth backends. These are what make_ble_scanner and
// make_camera_link used to be, renamed so the old names can do the choosing.
std::unique_ptr<Scanner> make_corebluetooth_scanner(
    Scanner::AdvertHandler on_advert, Scanner::SightingHandler on_camera,
    Scanner::StateHandler on_state);
std::unique_ptr<CameraLink> make_corebluetooth_camera_link();
#endif

}  // namespace octo

#endif  // OCTO_RADIO_H
