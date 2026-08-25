// The registry is where a plausible wrong answer costs the most: it decides
// whether a box is drifting, and drift is the thing the whole project exists
// to measure. These tests drive it from a synthetic clock so the arithmetic is
// checked rather than the weather.
#include "../src/registry.h"
#include "../src/timeutil.h"
#include "harness.h"

#include <cmath>
#include <vector>

using namespace octo;

namespace {

// A 0x32 payload: microseconds since midnight, 40-bit big-endian. Exact, so
// the test measures the registry rather than frame quantisation.
std::vector<uint8_t> micros_packet(double sod) {
  const uint64_t us = static_cast<uint64_t>(llround(sod * 1e6));
  std::vector<uint8_t> pkt = {0x32, 0x3d, 0x18, 0, 0, 0, 0, 0};
  for (int i = 0; i < 5; ++i) pkt[7 - i] = static_cast<uint8_t>((us >> (8 * i)) & 0xFF);
  return pkt;
}

// A wall-clock instant whose local time of day is known and far from midnight,
// so nothing here accidentally exercises the wrap.
double wall_at(double seconds_in) {
  static const double base = wall_now();
  return base + seconds_in;
}

void feed(Registry* reg, const std::string& id, double offset, double mono,
          double wall) {
  const std::vector<uint8_t> pkt =
      micros_packet(local_seconds_of_day(wall) + offset);
  reg->observe(id, "box", -40, pkt.data(), pkt.size(), mono, wall);
}

void test_offset_and_median() {
  Registry reg({}, 0.0);
  // Nine clean samples at -6.231 s and one wild outlier. The median must
  // ignore the outlier: one mangled advert is not a story about the box.
  for (int i = 0; i < 9; ++i) {
    feed(&reg, "a", -6.231, i, wall_at(i));
  }
  feed(&reg, "a", 5000.0, 9, wall_at(9));

  const Snapshot snap = reg.snapshot(9, wall_at(9));
  CHECK_EQ(snap.devices, 1);
  CHECK_EQ(snap.live, 1);
  const DeviceSnapshot& d = snap.device[0];
  CHECK(d.has_time);
  CHECK_EQ(static_cast<long long>(d.samples), 10LL);
  CHECK_NEAR(d.offset, 5000.0, 1e-3);        // the latest reading, warts and all
  CHECK_NEAR(d.median_offset, -6.231, 1e-3);  // ...but the median is sane
  CHECK(snap.has_bench);
  CHECK_NEAR(snap.bench_offset, -6.231, 1e-3);
}

void test_drift_needs_a_long_lever() {
  Policy policy;
  policy.min_drift_span = 900.0;
  policy.min_drift_samples = 30;

  // A minute of data, sampled fast: plenty of samples, no lever arm. Drift
  // must be refused, not estimated.
  Registry brief(policy, 0.0);
  for (int i = 0; i < 60; ++i) feed(&brief, "a", -6.0, i, wall_at(i));
  CHECK(!brief.snapshot(60, wall_at(60)).device[0].has_drift);

  // An hour at a known +50 ppm: 50 microseconds of offset gained per second.
  Registry hour(policy, 0.0);
  for (int i = 0; i <= 3600; i += 10) {
    feed(&hour, "a", -6.0 + 50e-6 * i, i, wall_at(i));
  }
  const Snapshot hour_snap = hour.snapshot(3600, wall_at(3600));
  const DeviceSnapshot& d = hour_snap.device[0];
  CHECK(d.has_drift);
  CHECK_NEAR(d.drift_ppm, 50.0, 0.5);
  CHECK_NEAR(d.drift_span, 3600.0, 1.0);
}

void test_alert_hysteresis() {
  Policy policy;
  policy.alert_enter = 60.0;
  policy.alert_exit = 45.0;
  policy.alert_confirm = 3;
  Registry reg(policy, 0.0);

  double t = 0;
  // Well inside tolerance: no alert, however long we look.
  for (int i = 0; i < 20; ++i, ++t) feed(&reg, "a", -6.0, t, wall_at(t));
  CHECK_EQ(reg.snapshot(t, wall_at(t)).alerting, 0);
  CHECK(reg.take_events().empty());

  // Now the box is 90 s out. The median is dragged across the threshold only
  // after enough samples, and the alert needs confirming on top of that, so
  // this deliberately feeds well past the bare minimum.
  for (int i = 0; i < 60; ++i, ++t) feed(&reg, "a", 90.0, t, wall_at(t));
  const Snapshot alerted = reg.snapshot(t, wall_at(t));
  CHECK_EQ(alerted.alerting, 1);
  CHECK(alerted.device[0].alerting);

  const std::vector<AlertEvent> events = reg.take_events();
  CHECK_EQ(static_cast<long long>(events.size()), 1LL);
  if (!events.empty()) {
    CHECK(events[0].entering);
    CHECK(!events[0].repeat);
    CHECK_NEAR(events[0].offset, 90.0, 1e-3);
  }

  // Back in agreement: it must clear, and say so exactly once.
  for (int i = 0; i < 60; ++i, ++t) feed(&reg, "a", 0.5, t, wall_at(t));
  CHECK_EQ(reg.snapshot(t, wall_at(t)).alerting, 0);
  const std::vector<AlertEvent> cleared = reg.take_events();
  CHECK_EQ(static_cast<long long>(cleared.size()), 1LL);
  if (!cleared.empty()) CHECK(!cleared[0].entering);
}

void test_no_flapping_in_the_band() {
  Policy policy;
  policy.alert_enter = 60.0;
  policy.alert_exit = 45.0;
  policy.alert_confirm = 3;
  Registry reg(policy, 0.0);
  // Parked between exit and enter. Neither transition may fire, or a box
  // sitting on the threshold would notify forever.
  double t = 0;
  for (int i = 0; i < 200; ++i, ++t) feed(&reg, "a", 52.0, t, wall_at(t));
  CHECK_EQ(reg.snapshot(t, wall_at(t)).alerting, 0);
  CHECK(reg.take_events().empty());
}

void test_host_clock_step_is_not_drift() {
  Policy policy;
  policy.min_drift_span = 100.0;
  policy.min_drift_samples = 10;
  Registry reg(policy, 0.0);

  double t = 0;
  for (int i = 0; i < 200; ++i, ++t) feed(&reg, "a", -6.0, t, wall_at(t));
  CHECK(reg.snapshot(t, wall_at(t)).device[0].has_drift);

  // NTP steps the host forward by an hour. Monotonic time did not move with
  // it, which is how we can tell. Fitting a line through that discontinuity
  // would report a spectacular and entirely fictional drift.
  const double before = reg.snapshot(t, wall_at(t)).clock_steps;
  feed(&reg, "a", -6.0, t + 1, wall_at(t + 1) + 3600.0);
  const Snapshot after = reg.snapshot(t + 1, wall_at(t + 1) + 3600.0);
  CHECK_EQ(static_cast<long long>(after.clock_steps),
           static_cast<long long>(before) + 1);
  CHECK(!after.device[0].has_drift);   // history discarded, not fitted
  CHECK_EQ(static_cast<long long>(after.device[0].samples), 1LL);
}

void test_staleness_and_bench_spread() {
  Policy policy;
  policy.stale_after = 30.0;
  Registry reg(policy, 0.0);
  for (int i = 0; i < 10; ++i) {
    feed(&reg, "a", -6.0, i, wall_at(i));
    feed(&reg, "b", -6.010, i, wall_at(i));
  }
  const Snapshot fresh = reg.snapshot(10, wall_at(10));
  CHECK_EQ(fresh.live, 2);
  CHECK_NEAR(fresh.bench_spread, 0.010, 1e-4);

  // Much later, with nothing heard: both are stale, so the bench figure has
  // nothing current to stand on and must not be reported.
  const Snapshot old = reg.snapshot(10000, wall_at(10000));
  CHECK_EQ(old.live, 0);
  CHECK(!old.has_bench);
  CHECK_EQ(old.devices, 2);  // still listed, just not counted
}

void test_median_helper() {
  CHECK_NEAR(median_offset({1.0}), 1.0, 1e-9);
  CHECK_NEAR(median_offset({1.0, 3.0}), 2.0, 1e-9);
  CHECK_NEAR(median_offset({5.0, 1.0, 3.0}), 3.0, 1e-9);
  CHECK_NEAR(median_offset({4.0, 1.0, 3.0, 2.0}), 2.5, 1e-9);
  CHECK_NEAR(median_offset({}), 0.0, 1e-9);
}


// --- the camera -----------------------------------------------------------

// A camera stops advertising while something holds a connection to it, and
// octomancer-sync holds one for about twenty seconds every cycle. If that read
// as the camera being switched off, every correction would look like a power
// cycle and throw away the drift figure it had just spent an hour measuring.
void test_a_sync_cycle_is_not_a_power_cycle() {
  octo::Policy policy;
  octo::Registry reg(policy, 0.0);

  for (double t = 0.0; t < 60.0; t += 1.0) {
    reg.observe_camera("CAM", "BMPCC 6K Pro", -60, t, 1000.0 + t);
  }
  octo::Snapshot snap = reg.snapshot(60.0, 1060.0);
  CHECK(snap.camera.reported);
  CHECK(snap.camera.seen);
  CHECK(snap.camera.present);
  CHECK_EQ(snap.camera.sessions, 1u);

  // Silent for twenty-five seconds while a write happens: still present.
  snap = reg.snapshot(85.0, 1085.0);
  CHECK(snap.camera.present);
  CHECK_EQ(snap.camera.sessions, 1u);

  // ...and back, with no new session.
  reg.observe_camera("CAM", "BMPCC 6K Pro", -60, 86.0, 1086.0);
  snap = reg.snapshot(86.0, 1086.0);
  CHECK(snap.camera.present);
  CHECK_EQ(snap.camera.sessions, 1u);
}

void test_power_cycle_counts_a_new_session() {
  octo::Policy policy;
  octo::Registry reg(policy, 0.0);

  reg.observe_camera("CAM", "BMPCC", -60, 10.0, 1010.0);
  CHECK(reg.snapshot(10.0, 1010.0).camera.present);

  // Gone for well past the timeout.
  octo::Snapshot gone = reg.snapshot(200.0, 1200.0);
  CHECK(gone.camera.seen);
  CHECK(!gone.camera.present);
  CHECK_EQ(gone.camera.sessions, 1u);
  // The absence is dated from when it stopped talking, not from when the
  // timeout expired.
  CHECK_NEAR(gone.camera.since, 190.0, 1e-6);

  reg.observe_camera("CAM", "BMPCC", -58, 300.0, 1300.0);
  octo::Snapshot back = reg.snapshot(300.0, 1300.0);
  CHECK(back.camera.present);
  CHECK_EQ(back.camera.sessions, 2u);
  CHECK_NEAR(back.camera.up_wall, 1300.0, 1e-6);
  CHECK_NEAR(back.camera.since, 0.0, 1e-6);
}

// Two cameras in the room is not a situation octomancer has an answer for, and
// alternating between them would flap the presence flag every advertisement.
void test_a_second_camera_is_ignored() {
  octo::Policy policy;
  octo::Registry reg(policy, 0.0);

  reg.observe_camera("FIRST", "Ursa", -50, 1.0, 1001.0);
  reg.observe_camera("SECOND", "Pocket", -40, 2.0, 1002.0);

  const octo::Snapshot snap = reg.snapshot(2.0, 1002.0);
  CHECK_STR(snap.camera.id, "FIRST");
  CHECK_STR(snap.camera.name, "Ursa");
  CHECK_EQ(snap.camera.adverts, 1u);
  CHECK_EQ(snap.camera.sessions, 1u);
}

void test_never_seen_is_reported_as_such() {
  octo::Policy policy;
  octo::Registry reg(policy, 0.0);
  const octo::Snapshot snap = reg.snapshot(100.0, 1100.0);
  CHECK(snap.camera.reported);
  CHECK(!snap.camera.seen);
  CHECK(!snap.camera.present);
  CHECK_EQ(snap.camera.sessions, 0u);
}
}  // namespace

int main() {
  test_median_helper();
  test_offset_and_median();
  test_drift_needs_a_long_lever();
  test_alert_hysteresis();
  test_no_flapping_in_the_band();
  test_host_clock_step_is_not_drift();
  test_staleness_and_bench_spread();
  test_a_sync_cycle_is_not_a_power_cycle();
  test_power_cycle_counts_a_new_session();
  test_a_second_camera_is_ignored();
  test_never_seen_is_reported_as_such();
  return octotest::report("test_registry");
}
