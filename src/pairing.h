// Deciding what a pairing attempt actually meant.
//
// None of this talks to a radio, for the same reason src/camsync.h does not:
// the interesting part of pairing is not the connecting, it is working out
// which of several indistinguishable-looking failures happened, and that is
// exactly the part no one can exercise without a camera in the room unless it
// is kept on this side of the seam. src/camera.h is where the radio starts.
//
// The failure this exists for is a quiet one. A Blackmagic camera whose bond
// has gone away still advertises, still accepts a connection, and still lets
// a subscription to its control characteristics succeed. What it does not do
// is send anything over them. The link looks healthy at every layer a program
// normally checks, and the only visible symptom is that no timecode ever
// arrives -- which is also what an idle camera in Clip mode looks like, and
// what a camera with its timecode stopped looks like. Telling those apart is
// the whole job here.
#ifndef OCTO_PAIRING_H
#define OCTO_PAIRING_H

#include <string>

namespace octo {
namespace pairing {

// What the radio saw while trying to bring up an encrypted link. Filled in by
// the caller in src/octomancer-sync.cc, which is the only part that touches
// CoreBluetooth; everything below reasons about it and nothing else.
struct Attempt {
  // A connection was established at all.
  bool connected = false;
  // Subscribing to the Timecode and Incoming Control characteristics was
  // accepted. On macOS this succeeding proves less than it appears to: see
  // the header comment.
  bool subscribed = false;
  // Something -- anything -- arrived over those characteristics afterwards.
  // This is the observation that separates a bonded camera from an unbonded
  // one, because an unbonded camera stays silent rather than erroring.
  bool saw_state = false;
  // Whatever the radio said when it failed, if it said anything. macOS is
  // frequently silent where an error would be more useful.
  std::string radio_error;
};

enum class Verdict {
  // Encryption is up and the camera is talking. Nothing more to do.
  kBonded,
  // Never got a link. The camera is not on the air, is out of range, or is
  // already connected to something else.
  kNotConnected,
  // The radio refused in a way that names authentication. Rare on macOS, but
  // unambiguous when it happens.
  kRefused,
  // Connected, subscribed, and then nothing. The characteristic-level silence
  // that means there is no bond.
  kSilent,
};

// True when a radio error names an authentication or encryption problem.
// Split out because the strings come from CoreBluetooth and from the ATT
// layer in src/att.h, and neither is a stable enough interface to match
// inline at the one call site that cares.
bool names_authentication_failure(const std::string& radio_error);

Verdict judge(const Attempt& attempt);

// One line naming what happened, and then what to do about it. `label` is
// whatever the camera is called to a person -- a name if it has one, an
// identifier otherwise -- and may be empty.
std::string explain(Verdict verdict, const std::string& label);

// A short machine-ish word for the verdict, for logs and for the exit status
// line. Stable: it is the thing a script would match on.
const char* word(Verdict verdict);

}  // namespace pairing
}  // namespace octo

#endif  // OCTO_PAIRING_H
