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

enum class RadioKind {
  // Use the dongle if one is plugged in, otherwise CoreBluetooth. A dongle
  // that is present but broken is an error rather than a silent fallback:
  // somebody who plugged it in meant to use it.
  kAuto,
  kCoreBluetooth,
  kDongle,
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

// True when the factories will use the dongle: either it was asked for, or it
// was left to chance and one is plugged in. Exposed because the two factories
// live in separate translation units -- see the note in radio.cc -- and both
// have to make the same decision.
bool dongle_selected();

// What the factories would use right now, for the logs and for `--version`:
// "nRF52840 at /dev/cu.usbmodem1101" or "CoreBluetooth". Does not open
// anything.
std::string describe_radio();

// The dongle backends, called by the factories in radio.cc. Exposed because
// the Zoom experiment tool wants a link of its own, on its own terms.
std::unique_ptr<Scanner> make_hci_scanner(Scanner::AdvertHandler on_advert,
                                          Scanner::SightingHandler on_camera,
                                          Scanner::StateHandler on_state);
std::unique_ptr<CameraLink> make_hci_camera_link();

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
