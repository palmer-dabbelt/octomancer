#include "registry.h"

#include <algorithm>
#include <cmath>

#include "timeutil.h"

namespace octo {

double median_offset(std::vector<double> values) {
  if (values.empty()) return 0.0;
  const size_t mid = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + mid, values.end());
  const double hi = values[mid];
  if (values.size() % 2 == 1) return hi;
  // Even count: average the two middle values, which needs the largest of the
  // lower half. nth_element has already partitioned, so this is a linear scan.
  const double lo = *std::max_element(values.begin(), values.begin() + mid);
  return (lo + hi) / 2.0;
}

bool fit_drift_ppm(const std::deque<Sample>& samples, double min_span,
                   int min_samples, double* ppm, double* span) {
  // Report the lever arm even when refusing to fit, so a caller can show how
  // much longer it needs to watch rather than a bare and unexplained dash.
  if (span) {
    *span = samples.empty() ? 0.0
                            : samples.back().mono - samples.front().mono;
  }
  if (samples.size() < static_cast<size_t>(min_samples)) return false;
  const double t0 = samples.front().mono;
  const double arm = samples.back().mono - t0;
  // Drift on these clocks is parts per million, so measuring it needs a long
  // lever arm, not more samples. A minute of data will happily produce a
  // confident-looking number that is entirely frame quantisation.
  if (arm < min_span) return false;

  double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (const Sample& s : samples) {
    const double x = s.mono - t0;
    const double y = s.offset;
    n += 1.0;
    sx += x;
    sy += y;
    sxx += x * x;
    sxy += x * y;
  }
  const double denom = n * sxx - sx * sx;
  if (std::fabs(denom) < 1e-9) return false;
  const double slope = (n * sxy - sx * sy) / denom;  // seconds per second
  if (!std::isfinite(slope)) return false;
  if (ppm) *ppm = slope * 1e6;
  return true;
}

Registry::Registry(Policy policy) : Registry(policy, mono_now()) {}

Registry::Registry(Policy policy, double start_mono)
    : policy_(policy), started_mono_(start_mono) {}

void Registry::set_radio(const std::string& state) {
  radio_ = state;
}

void Registry::trim(Device* dev, double mono) {
  const double cutoff = mono - policy_.window;
  while (!dev->samples.empty() && dev->samples.front().mono < cutoff) {
    dev->samples.pop_front();
  }
  while (dev->samples.size() > policy_.max_samples) dev->samples.pop_front();
}

void Registry::update_alert(Device* dev, double mono, double wall) {
  if (dev->samples.empty()) return;
  std::vector<double> offsets;
  offsets.reserve(dev->samples.size());
  for (const Sample& s : dev->samples) offsets.push_back(s.offset);
  const double median = median_offset(std::move(offsets));
  const double mag = std::fabs(median);

  // Judge on the median rather than the latest reading: a single advert that
  // arrives mangled should not be able to declare the box out of sync, and it
  // should not be able to declare it healthy again either.
  if (mag > policy_.alert_enter) {
    dev->over++;
    dev->under = 0;
  } else if (mag < policy_.alert_exit) {
    dev->under++;
    dev->over = 0;
  } else {
    // Inside the hysteresis band: hold whatever state we are in.
    dev->over = 0;
    dev->under = 0;
  }

  if (!dev->alerting && dev->over >= policy_.alert_confirm) {
    dev->alerting = true;
    dev->alert_since_wall = wall;
    dev->last_notified_mono = mono;
    events_.push_back({dev->id, dev->name, median, wall, true, false});
  } else if (dev->alerting && dev->under >= policy_.alert_confirm) {
    dev->alerting = false;
    dev->alert_since_wall = 0.0;
    events_.push_back({dev->id, dev->name, median, wall, false, false});
  } else if (dev->alerting &&
             mono - dev->last_notified_mono >= policy_.renotify_after) {
    dev->last_notified_mono = mono;
    events_.push_back({dev->id, dev->name, median, wall, true, true});
  }
}

bool Registry::observe_name(const std::string& id, const std::string& name) {
  if (id.empty() || name.empty()) return false;
  const auto it = devices_.find(id);
  if (it == devices_.end()) return false;
  if (it->second.name == name) return false;
  it->second.name = name;
  return true;
}

void Registry::observe(const std::string& id, const std::string& name, int rssi,
                       const uint8_t* data, size_t len, double mono,
                       double wall) {
  // If the host clock has been stepped -- NTP correcting it, or the user
  // changing timezone -- every offset moves at once and the jump would be
  // recorded as a spectacular drift. Monotonic time does not step, so
  // comparing the two advances catches it. Throw the history away rather than
  // fitting a line through a discontinuity.
  if (last_mono_ > 0.0) {
    const double mono_delta = mono - last_mono_;
    const double wall_delta = wall - last_wall_;
    if (std::fabs(wall_delta - mono_delta) > 1.0) {
      clock_steps_++;
      for (auto& entry : devices_) {
        entry.second.samples.clear();
        entry.second.over = 0;
        entry.second.under = 0;
      }
    }
  }
  last_mono_ = mono;
  last_wall_ = wall;

  adverts_total_++;

  Device& dev = devices_[id];
  if (dev.id.empty()) {
    dev.id = id;
    dev.first_seen_wall = wall;
  }
  if (!name.empty()) dev.name = name;
  dev.rssi = rssi;
  dev.adverts++;
  dev.last_seen_mono = mono;
  dev.last_seen_wall = wall;
  dev.heard_this_run = true;

  const Decoded decoded = decode(data, len);
  dev.last = decoded;
  dev.has_last = true;
  if (!decoded.ok) {
    undecodable_total_++;
    return;
  }

  dev.decoded++;
  // The box states a local time of day; so does this machine. Their difference
  // is what "how far off is this box" means, wrapped so that a box a second
  // past midnight is not reported as almost a day fast.
  //
  // The reference does not have to be a real clock. A dongle boots without
  // one -- no network, no RTC, nobody to ask -- and waiting for a host before
  // measuring anything would make a dongle in a bag useless, which is not what
  // it is for. So a machine with no real clock measures against its own
  // free-running one instead: every offset is then shifted by a single unknown
  // constant, and every *difference* between them survives untouched. The
  // spread across the bench, which is the number this exists to produce, is
  // exact either way.
  //
  // What the constant costs is the absolute question -- "is this box right?"
  // as against "do these boxes agree?" -- so which kind of reference was used
  // travels out in Snapshot::wall_is_real. Nothing downstream may report one
  // as though it were the other.
  const bool real = wall_known(wall);
  wall_is_real_ = real;
  if (!real) ++free_running_total_;
  // Deliberately not local_seconds_of_day() in the free-running case. That
  // asks the C library for a timezone, and applying a real zone to a clock
  // that starts at zero is arithmetic with one meaningful operand. Taking the
  // day modulus directly is the same reference on every host, which is what
  // lets a Mac test what a dongle will do.
  const double ours = real ? local_seconds_of_day(wall)
                           : seconds_of_day_at_offset(wall, 0);
  const double offset = wrap_delta(decoded.sod - ours);
  dev.samples.push_back({mono, offset});
  trim(&dev, mono);
  update_alert(&dev, mono, wall);

  // Forget boxes that have genuinely gone away, so a sync daemon does not
  // accumulate every Tentacle it has ever met -- it may be a box with nothing
  // but NVS to hold them in.
  //
  // Zero means never, which is what octomancerd sets: it has a filesystem and
  // is supposed to remember. A device restored from disk is skipped either
  // way, because it has no monotonic stamp from this run to age against and
  // ageing it against zero would delete the entire remembered roster on the
  // first advertisement.
  if (policy_.forget_after > 0.0) {
    for (auto it = devices_.begin(); it != devices_.end();) {
      if (it->first != id && it->second.heard_this_run &&
          mono - it->second.last_seen_mono > policy_.forget_after) {
        it = devices_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void Registry::observe_camera(const std::string& id, const std::string& name,
                              int rssi, double mono, double wall) {
  // A second camera in the room is not something octomancer has an answer for,
  // and picking one arbitrarily every advertisement would flap the presence
  // flag between them. Keep the first one heard and ignore the rest; the sync
  // daemon's --camera hint is where choosing between cameras belongs.
  if (camera_.seen && !camera_.id.empty() && camera_.id != id) return;

  age_camera(mono);

  camera_.id = id;
  if (!name.empty()) camera_.name = name;
  camera_.rssi = rssi;
  camera_.adverts += 1;
  camera_.last_seen_mono = mono;
  camera_.seen = true;

  if (!camera_.present) {
    camera_.present = true;
    camera_.sessions += 1;
    camera_.state_since_mono = mono;
    camera_.up_wall = wall;
  }
}

void Registry::age_camera(double mono) const {
  if (!camera_.present) return;
  if (mono - camera_.last_seen_mono <= policy_.camera_gone_after) return;
  camera_.present = false;
  // Date the absence from the last time it was heard, not from the moment the
  // timeout expired: the camera went away when it stopped talking.
  camera_.state_since_mono = camera_.last_seen_mono;
}

Snapshot Registry::snapshot() const { return snapshot(mono_now(), wall_now()); }

Snapshot Registry::snapshot(double mono, double wall) const {
  Snapshot snap;
  snap.wall = wall;
  snap.uptime = mono - started_mono_;
  snap.radio = radio_;
  snap.adverts_total = adverts_total_;
  snap.undecodable_total = undecodable_total_;
  snap.free_running_total = free_running_total_;
  snap.wall_is_real = wall_is_real_;
  snap.clock_steps = clock_steps_;
  snap.alert_threshold = policy_.alert_enter;

  std::vector<double> live_medians;
  for (const auto& entry : devices_) {
    const Device& dev = entry.second;
    DeviceSnapshot out;
    out.id = dev.id;
    out.name = dev.name.empty() ? std::string("(unnamed)") : dev.name;
    out.rssi = dev.rssi;
    out.adverts = dev.adverts;
    out.decoded = dev.decoded;
    // Monotonic while the device is one this run has heard; wall-clock when it
    // came off disk, because there is no monotonic stamp from this process to
    // subtract and the answer wanted is "how long since anybody heard it",
    // which spans restarts.
    out.heard_this_run = dev.heard_this_run;
    out.age = dev.heard_this_run
                  ? mono - dev.last_seen_mono
                  : wall - dev.last_seen_wall;
    // A wall clock that has been stepped backwards -- or a roster copied from
    // another machine -- can put the last sighting in the future. Reporting a
    // negative age would render as a device heard in several hours' time.
    if (out.age < 0.0) out.age = 0.0;
    out.first_seen_wall = dev.first_seen_wall;
    out.live = dev.heard_this_run && out.age <= policy_.stale_after;
    out.alerting = dev.alerting;
    out.alert_since_wall = dev.alert_since_wall;
    if (dev.has_last) {
      out.note = dev.last.note;
      out.resolution = resolution_name(dev.last.resolution);
      out.display = dev.last.display;
      out.fps = dev.last.fps;
    }
    if (!dev.samples.empty()) {
      out.has_time = true;
      out.sod = dev.last.ok ? dev.last.sod : 0.0;
      out.offset = dev.samples.back().offset;
      out.samples = static_cast<int>(dev.samples.size());
      std::vector<double> offsets;
      offsets.reserve(dev.samples.size());
      for (const Sample& s : dev.samples) offsets.push_back(s.offset);
      out.median_offset = median_offset(std::move(offsets));
      out.has_drift = fit_drift_ppm(dev.samples, policy_.min_drift_span,
                                    policy_.min_drift_samples, &out.drift_ppm,
                                    &out.drift_span);
      if (out.live) live_medians.push_back(out.median_offset);
    }
    if (out.live) snap.live++;
    if (out.alerting) snap.alerting++;
    snap.device.push_back(std::move(out));
  }
  snap.devices = static_cast<int>(snap.device.size());

  // Strongest signal first is the wrong order for a table someone reads; sort
  // by name so a box keeps its row between refreshes.
  std::sort(snap.device.begin(), snap.device.end(),
            [](const DeviceSnapshot& a, const DeviceSnapshot& b) {
              if (a.live != b.live) return a.live;
              if (a.name != b.name) return a.name < b.name;
              return a.id < b.id;
            });

  age_camera(mono);
  snap.camera.reported = true;
  snap.camera.seen = camera_.seen;
  snap.camera.present = camera_.present;
  snap.camera.id = camera_.id;
  snap.camera.name = camera_.name;
  snap.camera.rssi = camera_.rssi;
  snap.camera.adverts = camera_.adverts;
  snap.camera.sessions = camera_.sessions;
  snap.camera.up_wall = camera_.up_wall;
  if (camera_.seen) {
    snap.camera.age = mono - camera_.last_seen_mono;
    snap.camera.since = mono - camera_.state_since_mono;
  }

  if (!live_medians.empty()) {
    snap.has_bench = true;
    snap.bench_offset = median_offset(live_medians);
    const auto minmax = std::minmax_element(live_medians.begin(), live_medians.end());
    snap.bench_spread = *minmax.second - *minmax.first;
  }
  return snap;
}

void Registry::remember(const RememberedDevice& device, double now_wall) {
  if (device.id.empty()) return;
  // Anything heard this run beats anything on disk, so an existing entry is
  // left exactly as it is. This also makes load-then-observe and
  // observe-then-load produce the same result, which matters because the
  // daemon does the first and a test will do the second.
  if (devices_.count(device.id) != 0) return;

  Device& dev = devices_[device.id];
  dev.id = device.id;
  dev.name = device.name;
  dev.rssi = device.rssi;
  dev.first_seen_wall = device.first_seen_wall;
  dev.last_seen_wall = device.last_seen_wall;
  dev.heard_this_run = false;
  // No monotonic stamp: there is no instant in this process's life when this
  // was heard. snapshot() ages it against the wall instead, and every path
  // that would use last_seen_mono is guarded on heard_this_run.
  dev.last_seen_mono = 0.0;

  // No sample, deliberately -- see the header. What is restored is the last
  // *reading*, and it is kept apart from the sample window rather than pushed
  // into it: a sample is evidence, and evidence from a device that has been
  // switched off for a week must not vote on what time it is.
  dev.restored_has_time = device.has_time;
  dev.restored_offset = device.offset;
  dev.restored_median = device.median_offset;
  dev.restored_resolution = device.resolution;
  dev.last.fps = device.fps;
  (void)now_wall;
}

std::vector<RememberedDevice> Registry::remembered(double now_wall) const {
  std::vector<RememberedDevice> out;
  out.reserve(devices_.size());
  for (const auto& entry : devices_) {
    const Device& dev = entry.second;
    RememberedDevice d;
    d.id = dev.id;
    d.name = dev.name;
    d.first_seen_wall = dev.first_seen_wall;
    // A device heard this run has a wall stamp from when it was heard; one
    // restored from disk keeps the stamp it came with. Neither is `now`, which
    // is what a naive implementation would write and which would make every
    // device look freshly seen after a restart -- turning the file into a lie
    // that says the whole roster was on the air at the moment of shutdown.
    d.last_seen_wall = dev.last_seen_wall;
    d.rssi = dev.rssi;
    if (!dev.samples.empty()) {
      d.has_time = true;
      d.offset = dev.samples.back().offset;
      std::vector<double> offsets;
      offsets.reserve(dev.samples.size());
      for (const Sample& sample : dev.samples) offsets.push_back(sample.offset);
      d.median_offset = median_offset(offsets);
      d.resolution = resolution_name(dev.last.resolution);
    } else {
      // Restored and not heard again: hand back exactly what it came with, so
      // the file does not lose a field per restart.
      d.has_time = dev.restored_has_time;
      d.offset = dev.restored_offset;
      d.median_offset = dev.restored_median;
      d.resolution = dev.restored_resolution;
    }
    d.fps = dev.last.fps;
    out.push_back(d);
  }
  (void)now_wall;
  return out;
}

bool Registry::forget(const std::string& id) {
  bool gone = devices_.erase(id) > 0;
  // The camera is a single slot rather than a map, so forgetting it is
  // resetting it. Kept in the same call because a person looking at one list
  // of devices should not have to know which of them this daemon stores
  // differently.
  if (camera_.seen && camera_.id == id) {
    camera_ = Camera();
    gone = true;
  }
  return gone;
}

std::vector<AlertEvent> Registry::take_events() {
  std::vector<AlertEvent> out;
  out.swap(events_);
  return out;
}

}  // namespace octo
