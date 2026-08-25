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
  std::lock_guard<std::mutex> lock(mu_);
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

void Registry::observe(const std::string& id, const std::string& name, int rssi,
                       const uint8_t* data, size_t len, double mono,
                       double wall) {
  std::lock_guard<std::mutex> lock(mu_);

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

  const Decoded decoded = decode(data, len);
  dev.last = decoded;
  dev.has_last = true;
  if (!decoded.ok) {
    undecodable_total_++;
    return;
  }

  dev.decoded++;
  // The box states a local time of day; so does the host. Their difference is
  // what "how far off is this box" means, wrapped so that a box a second past
  // midnight is not reported as almost a day fast.
  const double offset = wrap_delta(decoded.sod - local_seconds_of_day(wall));
  dev.samples.push_back({mono, offset});
  trim(&dev, mono);
  update_alert(&dev, mono, wall);

  // Forget boxes that have genuinely gone away, so a long-running agent does
  // not accumulate every Tentacle it has ever met.
  for (auto it = devices_.begin(); it != devices_.end();) {
    if (it->first != id && mono - it->second.last_seen_mono > policy_.forget_after) {
      it = devices_.erase(it);
    } else {
      ++it;
    }
  }
}

Snapshot Registry::snapshot() const { return snapshot(mono_now(), wall_now()); }

Snapshot Registry::snapshot(double mono, double wall) const {
  std::lock_guard<std::mutex> lock(mu_);

  Snapshot snap;
  snap.wall = wall;
  snap.uptime = mono - started_mono_;
  snap.radio = radio_;
  snap.adverts_total = adverts_total_;
  snap.undecodable_total = undecodable_total_;
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
    out.age = mono - dev.last_seen_mono;
    out.first_seen_wall = dev.first_seen_wall;
    out.live = out.age <= policy_.stale_after;
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

  if (!live_medians.empty()) {
    snap.has_bench = true;
    snap.bench_offset = median_offset(live_medians);
    const auto minmax = std::minmax_element(live_medians.begin(), live_medians.end());
    snap.bench_spread = *minmax.second - *minmax.first;
  }
  return snap;
}

std::vector<AlertEvent> Registry::take_events() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<AlertEvent> out;
  out.swap(events_);
  return out;
}

}  // namespace octo
