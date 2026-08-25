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

  void trim(Device* dev, double mono);
  void update_alert(Device* dev, double mono, double wall);

  mutable std::mutex mu_;
  Policy policy_;
  std::map<std::string, Device> devices_;
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
