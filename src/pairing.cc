#include "pairing.h"

#include <algorithm>
#include <cctype>

namespace octo {
namespace pairing {

namespace {

std::string lowered(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

bool contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

// What to call the camera in a sentence. An empty label is not worth a
// placeholder that reads worse than the sentence without it.
std::string named(const std::string& label) {
  return label.empty() ? std::string("the camera") : label;
}

}  // namespace

bool names_authentication_failure(const std::string& radio_error) {
  const std::string e = lowered(radio_error);
  // CoreBluetooth phrases it several ways depending on which layer refused,
  // and the ATT codes in src/att.h arrive as text by the time they reach
  // here. Matching on any of the words that only appear in this family is
  // more durable than matching whole strings from either source.
  return contains(e, "authentic") || contains(e, "encrypt") ||
         contains(e, "insufficient") || contains(e, "pair") ||
         contains(e, "bond");
}

Verdict judge(const Attempt& attempt) {
  // Order matters: an authentication error is worth reporting as such even
  // when it arrived after a connection, because it names the cause outright
  // and nothing below can do better than infer one.
  if (names_authentication_failure(attempt.radio_error)) return Verdict::kRefused;
  if (!attempt.connected) return Verdict::kNotConnected;
  if (attempt.saw_state) return Verdict::kBonded;
  // Subscribed or not, the camera said nothing. On this hardware those are
  // the same finding: the characteristics are encrypted, and an unbonded peer
  // gets silence rather than a refusal.
  return Verdict::kSilent;
}

const char* word(Verdict verdict) {
  switch (verdict) {
    case Verdict::kBonded: return "bonded";
    case Verdict::kNotConnected: return "not-connected";
    case Verdict::kRefused: return "refused";
    case Verdict::kSilent: return "silent";
  }
  return "unknown";
}

std::string explain(Verdict verdict, const std::string& label) {
  const std::string who = named(label);
  switch (verdict) {
    case Verdict::kBonded:
      return "paired with " + who +
             ": the control characteristics are answering, so the clock can be"
             " read and set from now on.";

    case Verdict::kNotConnected:
      return "could not connect to " + who +
             ".\n"
             "  * a camera already connected to something else -- the"
             " Blackmagic Camera app on a\n"
             "    phone, or the firmware updater -- will not accept a second"
             " connection\n"
             "  * check Bluetooth is still enabled in the camera's setup menu\n"
             "  * move the camera nearer: this radio is weak enough that a"
             " room away is often\n"
             "    the difference";

    case Verdict::kRefused:
      return "pairing with " + who +
             " was refused.\n"
             "  * if macOS asked for a passkey and it was mistyped, forget the"
             " device in\n"
             "    System Settings > Bluetooth and run this again\n"
             "  * a bond that exists on only one of the two sides fails exactly"
             " this way: clear\n"
             "    it on the camera as well as on the Mac, then pair afresh";

    case Verdict::kSilent:
      return "connected to " + who +
             ", but it sent nothing back.\n"
             "  This is what an unpaired camera looks like: the control"
             " characteristics are\n"
             "  encrypted, so without a bond they stay silent rather than"
             " returning an error.\n"
             "  * macOS should have shown a pairing request with a six-digit"
             " code. The code is\n"
             "    displayed on the camera itself -- look at its screen while"
             " this runs\n"
             "  * if no request appeared, the Mac probably still holds a stale"
             " bond. Forget the\n"
             "    device in System Settings > Bluetooth, clear the pairing on"
             " the camera too,\n"
             "    and run this again so both sides start from nothing";
  }
  return "pairing with " + who + " ended in a state this program does not know.";
}

}  // namespace pairing
}  // namespace octo
