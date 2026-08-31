// What the daemon knows about the boxes in the room.
//
// The registry is the only mutable state in the service, and it belongs to one
// thread: the loop's. It used to take a lock on every entry point, because
// CoreBluetooth delivers advertisements on a private dispatch queue while the
// socket was served from somewhere else. That lock is gone, and its absence is
// a requirement rather than an economy -- the Zephyr SDK's libstdc++ has no
// std::mutex in any multilib, so a registry that holds one cannot be compiled
// for the box at all. See src/loop.h.
//
// What replaces it is src/scanbridge.h, which takes whatever the radio hands
// it on whatever thread, and replays it on the loop's thread. So the rule for
// callers is: touch this from the loop, or from a program that has only one
// thread to touch it from. Nothing here will notice if you break that rule,
// which is why it is written down here rather than left to be inferred from
// the absence of a lock.
//
// Reads still hand back a self-contained snapshot rather than a pointer into
// live state. That was never about threads: a snapshot is what a client is
// entitled to see and a live pointer is what it would go stale holding.
#ifndef OCTO_REGISTRY_H
#define OCTO_REGISTRY_H

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "devicedb.h"
#include "tentacle.h"

namespace octo {

struct Policy {
  // A box more than this far from the host clock needs re-jamming. The gap
  // between enter and exit is hysteresis: a box sitting exactly on the
  // threshold must not alternate between states every few seconds.
  double alert_enter = 60.0;
  double alert_exit = 45.0;
  // Consecutive observations required before either transition is believed.
  // One bad advert is not a story about the box.
  int alert_confirm = 3;
  // How long to keep re-notifying while a box stays out of sync.
  double renotify_after = 1800.0;

  // A box not heard from in this long is stale: still listed, but no longer
  // counted in the bench figures, because a remembered offset is not evidence
  // about the present.
  double stale_after = 30.0;
  // ...and after this long it is gone; the box left the building.
  //
  // Zero or negative means never, which is what octomancerd sets. The two
  // halves of the program want opposite things here and the reason is the
  // layering: a sync daemon may be a box with nothing but NVS, so it holds a
  // working set; the control daemon runs on a Mac with a filesystem and is
  // supposed to know every device that has ever been seen, showing the ones it
  // has not heard from lately as offline. See src/devicedb.h.
  double forget_after = 86400.0;

  // Sample retention, which sets how long a lever arm drift is measured over.
  double window = 3600.0;
  size_t max_samples = 8192;

  // Drift is parts per million. Refuse to report it from a short arm: over a
  // minute, one frame of quantisation invents hundreds of ppm that read as a
  // measurement. The Python daemon learned this the hard way.
  double min_drift_span = 900.0;
  int min_drift_samples = 30;

  // A camera advertises every second or so while it is idle, and not at all
  // while something holds a connection to it. This has to be longer than a
  // sync cycle's connection (connect, read, write, verify: about twenty
  // seconds) or every correction would look like the camera being switched
  // off and back on again.
  double camera_gone_after = 90.0;
};

struct Sample {
  double mono;
  double offset;
};

// A box that crossed an alert threshold. The daemon acts on these; the UI
// diffs snapshots instead, so both can notify without coordinating.
struct AlertEvent {
  std::string id;
  std::string name;
  double offset;
  double wall;
  bool entering;  // false when the box came back into agreement
  bool repeat;    // a reminder about an alert already reported
};

struct DeviceSnapshot {
  std::string id;
  std::string name;

  // Which radio heard this. Empty means the one in this machine, which is
  // both the common case and the only one the registry itself produces;
  // anything else was heard by a dongle and folded in by src/dongle.h.
  //
  // It is a field rather than a merge, and that is forced rather than chosen.
  // The two radios cannot agree on what to call a box: CoreBluetooth hands
  // out a per-host UUID and refuses to show the hardware address, while the
  // dongle sees the address and nothing else. Nor is there anything in the
  // advertisement to match on -- a Tentacle broadcasts a clock and no serial
  // number at all (src/tentacle.h). So a box in earshot of both appears
  // twice, once per radio, and the rows are related by the reader rather than
  // by a guess made here. Two readings of the same box from two radios is
  // also the more useful thing to look at: they should agree, and noticing
  // that they do not is the entire reason for having the second radio.
  std::string radio;

  int rssi = 0;
  uint64_t adverts = 0;
  uint64_t decoded = 0;
  double age = 0.0;          // seconds since the last advertisement
  double first_seen_wall = 0.0;
  bool live = false;         // heard from recently enough to count

  // Whether anything in this row was measured during this run of the daemon.
  //
  // False for a device restored from disk and not heard since. Everything in
  // such a row is last-known rather than current, and `age` says how long ago
  // -- but a renderer that showed a three-day-old offset in the same style as
  // a fresh one would be inviting somebody to act on it.
  bool heard_this_run = true;

  bool has_time = false;
  double sod = 0.0;
  double offset = 0.0;        // most recent, signed, wrapped
  double median_offset = 0.0; // robust over the window
  int samples = 0;
  std::string display;
  std::string resolution;
  std::string note;
  int fps = 0;

  bool has_drift = false;
  double drift_ppm = 0.0;
  double drift_span = 0.0;

  bool alerting = false;
  double alert_since_wall = 0.0;
};

// Whether the camera is on the air, which is all an advertisement can say
// about it -- see Sighting in scanner.h.
//
// `sessions` is the interesting field. It counts absent -> present
// transitions, so a consumer that remembers the number it saw last time can
// tell "the camera has been on all along" from "the camera was power-cycled
// while you were not looking", which matters because a power cycle resets the
// camera's clock and invalidates every drift figure measured against it.
struct CameraSnapshot {
  // Whether the daemon said anything about cameras at all. A daemon built
  // before it could watch for one says nothing, and that is a different fact
  // from "the camera is off" -- the first means find out the expensive way,
  // the second means there is nothing to find out.
  bool reported = false;
  bool seen = false;      // heard from at least once since the daemon started
  bool present = false;   // heard from recently enough to believe
  std::string id;
  std::string name;
  int rssi = 0;
  double age = 0.0;         // since the last advertisement
  double since = 0.0;       // how long it has been in the present state
  uint64_t sessions = 0;    // absent -> present transitions, including the first
  uint64_t adverts = 0;
  double up_wall = 0.0;     // when the current session began
};

struct Snapshot {
  double wall = 0.0;
  double uptime = 0.0;
  std::string radio = "unknown";
  uint64_t adverts_total = 0;
  uint64_t undecodable_total = 0;
  uint64_t clock_steps = 0;
  // Readings measured against a free-running reference rather than a real
  // clock. Nonzero on a box nobody has told the time to; always zero on a Mac.
  uint64_t free_running_total = 0;

  // Whether the offsets below can be compared to real time, or only to each
  // other.
  //
  // False on a dongle that has not been told the time. Everything about the
  // *spread* is still exact -- that is a difference, and the unknown constant
  // cancels -- but the offsets themselves are all displaced by that constant,
  // so "this box is 6 seconds slow" is not a sentence this snapshot supports.
  // Anything that prints an absolute offset, or writes one to a camera, has to
  // ask first.
  bool wall_is_real = true;

  int devices = 0;
  int live = 0;

  // Rows in `device` that some other radio heard -- a dongle on a cable, or
  // one reached over Bluetooth. Counted separately and NOT included in
  // `devices` or `live` above, so that a reader which knows nothing about
  // second radios still gets right answers about this one. The array is
  // longer than `devices` by exactly `remote_devices`.
  //
  // The bench figures below are always this machine's radio alone. Folding in
  // another radio's readings would add the distance between two machines'
  // clocks to a figure that is supposed to be the distance between two
  // timecode boxes, and would count every box in earshot of both twice.
  int remote_devices = 0;
  int remote_live = 0;
  int alerting = 0;
  bool has_bench = false;
  double bench_offset = 0.0;  // median across live boxes
  double bench_spread = 0.0;  // worst disagreement between live boxes

  double alert_threshold = 60.0;

  CameraSnapshot camera;

  std::vector<DeviceSnapshot> device;
};

class Registry {
 public:
  // The monotonic origin is injectable so tests can drive the whole thing
  // from a synthetic clock; nothing here should ever consult a clock it was
  // not handed.
  explicit Registry(Policy policy = {});
  Registry(Policy policy, double start_mono);

  // One advertisement. `data` is the raw FDAC service-data payload.
  // A name, from a report that carried no reading -- a scan response. Updates
  // a device that already exists and creates nothing: a name is not a
  // sighting, and a roster that grew a row for every named device in the
  // building would be a different program.
  //
  // Returns whether anything was updated, which is the only thing a caller
  // could usefully do with the answer.
  bool observe_name(const std::string& id, const std::string& name);

  void observe(const std::string& id, const std::string& name, int rssi,
               const uint8_t* data, size_t len, double mono, double wall);

  // One advertisement from something carrying the camera-control service.
  // Nothing is decoded: this only records that it was heard.
  void observe_camera(const std::string& id, const std::string& name, int rssi,
                      double mono, double wall);

  void set_radio(const std::string& state);

  Snapshot snapshot() const;
  Snapshot snapshot(double mono, double wall) const;

  // Seed a device known from a previous run, which has not been heard from
  // yet in this one.
  //
  // Deliberately separate from observe(): this puts no sample in the window,
  // so the device cannot vote on the bench, cannot alert, and cannot produce a
  // drift figure. All of those are measurements of the present, and a
  // remembered device has none. What it has is a name, an age and a last-known
  // reading, which is what "offline since Tuesday" is made of.
  //
  // An id already present is left alone: whatever has been heard this run is
  // better than anything on disk.
  void remember(const RememberedDevice& device, double now_wall);

  // Everything worth keeping across a restart, for src/devicedb.h to write.
  // Includes devices only known from a previous run, so a roster does not
  // erode each time the daemon starts before the boxes are switched on.
  std::vector<RememberedDevice> remembered(double now_wall) const;

  // Throw away everything known about one device, by id. Returns false when
  // there was nothing to throw away.
  //
  // This is not "switch it off": there is no tombstone and nothing is
  // remembered. The next advertisement from that id builds the device again
  // from nothing -- new first_seen, an empty sample window, no drift, no alert
  // state -- which is exactly what somebody removing a box from the list is
  // asking for. A box wrongly removed costs an hour of drift history and
  // nothing else, and the alternative, a list that only ever grows, costs a
  // page full of equipment that left the building years ago.
  bool forget(const std::string& id);

  // Hand the caller every alert transition since the last call.
  std::vector<AlertEvent> take_events();

  const Policy& policy() const { return policy_; }

 private:
  struct Device {
    std::string id;
    std::string name;
    int rssi = 0;
    uint64_t adverts = 0;
    uint64_t decoded = 0;
    double first_seen_wall = 0.0;
    double last_seen_mono = 0.0;
    // Kept alongside the monotonic stamp rather than instead of it. Monotonic
    // time is what ages a device that is here now, and is immune to the wall
    // clock being stepped -- which in this program of all programs is not
    // hypothetical. But it restarts with the process, so a device restored
    // from disk can only be aged against the wall.
    double last_seen_wall = 0.0;
    bool heard_this_run = true;
    // What this device came back from disk carrying, kept verbatim until it is
    // heard again. Without it a roster saved from a roster loaded is not the
    // same roster: a device offline across two restarts would come back with
    // its last reading zeroed, and the file would erode a little each time the
    // daemon started. Round-tripping losslessly is the property, and it is
    // tested.
    bool restored_has_time = false;
    double restored_offset = 0.0;
    double restored_median = 0.0;
    std::string restored_resolution;
    std::deque<Sample> samples;
    Decoded last;
    bool has_last = false;

    bool alerting = false;
    int over = 0;
    int under = 0;
    double alert_since_wall = 0.0;
    double last_notified_mono = 0.0;
  };

  struct Camera {
    std::string id;
    std::string name;
    int rssi = 0;
    uint64_t adverts = 0;
    uint64_t sessions = 0;
    bool seen = false;
    bool present = false;
    double last_seen_mono = 0.0;
    double state_since_mono = 0.0;
    double up_wall = 0.0;
  };

  void trim(Device* dev, double mono);
  void update_alert(Device* dev, double mono, double wall);
  // Age the camera out of the present state. Called from snapshot() as well as
  // from observe_camera(), because a camera that has been switched off sends
  // nothing at all -- there is no event to notice, only a silence to time.
  void age_camera(double mono) const;

  Policy policy_;
  std::map<std::string, Device> devices_;
  mutable Camera camera_;
  std::string radio_ = "unknown";
  double started_mono_ = 0.0;
  uint64_t adverts_total_ = 0;
  uint64_t undecodable_total_ = 0;
  uint64_t free_running_total_ = 0;
  bool wall_is_real_ = true;
  uint64_t clock_steps_ = 0;
  double last_mono_ = 0.0;
  double last_wall_ = 0.0;
  std::vector<AlertEvent> events_;
};

// Exposed for testing: least-squares slope of offset against time, in parts
// per million, and the median of a set of offsets.
bool fit_drift_ppm(const std::deque<Sample>& samples, double min_span,
                   int min_samples, double* ppm, double* span);
double median_offset(std::vector<double> values);

}  // namespace octo

#endif  // OCTO_REGISTRY_H
