// Which boxes are the bench, and what happens when they stop answering.
//
// The numbers below are a real bench, copied out of octomancerd on the evening
// of 2026-08-30: five Tentacle boxes, all about 3.59 seconds from this Mac's
// clock and within 25 milliseconds of each other, with one of them -- the F55,
// out at the far end of the room -- heard four times in ten minutes and off
// the air the rest. Using the real figures rather than round ones keeps the
// tests honest about the scale involved: the offsets are seconds, the
// disagreement between boxes is milliseconds, and a test written with 1.0 and
// 2.0 would not notice a routine that lost the distinction.
#include "../src/bench.h"

#include <unistd.h>

#include <fstream>
#include <string>

#include "../src/camconf.h"
#include "../src/registry.h"
#include "harness.h"

using octo::Bench;
using octo::CamConf;
using octo::DeviceSnapshot;
using octo::Snapshot;

namespace {

DeviceSnapshot box(const std::string& id, const std::string& name, bool live,
                   double median) {
  DeviceSnapshot d;
  d.id = id;
  d.name = name;
  d.live = live;
  d.rssi = -55;
  d.has_time = true;
  d.median_offset = median;
  d.offset = median;
  d.samples = 40;
  d.resolution = "frame+us";
  d.age = live ? 1.0 : 99.4;
  return d;
}

std::string temp_path(const char* tag) {
  return "/tmp/octo-bench-" + std::to_string(getpid()) + "-" + tag + ".conf";
}

CamConf conf_with(const char* tag, const std::string& body) {
  const std::string path = temp_path(tag);
  {
    std::ofstream out(path, std::ios::trunc);
    out << body;
  }
  CamConf conf;
  std::string err;
  CHECK(conf.load(path, &err));
  ::unlink(path.c_str());
  return conf;
}

CamConf plain() { return conf_with("plain", ""); }

// The room as it stood, F55 included and off the air.
Snapshot the_bench() {
  Snapshot snap;
  snap.device.push_back(box("B80D95C9", "BMPCC", true, -3.575476));
  snap.device.push_back(box("E7EEBE32", "FS5", true, -3.585987));
  snap.device.push_back(box("42723B20", "FS7", true, -3.600590));
  snap.device.push_back(box("F1139A0E", "Krysta", true, -3.594125));
  snap.device.push_back(box("97A75BDD", "F55", false, -3.599526));
  return snap;
}

bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// ------------------------------------------------------------------ the tests

void test_the_bench_is_the_boxes_being_heard() {
  const CamConf conf = plain();
  const Bench b = octo::bench_from(the_bench(), conf, "octomancerd");

  CHECK(b.ok);
  CHECK_EQ(b.boxes, 4);
  CHECK_EQ(b.skipped, 0);
  CHECK_STR(b.source, "octomancerd");
  // Median of the four live medians: the mean of the middle pair, which is
  // FS5 and Krysta.
  CHECK_NEAR(b.offset, (-3.585987 + -3.594125) / 2.0, 1e-9);
  // ...and the spread is the worst disagreement, BMPCC against FS7. Twenty-five
  // milliseconds, on offsets of three and a half seconds: this is the
  // subtraction that has to survive, because it is the one a person acts on.
  CHECK_NEAR(b.spread, -3.575476 - -3.600590, 1e-9);
}

// The F55 is far away and hears the mesh perfectly well; what is missing is its
// broadcasts, not its synchronisation. It is still tempting to count it, and
// the reason not to is that what would be counted is a remembered offset --
// evidence about several minutes ago dressed up as evidence about now.
void test_a_box_off_the_air_does_not_vote() {
  const CamConf conf = plain();
  Snapshot snap = the_bench();
  const Bench with = octo::bench_from(snap, conf, "octomancerd");

  // Bring it back on the air and it votes, without anything else changing.
  snap.device[4].live = true;
  snap.device[4].age = 1.0;
  const Bench back = octo::bench_from(snap, conf, "octomancerd");

  CHECK_EQ(with.boxes, 4);
  CHECK_EQ(back.boxes, 5);
  // It was never outside the others, so hearing it changes the count and the
  // median without widening the disagreement. That is the shape of a box
  // rejoining a bench it never actually left.
  CHECK_NEAR(back.spread, with.spread, 1e-9);
  CHECK(!contains(with.boxes_json, "F55"));
  CHECK(contains(back.boxes_json, "F55"));
}

// Heard, but never decoded: a box that has advertised without a readable clock
// in it has no offset to contribute, and contributing its default would put a
// zero into the median.
void test_a_box_with_no_reading_does_not_vote() {
  const CamConf conf = plain();
  Snapshot snap = the_bench();
  snap.device.push_back(box("DEADBEEF", "Mute", true, 0.0));
  snap.device.back().has_time = false;
  snap.device.back().samples = 0;

  const Bench b = octo::bench_from(snap, conf, "octomancerd");
  CHECK_EQ(b.boxes, 4);
  CHECK_EQ(b.skipped, 0);  // not a person's decision, so not counted as one
  CHECK(!contains(b.boxes_json, "Mute"));
}

// Switching a box off is a person's decision and is reported as one, because
// "there are no boxes to sync to" and "you switched the only box off" want
// different things doing about them.
void test_a_switched_off_box_is_skipped_and_counted() {
  const CamConf conf = conf_with("off", "box B80D95C9 enabled=off\n");
  const Bench b = octo::bench_from(the_bench(), conf, "octomancerd");

  CHECK_EQ(b.boxes, 3);
  CHECK_EQ(b.skipped, 1);
  // BMPCC was the high end of the spread, so dismissing it narrows the bench.
  CHECK_NEAR(b.spread, -3.585987 - -3.600590, 1e-9);
  CHECK(!contains(b.boxes_json, "BMPCC"));
  CHECK(contains(b.boxes_json, "Krysta"));
}

// The bug this file was written for.
//
// A bench of nothing reports an offset of zero, and zero is also what a bench
// sitting exactly on this Mac's clock reports. Only `ok` tells them apart, so
// anything that publishes this figure has to publish `ok` with it -- and
// anything that *keeps* the last good figure instead of this one is showing a
// time that nothing is measuring any more. octomancer-sync did exactly that:
// it only ever republished the bench at the end of a camera cycle, so with no
// camera in the room the window kept showing "1 timecode box, spread 0ms" --
// true at the instant the daemon started, and hours stale by the time anybody
// read it, while the device list beside it showed four boxes.
void test_an_empty_room_is_not_an_offset_of_zero() {
  const CamConf conf = plain();
  Snapshot snap;
  const Bench b = octo::bench_from(snap, conf, "octomancerd");

  CHECK(!b.ok);
  CHECK_EQ(b.boxes, 0);
  CHECK_EQ(b.spread, 0.0);
  // Still says where it looked. A caller reporting "no boxes" wants to be able
  // to say whether that was the daemon's answer or its own.
  CHECK_STR(b.source, "octomancerd");
  CHECK_STR(b.boxes_json, "{}");
}

// One box is a bench, and its spread is genuinely zero -- there is nothing for
// it to disagree with. Worth pinning because it is indistinguishable, in the
// numbers alone, from the stale reading above.
void test_one_box_is_a_bench_with_no_spread() {
  const CamConf conf = plain();
  Snapshot snap;
  snap.device.push_back(box("F1139A0E", "Krysta", true, -3.594125));
  const Bench b = octo::bench_from(snap, conf, "octomancerd");

  CHECK(b.ok);
  CHECK_EQ(b.boxes, 1);
  CHECK_NEAR(b.offset, -3.594125, 1e-9);
  CHECK_EQ(b.spread, 0.0);
}

// The record of a cycle has to be the arithmetic that actually happened, so
// the voters are named with the figures they voted with.
void test_the_record_names_the_voters_and_their_readings() {
  const CamConf conf = plain();
  const Bench b = octo::bench_from(the_bench(), conf, "octomancerd");

  CHECK(contains(b.boxes_json, "\"BMPCC\":{\"offset_s\":-3.5755"));
  CHECK(contains(b.boxes_json, "\"adverts\":40"));
  CHECK(contains(b.boxes_json, "\"resolution\":\"frame+us\""));
}

// A box that has never been named is still a box, and the record has to have
// something to key it by.
void test_an_unnamed_box_is_recorded_by_its_id() {
  const CamConf conf = plain();
  Snapshot snap;
  snap.device.push_back(box("F1139A0E", "", true, -3.594125));
  const Bench b = octo::bench_from(snap, conf, "octomancerd");
  CHECK(contains(b.boxes_json, "\"F1139A0E\""));
}

}  // namespace

int main() {
  test_the_bench_is_the_boxes_being_heard();
  test_a_box_off_the_air_does_not_vote();
  test_a_box_with_no_reading_does_not_vote();
  test_a_switched_off_box_is_skipped_and_counted();
  test_an_empty_room_is_not_an_offset_of_zero();
  test_one_box_is_a_bench_with_no_spread();
  test_the_record_names_the_voters_and_their_readings();
  test_an_unnamed_box_is_recorded_by_its_id();
  return octotest::report("test_bench");
}
