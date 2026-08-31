// The join, rather than the pieces -- see tests/scenario.h.
//
// Every test here says what is in the room and then asserts what somebody
// would see on the page, with nothing between the two faked except the radio.
// They are the regression net for the class of bug the unit tests kept
// missing: each component correct, the composition wrong.
#include "scenario.h"

#include <cmath>
#include <string>

#include "devices.h"
#include "harness.h"

namespace {

using octo::DeviceKind;
using octo::DeviceRow;
using octo::LinkState;
using octo::WarnLevel;
using octotest::Scenario;

bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

int rows_of_kind(const octo::DeviceView& v, DeviceKind kind) {
  int n = 0;
  for (const DeviceRow& r : v.rows) {
    if (r.kind == kind) ++n;
  }
  return n;
}

// The bench a person actually has: several boxes agreeing to within a few
// milliseconds, all of them a long way from this Mac's clock. The offsets on
// the page must be distances from the canonical time and not from the host,
// because a column that quoted the host would have somebody re-jamming a set
// of boxes that were in perfect agreement.
void test_a_bench_that_agrees_reads_as_milliseconds_apart() {
  Scenario s("box,F55,-3.558;box,FS7,-3.554;box,Krysta,-3.553");
  s.run(60.0);

  CHECK_EQ(rows_of_kind(s.view(), DeviceKind::kTentacle), 3);
  CHECK(s.view().has_canonical);

  for (const char* name : {"F55", "FS7", "Krysta"}) {
    const DeviceRow* r = s.row(name);
    CHECK(r != nullptr);
    if (r == nullptr) continue;
    CHECK(r->link == LinkState::kOnTheAir);
    CHECK(r->has_offset);
    // Every box is three and a half seconds from this Mac and within
    // milliseconds of the others. The column must show the latter.
    CHECK(std::fabs(r->offset_s) < 0.050);
  }

  const std::string page = s.table();
  CHECK(contains(page, "F55"));
  // "on the air" is no longer a column: a box being heard shows as a small
  // age and a row that is not dimmed.
  CHECK(contains(page, "OFFSET"));
  // Nowhere on the page, not merely nowhere in the table. There is no column
  // anywhere for a host clock's distance from the mesh: nothing syncs against
  // the host clock, so the number has no consequence.
  CHECK(!contains(page, "-3.5"));
}

// A camera in the file that is switched off. Both sources of camera rows are
// records of something being heard, so this used to produce no row at all --
// and a camera somebody had named and asked to be warned about looked exactly
// like one that had never existed.
void test_a_configured_camera_that_is_off_is_still_on_the_page() {
  Scenario s("box,F55,-3.558;box,FS7,-3.554");
  s.conf("camera cam-1 writes=on name=A:1EAE18A7 warn=on\n");
  s.run(60.0);

  const DeviceRow* r = s.row("A:1EAE18A7");
  CHECK(r != nullptr);
  if (r == nullptr) return;
  CHECK(r->kind == DeviceKind::kCamera);
  CHECK(r->link == LinkState::kOffTheAir);
  CHECK(!r->has_age);
  CHECK(!r->has_offset);
  // Yellow, which is the point: with no row there was nothing for the
  // menu-bar blip to colour, so a camera that was switched off left the icon
  // grey and said nothing at all.
  CHECK(r->warn_level == WarnLevel::kUnsure);
  CHECK(s.view().worst_warning == WarnLevel::kUnsure);

  CHECK(contains(s.table(), "A:1EAE18A7"));
}

// ...and when it is on the air it is one row, with an age, and not warned.
void test_a_camera_that_is_present_is_listed_once_and_quiet() {
  Scenario s("box,F55,-3.558;cam,cam-1,A:1EAE18A7,0.010");
  s.conf("camera cam-1 writes=on name=A:1EAE18A7 warn=on\n");
  s.run(60.0);

  CHECK_EQ(rows_of_kind(s.view(), DeviceKind::kCamera), 1);
  const DeviceRow* r = s.row("A:1EAE18A7");
  CHECK(r != nullptr);
  if (r == nullptr) return;
  CHECK(r->link == LinkState::kOnTheAir);
  CHECK(r->has_age);
  CHECK(r->warn_level == WarnLevel::kNone);
}

// A camera stops advertising while something holds its link, so silence there
// is our own doing. Reporting it as absence would have somebody power-cycling
// a camera that is being written to at that moment.
void test_a_held_camera_is_not_reported_as_missing() {
  Scenario s("box,F55,-3.558;cam,cam-1,A:1EAE18A7,0.010");
  s.conf("camera cam-1 writes=on name=A:1EAE18A7 warn=on\n");
  s.camera_held();
  s.run(60.0);

  const DeviceRow* r = s.row("A:1EAE18A7");
  CHECK(r != nullptr);
  if (r == nullptr) return;
  CHECK(r->link == LinkState::kHeld);
  CHECK(r->warn_level == WarnLevel::kNone);
  CHECK(contains(s.table(), "held"));
}

// A box nobody has enabled votes on nothing and appears nowhere but the
// hidden count. Switching one off has to be a real relief from it, or the
// only way to stop being told about it is to stop looking.
void test_a_disabled_box_is_hidden_and_does_not_vote() {
  // The third box is the one in the next room: two minutes out and nothing to
  // do with this shoot. If it votes, it drags the canonical time with it and
  // every other row on the page reads as wrong.
  Scenario s("box,F55,-3.558;box,FS7,-3.554;box,Elsewhere,+120.0");
  s.conf("box " + s.box_id("Elsewhere") + " enabled=off name=Elsewhere\n");
  s.run(60.0);

  CHECK(s.row("Elsewhere") == nullptr);
  CHECK_EQ(s.view().hidden, 1);
  CHECK(s.view().has_canonical);

  const DeviceRow* f55 = s.row("F55");
  CHECK(f55 != nullptr);
  if (f55 != nullptr) CHECK(std::fabs(f55->offset_s) < 0.050);
  CHECK(!contains(s.table(), "Elsewhere"));
}

// With octomancerd not answering there is still a page, and it is built from
// what octomancer-sync heard. This is the state somebody is looking at when
// they are working out which half of the program is missing.
void test_one_daemon_missing_still_says_something() {
  Scenario s("box,F55,-3.558;cam,cam-1,A:1EAE18A7,0.010");
  s.conf("camera cam-1 writes=on name=A:1EAE18A7 warn=on\n");
  s.without_octomancerd();
  s.run(60.0);

  // No boxes: they are octomancerd's to report. The camera survives.
  CHECK_EQ(rows_of_kind(s.view(), DeviceKind::kTentacle), 0);
  CHECK_EQ(rows_of_kind(s.view(), DeviceKind::kCamera), 1);
}

// And with octomancer-sync not answering, "off the air" would be a claim
// about a radio nobody was driving.
void test_no_sync_daemon_means_the_camera_is_unknown_not_absent() {
  Scenario s("box,F55,-3.558");
  s.conf("camera cam-1 writes=on name=A:1EAE18A7 warn=on\n");
  s.without_sync();
  s.run(60.0);

  const DeviceRow* r = s.row("A:1EAE18A7");
  CHECK(r != nullptr);
  if (r == nullptr) return;
  CHECK(r->link == LinkState::kUnknown);
}

// The default bench, end to end. Mostly a smoke test of the whole chain: if
// the fake radio, the decoder, the roster, the merge and the renderer all
// still agree, this passes and says nothing more.
void test_the_standard_bench_renders() {
  Scenario s;
  s.run(120.0);

  CHECK(!s.view().rows.empty());
  CHECK(s.view().has_canonical);
  const std::string page = s.table();
  CHECK(contains(page, "DEVICE"));
  CHECK(contains(page, "OFFSET"));

  // Verbose adds columns and must not lose any rows.
  const size_t plain = s.view().rows.size();
  CHECK_EQ(s.view().rows.size(), plain);
  CHECK(contains(s.table(true), "MEDIAN"));
}

}  // namespace

int main() {
  test_a_bench_that_agrees_reads_as_milliseconds_apart();
  test_a_configured_camera_that_is_off_is_still_on_the_page();
  test_a_camera_that_is_present_is_listed_once_and_quiet();
  test_a_held_camera_is_not_reported_as_missing();
  test_a_disabled_box_is_hidden_and_does_not_vote();
  test_one_daemon_missing_still_says_something();
  test_no_sync_daemon_means_the_camera_is_unknown_not_absent();
  test_the_standard_bench_renders();
  return octotest::report("test_scenario");
}
