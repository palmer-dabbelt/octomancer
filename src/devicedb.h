// The devices octomancerd has ever seen, kept across restarts.
//
// The registry is memory, and memory is the right shape for it: it holds a
// window of samples, a drift fit, an alert state machine. None of that is
// worth keeping across a restart, because all of it is re-measured within a
// minute of the radio coming back.
//
// The *list* is different. A timecode box that was on the bench yesterday and
// is switched off today is a fact worth showing, and losing it on restart made
// `octomancer status` quietly disagree with the room: five boxes before a
// restart, four after, with nothing saying that the fifth had ever existed.
// Somebody looking for a box that is missing needs to see a row saying it has
// not been heard from since Tuesday, not an absence they have to already know
// about to notice.
//
// This is a layer-2 file in the sense doc/box-notes.md means. The sync daemon
// keeps almost nothing, because it may be a box with nothing but NVS; the
// control daemon runs on a Mac with a filesystem and remembers everything. So
// there is no equivalent of this on the box and there should not be one.
//
// # Format
//
// JSON Lines, one flat object per device, rewritten whole on every save. That
// is the opposite of src/camdb.h's append-and-compact, and deliberately: this
// file holds one short record per device rather than a growing history of
// observations, so a rewrite is a few hundred bytes and there is no path here
// that runs while a camera is connected. A log would be more machinery for a
// document that fits in a packet.
//
// The records are flat objects so src/logscan.h reads them, which is the same
// constraint camdb.h works under and for the same reason.
#ifndef OCTO_DEVICEDB_H
#define OCTO_DEVICEDB_H

#include <string>
#include <vector>

namespace octo {

// One device, as much as is worth keeping about it between runs.
//
// Everything here is either identity or a last-known reading. Nothing is a
// measurement of the present, and the distinction is the whole design: the
// numbers are shown beside an age, so a reader can tell "this box is 3.6 s out"
// from "this box was 3.6 s out when it was last heard, three days ago".
struct RememberedDevice {
  std::string id;
  std::string name;

  double first_seen_wall = 0.0;
  double last_seen_wall = 0.0;

  // The last reading, and whether there was one. A device seen only as an
  // undecodable advertisement has an id and a name and no time at all.
  //
  // Kept for the record rather than for the screen: `octomancer status` shows
  // "--" in the OFFSET column for a device it is not hearing, because a
  // three-day-old number printed in the same style as a fresh one is a number
  // somebody will act on. It is here so that whoever opens this file can see
  // where a box was when it was last heard, and so that a renderer which wants
  // to show it dimmed has it available. DeviceSnapshot::heard_this_run is the
  // flag such a renderer would key on.
  bool has_time = false;
  double offset = 0.0;
  double median_offset = 0.0;
  std::string resolution;
  int fps = 0;
  int rssi = 0;
};

class DeviceDb {
 public:
  // Missing is not an error: the first run has no file, and neither does a
  // fresh machine. A file that exists and cannot be parsed *is* an error,
  // because carrying on would silently drop a roster somebody is relying on
  // and then overwrite it on the next save.
  bool load(const std::string& path, std::string* err);

  // Written to a temporary file and renamed, so a crash halfway through leaves
  // the previous roster rather than half of the new one. A device list is not
  // precious, but a truncated one would come back as a *shorter* roster, which
  // is exactly the failure this file exists to prevent.
  bool save(const std::string& path, const std::vector<RememberedDevice>& devices,
            std::string* err) const;

  const std::vector<RememberedDevice>& devices() const { return devices_; }

  static std::string default_path();

 private:
  std::vector<RememberedDevice> devices_;
};

}  // namespace octo

#endif  // OCTO_DEVICEDB_H
