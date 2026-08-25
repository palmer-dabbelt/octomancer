// What the daemon knows about the boxes in the room.
//
// The registry is the only mutable state in the service. The radio delivers
// advertisements on a dispatch queue while the socket loop serves clients from
// another thread, so every entry point here takes a lock and every read hands
// back a self-contained snapshot rather than a pointer into live state.
#ifndef OCTO_REGISTRY_H
#define OCTO_REGISTRY_H

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

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
  int rssi = 0;
  uint64_t adverts = 0;
  uint64_t decoded = 0;
  double age = 0.0;          // seconds since the last advertisement
  double first_seen_wall = 0.0;
  bool live = false;         // heard from recently enough to count

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

  int devices = 0;
  int live = 0;
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
  void observe(const std::string& id, const std::string& name, int rssi,
               const uint8_t* data, size_t len, double mono, double wall);

  // One advertisement from something carrying the camera-control service.
  // Nothing is decoded: this only records that it was heard.
  void observe_camera(const std::string& id, const std::string& name, int rssi,
                      double mono, double wall);

  void set_radio(const std::string& state);

  Snapshot snapshot() const;
  Snapshot snapshot(double mono, double wall) const;

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

  mutable std::mutex mu_;
  Policy policy_;
  std::map<std::string, Device> devices_;
  mutable Camera camera_;
  std::string radio_ = "unknown";
  double started_mono_ = 0.0;
  uint64_t adverts_total_ = 0;
  uint64_t undecodable_total_ = 0;
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
