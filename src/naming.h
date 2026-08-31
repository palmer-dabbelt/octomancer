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

// The stand-ins a snapshot uses when a device has not said what it is called.
// Not names, and storing one as though it were would put "(unnamed)" on a row
// that a probe could have named properly.
bool is_placeholder_name(const std::string& name);

}  // namespace octo

#endif  // OCTO_NAMING_H
