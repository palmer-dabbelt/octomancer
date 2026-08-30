// The bench that is not there.
//
// A fake radio is only worth having if it is trustworthy in a specific way: a
// program driven by it has to fail for the same reasons it would fail on real
// hardware, and pass for the same reasons. So what is checked here is not
// "does it emit adverts" but the handful of properties everything above it
// silently relies on -- that a box's clock is where it was said to be, that no
// advert is delivered twice or lost when the caller polls irregularly, and
// that a box which goes off the air actually stops transmitting.
//
// The last one is the reason this file exists at all. Every one of those cases
// is an afternoon's work to arrange with five boxes in a room, which is why
// they have never been tested.
#include <cmath>
#include <set>
#include <string>

#include "fakebench.h"
#include "harness.h"
#include "tentacle.h"

using octo::Advert;
using octo::FakeBench;
using octo::FakeBox;

namespace {

// A wall instant on a whole second, so that seconds-of-day arithmetic in the
// expectations is exact rather than nearly exact. Which second does not
// matter; that it is a real one does, because the boxes report local time of
// day and localtime_r has to have something to work with.
const double kWall0 = 1788117083.0;

FakeBench one_box(double offset, double drift = 0.0,
                  FakeBox::Kind kind = FakeBox::Kind::kFrameMicros) {
  FakeBench b;
  FakeBox box;
  box.id = "box-1";
  box.name = "Solo";
  box.offset_s = offset;
  box.drift_ppm = drift;
  box.kind = kind;
  box.interval_s = 0.5;
  b.boxes.push_back(box);
  return b;
}

// What a program above the radio would work out: decode the advert, and see
// how far the box is from the true time at the instant it transmitted.
double measured_offset(const Advert& a, double wall0) {
  const octo::Decoded d = octo::decode(a.data);
  if (!d.ok) return 1e9;
  // seconds-of-day of the instant this advert was sent
  const time_t whole = static_cast<time_t>(std::floor(a.wall));
  struct tm local;
  localtime_r(&whole, &local);
  const double true_sod = local.tm_hour * 3600.0 + local.tm_min * 60.0 +
                          local.tm_sec + (a.wall - std::floor(a.wall));
  double diff = d.sod - true_sod;
  // Around midnight the two straddle the wrap; nothing here does, but a test
  // that silently depended on that would be a trap for whoever moves kWall0.
  if (diff > 43200.0) diff -= 86400.0;
  if (diff < -43200.0) diff += 86400.0;
  return diff;
}

// The property the whole thing rests on: a box asked to be 3.6 s slow reads
// 3.6 s slow to whatever is listening.
void test_a_box_reports_the_offset_it_was_given() {
  for (double offset : {0.0, -3.5928, 0.25, -0.001}) {
    const FakeBench b = one_box(offset);
    const std::vector<Advert> adverts = octo::adverts_between(b, -1.0, 2.0, 0.0, kWall0);
    CHECK(!adverts.empty());
    for (const Advert& a : adverts) {
      // One frame at 24 fps is 42 ms and the encoder rounds into it, so the
      // tolerance is the format's, not the model's.
      CHECK_NEAR(measured_offset(a, kWall0), offset, 1e-5);
    }
  }
}

// Drift is what a real bench does and what the estimator upstairs exists for.
// A box that held still would let a broken estimator pass.
void test_a_box_drifts() {
  const double ppm = -23.1;
  const FakeBench b = one_box(-3.0, ppm);
  const std::vector<Advert> adverts =
      octo::adverts_between(b, -1.0, 3600.0, 0.0, kWall0);
  CHECK(!adverts.empty());
  const Advert& first = adverts.front();
  const Advert& last = adverts.back();
  CHECK_NEAR(measured_offset(first, kWall0), -3.0, 1e-5);
  // An hour at -23.1 ppm is -83 ms, which is far larger than a frame and so
  // is a real measurement rather than rounding.
  const double expected = -3.0 + ppm * 1e-6 * last.mono;
  CHECK_NEAR(measured_offset(last, kWall0), expected, 1e-5);
  CHECK(std::fabs(measured_offset(last, kWall0) -
                  measured_offset(first, kWall0)) > 0.05);
}

// The delivery property. A caller polling on a timer will be late, early and
// occasionally twice in the same millisecond; none of that may duplicate or
// drop an advert, because a program above a radio that did would look
// unreliable for reasons that are not its own.
void test_polling_irregularly_neither_repeats_nor_loses() {
  const FakeBench b = one_box(-1.0);
  const double until = 20.0;

  // Every advert the whole window contains, as the reference.
  const std::vector<Advert> all = octo::adverts_between(b, -1.0, until, 0.0, kWall0);
  std::set<long long> want;
  for (const Advert& a : all) want.insert(llround(a.mono * 1e6));

  // The same window, walked in lumpy steps -- including a repeat of the same
  // instant, which must yield nothing the second time.
  const double steps[] = {0.1, 0.9, 0.9, 3.0, 0.05, 5.0, 5.0, 5.0, 0.05};
  std::set<long long> got;
  double since = -1.0, now = 0.0;
  int duplicates = 0;
  for (double step : steps) {
    now += step;
    if (now > until) now = until;
    for (const Advert& a : octo::adverts_between(b, since, now, 0.0, kWall0)) {
      if (!got.insert(llround(a.mono * 1e6)).second) ++duplicates;
    }
    since = now;
  }
  // Asking again with the clock unmoved: still nothing new.
  for (const Advert& a : octo::adverts_between(b, since, now, 0.0, kWall0)) {
    if (!got.insert(llround(a.mono * 1e6)).second) ++duplicates;
  }
  CHECK_EQ(duplicates, 0);
  CHECK_EQ((int)got.size(), (int)want.size());
  CHECK(got == want);
}

// A box that goes quiet has to actually stop, and one that comes back has to
// actually come back. This is the case that decides what the canonical time
// is, and the one nobody can arrange on demand with real hardware.
void test_a_box_can_go_quiet_and_return() {
  FakeBench b = one_box(-2.0);
  b.boxes[0].silent_after_s = 10.0;
  b.boxes[0].returns_after_s = 20.0;

  int before = 0, during = 0, after = 0;
  for (const Advert& a : octo::adverts_between(b, -1.0, 30.0, 0.0, kWall0)) {
    if (a.mono < 10.0) ++before;
    else if (a.mono < 20.0) ++during;
    else ++after;
  }
  CHECK(before > 0);
  CHECK_EQ(during, 0);
  CHECK(after > 0);
}

// Each payload type reaches the decoder as the kind it claims to be. A bench
// of one kind would leave two decoder branches unvisited, which is how a fake
// radio ends up proving less than it appears to.
void test_every_payload_kind_decodes_as_itself() {
  struct Case {
    FakeBox::Kind kind;
    bool ok;
    octo::Resolution resolution;
  } cases[] = {
      {FakeBox::Kind::kFrameMicros, true, octo::Resolution::kFrameMicros},
      {FakeBox::Kind::kFrame, true, octo::Resolution::kFrame},
      {FakeBox::Kind::kMicros, true, octo::Resolution::kMicrosecond},
      {FakeBox::Kind::kStatic, false, octo::Resolution::kNone},
  };
  for (const Case& c : cases) {
    const FakeBench b = one_box(-1.0, 0.0, c.kind);
    const std::vector<Advert> adverts =
        octo::adverts_between(b, -1.0, 1.0, 0.0, kWall0);
    CHECK(!adverts.empty());
    if (adverts.empty()) continue;
    const octo::Decoded d = octo::decode(adverts.front().data);
    CHECK_EQ(d.ok, c.ok);
    CHECK(d.resolution == c.resolution);
  }
}

// ------------------------------------------------------------------ the spec

void test_a_spec_builds_the_bench_it_describes() {
  FakeBench b;
  std::string err;
  CHECK(FakeBench::parse(
      "box,A,-1.5,24,frame+us,-20; box,B,-1.6,25,us; cam,cam-1,Studio,-0.25,30",
      &b, &err));
  CHECK_STR(err, "");
  CHECK_EQ((int)b.boxes.size(), 2);
  if (b.boxes.size() == 2) {
    CHECK_STR(b.boxes[0].name, "A");
    CHECK_NEAR(b.boxes[0].offset_s, -1.5, 1e-9);
    CHECK_EQ(b.boxes[0].fps, 24);
    CHECK_NEAR(b.boxes[0].drift_ppm, -20.0, 1e-9);
    CHECK(b.boxes[1].kind == FakeBox::Kind::kMicros);
    CHECK_EQ(b.boxes[1].fps, 25);
    // Two boxes with different names must not collide, or a bench would
    // silently be one box reported twice.
    CHECK(b.boxes[0].id != b.boxes[1].id);
  }
  CHECK(b.has_camera);
  CHECK_STR(b.camera.id, "cam-1");
  CHECK_NEAR(b.camera.error_s, -0.25, 1e-9);
  CHECK_EQ(b.camera.fps, 30);
}

// A typo must not quietly produce a different experiment from the one somebody
// meant to run -- which is the failure that would waste the most time here,
// because the output of a wrong bench looks exactly like the output of a right
// one.
void test_a_bad_spec_is_refused_with_a_reason() {
  const char* bad[] = {
      "box,A",                  // no offset
      "box,A,not-a-number",
      "box,A,-1,24,sideways",   // no such payload kind
      "cam,only-an-id",
      "wombat,A,-1",
  };
  for (const char* spec : bad) {
    FakeBench b;
    std::string err;
    CHECK(!FakeBench::parse(spec, &b, &err));
    CHECK(!err.empty());
  }
}

// Nothing said means the standard bench, not an empty one: `--radio fake` on
// its own has to do something worth looking at.
void test_an_empty_spec_is_the_standard_bench() {
  FakeBench b;
  std::string err;
  CHECK(FakeBench::parse("", &b, &err));
  CHECK_EQ((int)b.boxes.size(), 5);
  CHECK(b.has_camera);

  // It has to resemble the real bench, or it is useless for the thing it is
  // for: boxes that disagree by tens of milliseconds, not by seconds.
  double lo = 1e9, hi = -1e9;
  for (const FakeBox& box : b.boxes) {
    lo = std::fmin(lo, box.offset_s);
    hi = std::fmax(hi, box.offset_s);
    CHECK(box.drift_ppm != 0.0);
  }
  CHECK(hi - lo > 0.005);
  CHECK(hi - lo < 0.100);

  // And at least one of each interesting kind, so the default exercises more
  // than one decoder branch.
  bool micros = false;
  for (const FakeBox& box : b.boxes) {
    if (box.kind == FakeBox::Kind::kMicros) micros = true;
  }
  CHECK(micros);
}

}  // namespace

int main() {
  test_a_box_reports_the_offset_it_was_given();
  test_a_box_drifts();
  test_polling_irregularly_neither_repeats_nor_loses();
  test_a_box_can_go_quiet_and_return();
  test_every_payload_kind_decodes_as_itself();
  test_a_spec_builds_the_bench_it_describes();
  test_a_bad_spec_is_refused_with_a_reason();
  test_an_empty_spec_is_the_standard_bench();
  return octotest::report("test_fakebench");
}
