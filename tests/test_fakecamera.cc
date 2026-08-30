// The camera that is not there.
//
// One property matters more than the rest and the others are mostly here to
// keep it honest: *a write moves the camera's clock to the value that was
// written, at the moment the packet lands.* Everything octomancer does
// converges on that, and until there was a fake camera it could only be
// checked with a real one switched on in front of somebody.
//
// What this cannot check is whether the timing is right on real hardware. A
// fake camera applies a write when it is told to; a real one has a radio, a
// stack and a firmware between the packet and the clock. doc/dongle-notes.md's
// distinction holds -- these are pinned to a model, not to a published vector
// -- and doc/fake-notes.md says so.
#include <cmath>
#include <string>
#include <vector>

#include "bmd.h"
#include "camera.h"
#include "fakebench.h"
#include "harness.h"
#include "radio.h"
#include "timeutil.h"

namespace {

// The fake link is built from the process-wide radio options, the same way
// every real factory is, so a test has to set them.
std::unique_ptr<octo::CameraLink> link_with(const std::string& spec) {
  octo::radio_options().kind = octo::RadioKind::kFake;
  octo::radio_options().fake = spec;
  return octo::make_fake_camera_link();
}

// Seconds since local midnight for a timecode the camera reported.
double sod_of(const octo::bmd::Timecode& tc, int fps) {
  return tc.hours * 3600.0 + tc.minutes * 60.0 + tc.seconds +
         tc.frames / static_cast<double>(fps);
}

double now_sod() {
  const double now = octo::wall_now();
  const time_t whole = static_cast<time_t>(std::floor(now));
  struct tm local;
  localtime_r(&whole, &local);
  return local.tm_hour * 3600.0 + local.tm_min * 60.0 + local.tm_sec +
         (now - std::floor(now));
}

// One frame at 24 fps, which is the finest anything here can be checked to:
// the camera reports whole frames and nothing smaller. Every tolerance below
// is this plus a little slack for the test's own scheduling, and saying so
// once is better than a different magic number on each line.
const double kFrame = 1.0 / 24.0;
const double kSlop = kFrame + 0.05;

void test_a_camera_reports_the_error_it_was_given() {
  for (double error : {0.0, -0.25, 1.5}) {
    auto link = link_with("cam,cam-1,Studio," + std::to_string(error));
    CHECK(link != nullptr);
    if (!link) continue;
    std::string err;
    CHECK(link->connect("cam-1", 1.0, &err));
    const octo::CameraView v = link->view();
    CHECK(v.has_timecode);
    if (!v.has_timecode) continue;
    double diff = sod_of(v.timecode, v.fps) - now_sod();
    CHECK_NEAR(diff, error, kSlop);
  }
}

// The one that matters. Write a time, and the camera afterwards reports that
// time -- not the one it had, and not the one it would have had.
void test_a_write_moves_the_clock_to_what_was_written() {
  // No transit latency, so the expected answer is exact rather than a model
  // of the link. Latency has its own test below.
  auto link = link_with("cam,cam-1,Studio,-5.0,24,0");
  CHECK(link != nullptr);
  if (!link) return;
  std::string err;
  CHECK(link->connect("cam-1", 1.0, &err));

  // Five seconds out to start with, and visibly so.
  const octo::CameraView before = link->view();
  CHECK(before.has_timecode);
  CHECK_NEAR(sod_of(before.timecode, before.fps) - now_sod(), -5.0, kSlop);

  // Write the current time. The RTC is specified in UTC -- writing local time
  // is the double-offset bug bmd.h warns about, and a fake that accepted local
  // time would hide it.
  const double now = octo::wall_now();
  const double dropped = now - std::floor(now);
  const octo::bmd::Civil utc = octo::bmd::utc_civil(now);
  CHECK(link->write_control(octo::bmd::rtc_packet(utc, 0), 1.0, &err));
  CHECK_STR(err, "");

  link->forget_timecode();
  const octo::CameraView after = link->await_state(1.0);
  CHECK(after.has_timecode);
  if (!after.has_timecode) return;

  // Five seconds out becomes a fraction of one out -- and the fraction is not
  // slack, it is the whole reason the daemon aims a write at a second
  // boundary. The RTC field carries whole seconds, so writing the truncated
  // value of an arbitrary instant leaves the camera behind by exactly the
  // fraction that was dropped.
  //
  // The expectation is computed from that fraction rather than arranged by
  // waiting for a boundary, deliberately: a test that only passes at some
  // phases of the wall clock is a test that fails a few times in a hundred
  // runs and looks like a real bug. tests/test_syncd.cc has the scar.
  CHECK_NEAR(sod_of(after.timecode, after.fps) - now_sod(), -dropped, kSlop);
}

// Transit latency, which is the reason the daemon writes early at all. A
// packet that lands `L` late carries a value that is `L` stale, so a camera
// written with the exact time ends up `L` slow -- and that is the signal the
// lead-learning loop upstairs converges on. A fake without this left every
// write a whole lead fast and had nothing for that loop to learn from.
void test_transit_latency_makes_a_write_land_late() {
  const double latency = 0.30;  // large enough to see past frame quantisation
  auto link = link_with("cam,cam-1,Studio,0,24," + std::to_string(latency));
  CHECK(link != nullptr);
  if (!link) return;
  std::string err;
  CHECK(link->connect("cam-1", 1.0, &err));

  const double now = octo::wall_now();
  const double dropped = now - std::floor(now);
  const octo::bmd::Civil utc = octo::bmd::utc_civil(now);
  CHECK(link->write_control(octo::bmd::rtc_packet(utc, 0), 1.0, &err));
  link->forget_timecode();
  const octo::CameraView after = link->await_state(1.0);
  CHECK(after.has_timecode);
  if (!after.has_timecode) return;
  // Behind by the transit time, on top of the truncated fraction from the
  // test above. Both are real and they add.
  CHECK_NEAR(sod_of(after.timecode, after.fps) - now_sod(),
             -(dropped + latency), kSlop);
}

// A camera that reports something other than BCD in the RTC field is a camera
// this program has misunderstood, and the fake must not paper over it -- the
// whole reason bmd.h decodes BCD strictly is that a non-BCD word read as BCD
// gives a believable wrong time.
void test_a_non_bcd_rtc_is_refused() {
  auto link = link_with("cam,cam-1,Studio,0");
  CHECK(link != nullptr);
  if (!link) return;
  std::string err;
  CHECK(link->connect("cam-1", 1.0, &err));

  // Group 7 parameter 0 with 0x1F in the seconds nibble: not a decimal digit.
  std::vector<uint8_t> packet =
      octo::bmd::build_packet(7, 0, /*type=*/3, /*op=*/0,
                              std::vector<uint8_t>{0x1F, 0x5A, 0x34, 0x12,
                                                   0x01, 0x01, 0x70, 0x19});
  err.clear();
  CHECK(!link->write_control(packet, 1.0, &err));
  CHECK(!err.empty());
}

// Connection state, checked because the real link enforces it and a caller
// that subscribes twice has a bug this fake exists to catch rather than to
// tolerate.
void test_the_connection_behaves_like_a_connection() {
  auto link = link_with("cam,cam-1,Studio,0");
  CHECK(link != nullptr);
  if (!link) return;
  std::string err;

  CHECK(!link->connected());
  CHECK(!link->subscribe(1.0, &err));  // nothing to subscribe to yet

  CHECK(link->connect("cam-1", 1.0, &err));
  CHECK(link->connected());
  CHECK(!link->subscribed());
  CHECK(link->subscribe(1.0, &err));
  CHECK(link->subscribed());
  err.clear();
  CHECK(!link->subscribe(1.0, &err));  // twice is an error, as on hardware
  CHECK(!err.empty());

  // A camera that is not the one on the bench is not found.
  CHECK(!link->connect("some-other-camera", 1.0, &err));

  link->disconnect();
  CHECK(!link->connected());
  CHECK(!link->subscribed());
}

// A scan has to identify the camera by service UUID rather than by name. A
// fake that matched on the name would let a caller pass that would fail
// against a real bench, where a Tentacle named after its camera answers a name
// guess and then has no control characteristic -- the trap camera.h warns
// about.
void test_a_scan_identifies_by_service_uuid() {
  auto link = link_with("box,BMPCC,-1;cam,cam-1,BMPCC,0");
  CHECK(link != nullptr);
  if (!link) return;
  int seen = 0;
  const octo::ScanResult r =
      link->scan(0.1, "", false, [&seen](const octo::CameraDevice&) { ++seen; });
  CHECK_EQ((int)r.cameras.size(), 1);
  CHECK_EQ(seen, 1);
  if (!r.cameras.empty()) {
    CHECK(r.cameras[0].by_service_uuid);
    CHECK_STR(r.cameras[0].id, "cam-1");
  }
  // The box on the same bench shares the camera's name and must not be
  // counted as a camera.
  CHECK_EQ(r.tentacles, 1);
}

// A bench with no camera in it is refused with an explanation rather than
// handing back a link that finds nothing, which would look identical to a
// camera that is switched off.
void test_a_bench_with_no_camera_has_no_link() {
  auto link = link_with("box,A,-1");
  CHECK(link == nullptr);
}

}  // namespace

int main() {
  test_a_camera_reports_the_error_it_was_given();
  test_a_write_moves_the_clock_to_what_was_written();
  test_transit_latency_makes_a_write_land_late();
  test_a_non_bcd_rtc_is_refused();
  test_the_connection_behaves_like_a_connection();
  test_a_scan_identifies_by_service_uuid();
  test_a_bench_with_no_camera_has_no_link();
  return octotest::report("test_fakecamera");
}
