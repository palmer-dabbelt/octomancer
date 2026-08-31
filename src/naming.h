// What to call a device, and who gets to decide.
//
// Three things can claim to know a device's name, and they disagree often
// enough that the precedence has to be written down rather than left to
// whichever code path ran last.
//
//   * **A person.** Somebody typed `octomancer name <id> "B camera"`. This
//     always wins. It is the only one of the three that is a decision rather
//     than an observation, and a program that overruled it would be arguing
//     with the person holding it.
//   * **A probe.** Something connected to the device and read its GAP name.
//     Authoritative as far as the device is concerned, and expensive: it costs
//     a connection, so it is done once and remembered.
//   * **An advertisement.** Free, and not always there. CoreBluetooth fills
//     names in from scan responses it has cached, which is why devices heard
//     by this Mac usually have one; a passive listener that never sends a scan
//     request -- which is what a dongle is -- gets nothing.
//
// **Why this is one book rather than a field on each device.** The names have
// to outlive the roster: a box switched off for a week should come back with
// the name somebody gave it, and the alternative is asking the person to name
// it again every time the daemon restarts. They also have to be reachable
// without a device present at all, because renaming something you cannot
// currently hear is a reasonable thing to want to do.
//
// **The identifier is whatever the radio that heard it calls it.** A
// CoreBluetooth UUID for this Mac's devices, a hardware address for a
// dongle's. Those are different namespaces and this book holds both, which is
// safe precisely because they cannot collide -- one is 36 characters of hex
// and dashes, the other is 17. Nothing here tries to match them up; see
// doc/box-notes.md for why nothing can.
#ifndef OCTO_NAMING_H
#define OCTO_NAMING_H

#include <map>
#include <string>
#include <vector>

namespace octo {

// Where a displayed name came from. Worth carrying because the answers a
// person wants differ: a name they typed needs no explanation, a probed one
// might be stale, and an identifier standing in for a name is an invitation to
// go and probe it.
enum class NameSource { kNone, kHeard, kProbed, kUser };

const char* name_source_name(NameSource source);

struct DeviceName {
  std::string user;    // somebody typed it
  std::string probed;  // the device said so when asked
  std::string heard;   // it arrived in an advertisement

  // Whether a probe has been attempted and come back empty, as opposed to
  // never having been attempted. Without this a device that genuinely has no
  // name is asked again on every pass forever.
  bool probed_done = false;

  bool empty() const {
    return user.empty() && probed.empty() && heard.empty() && !probed_done;
  }
};

class NameBook {
 public:
  // The name to show, falling back to the identifier. Never returns empty:
  // something has to go in the column, and an identifier is a worse name but a
  // better answer than a blank.
  std::string display(const std::string& id, NameSource* from = nullptr) const;

  // What a person called it. Empty clears it, which is how somebody undoes a
  // rename without a second verb for it.
  void rename(const std::string& id, const std::string& name);

  // What a probe found. Recorded even when empty, so that a device with no
  // name of its own is asked once rather than forever.
  void probed(const std::string& id, const std::string& name);

  // What an advertisement carried. Ignored when it is empty or is one of the
  // stand-ins a snapshot uses for "no name", because storing those would make
  // the book claim to know something it does not.
  void heard(const std::string& id, const std::string& name);

  // Forget everything learned *about* this device, so it is probed again.
  //
  // A name somebody typed is deliberately kept. It is not information about
  // the device, it is a decision about it, and a person who renames a camera
  // and then asks for a refresh is asking to re-read the device -- not to lose
  // their own label. `rename(id, "")` is how the label goes.
  //
  // Returns false when there was nothing to forget, so a caller can tell
  // "done" from "there was no such device", without making that difference
  // into a failure.
  bool refresh(const std::string& id);

  // Devices worth asking about: seen, not named by a person, and not probed
  // yet. This is the work list a control daemon walks.
  std::vector<std::string> unprobed() const;

  bool needs_probe(const std::string& id) const;

  // Everything known, for saving. Devices with nothing worth keeping are left
  // out rather than written as empty records.
  const std::map<std::string, DeviceName>& all() const { return names_; }
  void put(const std::string& id, const DeviceName& name);

  size_t size() const { return names_.size(); }

 private:
  std::map<std::string, DeviceName> names_;
};

// Whether a radio should be scanning actively rather than passively.
//
// A passive listener hears advertisements and nothing else. A Tentacle puts
// its clock in the advertisement -- which is the whole reason this project can
// listen to a whole bench at once without connecting to anything -- and its
// *name* in the scan response, which only an active scan asks for. So a
// passive radio knows exactly what time every box thinks it is and has no idea
// what any of them are called.
//
// Active scanning is not free: it transmits a scan request for every
// advertisement it hears, which costs power and puts this device on the air in
// a room that has not asked to hear from it. So it is switched on when there
// is something to learn and off again once there is not, rather than left on.
//
// `unnamed_recent` is how many devices we have heard lately and cannot name --
// **lately**, not *this instant*, and the difference is the whole of a bug that
// made some boxes impossible to name rather than slow to name.
//
// The rule used to count only devices that were live. A box at the edge of
// range is heard every minute or two and is stale in between, so it was
// almost never live at the moment the decision was made; the count came out
// zero, the radio stayed passive, and the box kept its hardware address
// forever. The boxes near enough to be continuously live were exactly the
// boxes that already had names. See kNameWithin.
//
// `since_change` is how long the current setting has been in force; a radio
// that flipped on every advertisement would spend its time restarting scans
// rather than doing either.
bool want_active_scan(bool active_now, int unnamed_recent, double since_change);

// How long a scan setting is left alone before it may change again. Long
// enough that a box appearing and disappearing at the edge of range does not
// restart the radio every second; short enough that a new box is named while
// somebody is still looking at the screen.
inline constexpr double kScanSettleSeconds = 20.0;

// How recently a device must have been heard for its missing name to be worth
// putting the radio on the air for.
//
// Long enough to cover a distant box's advertising gap several times over --
// a Tentacle at -84 dBm can genuinely go minutes between packets, and the
// point of this rule is to still be scanning actively when its next one
// arrives. Short enough that a box carried out of the building stops costing
// airtime within a setup break rather than for the rest of the day.
//
// It terminates because everything in a sync daemon's roster is a Tentacle --
// nothing else gets that far -- and a Tentacle always answers a scan request
// with its name. So the radio goes quiet once it has asked everything present,
// rather than staying active forever on a device that will never answer.
inline constexpr double kNameWithin = 300.0;

// A radio's entry in the same book, under a key that cannot collide with a
// device's.
//
// Radios have names worth giving too: somebody with two dongles on a cart
// wants to know which one is "cart left", and "dongle" is what the firmware
// calls itself rather than a choice anybody made. The book is already the
// thing that remembers a name across a restart and writes it through to disk
// the moment it changes, so radios go in it -- one mechanism rather than a
// second one that would have to be taught the same lessons.
//
// The prefix is what keeps the two apart. Every device identifier this program
// deals in is hex: a hardware address is hex and colons, a CoreBluetooth UUID
// is hex and dashes, and neither can begin with "radio:". A radio's own name
// is whatever its firmware says, so it is the part that varies and it goes
// after the prefix rather than in front of it.
//
// Only a person can name a radio. Nothing advertises a dongle and there is
// nothing to connect to and ask, so of the three claims in this file's opening
// note only the first one ever applies -- which is why radio_display() takes
// the user's name or the radio's own and never looks at the other two.
// The empty radio is this machine's own -- the one whose rows carry no tag at
// all -- and it keys as "radio:" with nothing after it. Deliberately not the
// hostname: a machine that gets renamed would otherwise lose the name somebody
// gave its radio, and the caller asking to rename it has only the label in
// front of it to go on, not the hostname underneath.
std::string radio_name_key(const std::string& radio);
bool is_radio_key(const std::string& id);

// What a person has called this radio, or empty when nobody has. The fallback
// is left to the caller because it differs: a dongle falls back to the name its
// firmware gave, and this machine's radio falls back to the hostname, and
// neither is knowable from here.
std::string radio_user_name(const NameBook& names, const std::string& radio);

// The stand-ins a snapshot uses when a device has not said what it is called.
// Not names, and storing one as though it were would put "(unnamed)" on a row
// that a probe could have named properly.
bool is_placeholder_name(const std::string& name);

}  // namespace octo

#endif  // OCTO_NAMING_H
