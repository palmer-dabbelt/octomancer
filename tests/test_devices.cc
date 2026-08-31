// Merging two daemons' idea of the room into one list.
//
// The thing worth testing here is the offset column, because it is the one
// number a person will act on. It has to be a device's distance from the
// canonical time and not from this Mac -- so the tests below put the whole
// bench half a second away from the host clock and then insist the rows still
// read as milliseconds apart. If that ever regresses, somebody re-jams a set
// of boxes that were in perfect agreement.
//
// The rest is about not lying: a disabled box votes on nothing and appears
// nowhere but the hidden count, a camera whose link is held reads as held
// rather than as missing, and a device's distance from a canonical time that
// does not exist is rendered as a dash and never as 0.000s.
#include <unistd.h>

#include <fstream>
#include <string>

#include "camconf.h"
#include "control.h"
#include "devices.h"
#include "harness.h"
#include "registry.h"

using octo::CamConf;
using octo::CameraSnapshot;
using octo::CameraStatus;
using octo::DeviceKind;
using octo::DeviceRow;
using octo::DeviceSnapshot;
using octo::DeviceSources;
using octo::DeviceView;
using octo::LinkState;
using octo::Snapshot;
using octo::Status;
using octo::WarnLevel;

namespace {

const double kNow = 1700000000.0;

DeviceSnapshot box(const std::string& id, const std::string& name, bool live,
                   double median) {
  DeviceSnapshot d;
  d.id = id;
  d.name = name;
  d.live = live;
  d.rssi = -55;
  d.has_time = true;
  d.sod = 43200.0;
  d.display = "12:00:00.000";
  d.offset = median;
  d.median_offset = median;
  d.samples = 40;
  d.age = live ? 1.0 : 120.0;
  return d;
}

CameraStatus camera(const std::string& id, const std::string& name) {
  CameraStatus c;
  c.id = id;
  c.name = name;
  c.writes_enabled = true;
  c.action = "write";
  return c;
}

DeviceView view_of(const Snapshot& snap, const CamConf* conf) {
  DeviceSources from;
  from.bench = &snap;
  from.conf = conf;
  from.now_wall = kNow;
  return octo::build_device_view(from);
}

const DeviceRow* find_row(const DeviceView& v, const std::string& name) {
  for (const DeviceRow& r : v.rows) {
    if (r.name == name) return &r;
  }
  return nullptr;
}

bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// Everything between an ESC and the terminating 'm', removed. What is left has
// to be exactly what the uncoloured renderer produced, which is the only way a
// test can pin the layout without pinning the escape sequences too.
std::string strip_escapes(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\033') {
      while (i < s.size() && s[i] != 'm') ++i;
      continue;
    }
    out += s[i];
  }
  return out;
}

// The one line of a rendering that mentions `name`, escapes and all.
std::string row_for(const std::string& out, const std::string& name) {
  const size_t at = out.find(name);
  if (at == std::string::npos) return std::string();
  const size_t start = out.rfind('\n', at);
  const size_t end = out.find('\n', at);
  return out.substr(start == std::string::npos ? 0 : start + 1,
                    end == std::string::npos ? std::string::npos
                                             : end - (start + 1));
}

// A row is written as a run of <escape><text><reset> fields, so splitting on
// the reset hands them back in column order: 0 name, 1 age, 2 offset, 3 link,
// 4 signal, and the verbose ones after that.
std::vector<std::string> columns_of(const std::string& row) {
  std::vector<std::string> out;
  size_t at = 0;
  while (true) {
    const size_t end = row.find("\033[0m", at);
    if (end == std::string::npos) break;
    out.push_back(row.substr(at, end - at));
    at = end + 4;
  }
  return out;
}

bool dimmed(const std::string& column) {
  return column.find("\033[2m") != std::string::npos;
}

std::string temp_path(const char* tag) {
  return "/tmp/octo-devices-" + std::to_string(getpid()) + "-" + tag + ".conf";
}

// A configuration file with exactly the lines a test cares about. Written out
// rather than faked because CamConf is the thing that decides what "enabled"
// means, and a test that reimplements that decision is testing itself.
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

CamConf plain_conf() { return conf_with("plain", ""); }

// ------------------------------------------------------------ the canonical

void test_canonical_is_a_median_of_enabled_live_boxes() {
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, 10.0));
  snap.device.push_back(box("B", "Tentacle_B", true, 12.0));
  snap.device.push_back(box("C", "Tentacle_C", false, 100.0));
  snap.device.push_back(box("D", "Tentacle_D", true, -50.0));

  CamConf conf = conf_with("disabled-box", "box D enabled=off\n");
  DeviceSources from;
  from.bench = &snap;
  from.conf = &conf;
  from.now_wall = kNow;

  const DeviceView v = octo::build_device_view(from);
  CHECK(v.has_canonical);
  // The stale box and the disabled one are both out: 11.0, not 11.5 and not
  // -20. The disabled box is the interesting exclusion, because it is live and
  // would otherwise drag the bench fifty seconds.
  CHECK_NEAR(v.canonical_offset_s, 11.0, 1e-9);
  CHECK_NEAR(v.canonical_spread_s, 2.0, 1e-9);
  CHECK_EQ(v.contributing, 2);
  CHECK_STR(v.canonical_source, "octomancerd");

  // Rows are only the enabled devices, and the one left out is counted rather
  // than quietly dropped.
  CHECK_EQ(static_cast<int>(v.rows.size()), 3);
  CHECK_EQ(v.hidden, 1);
  CHECK(find_row(v, "Tentacle_D") == nullptr);

  const DeviceRow* a = find_row(v, "Tentacle_A");
  const DeviceRow* b = find_row(v, "Tentacle_B");
  CHECK(a != nullptr && b != nullptr);
  CHECK(a->contributes);
  CHECK(b->contributes);
  CHECK_NEAR(a->offset_s, -1.0, 1e-9);
  CHECK_NEAR(b->offset_s, 1.0, 1e-9);
  // Live boxes sort ahead of the stale one.
  CHECK_STR(v.rows[2].name, "Tentacle_C");
  CHECK(!v.rows[2].contributes);
}

// The point of the whole module: this Mac's clock cancels out. Both boxes are
// half a minute from the host and ten milliseconds apart, and it is the ten
// milliseconds that get rendered.
// The rig as it actually stands: four boxes on the bench and a fifth that
// wandered off. The fifth is still listed -- somebody wants to know it is
// missing -- but nothing it said an hour ago is allowed anywhere near the
// arithmetic, and the column that would quote it is left blank.
//
// The trap this pins down is subtle enough to have been read as a bug in the
// spread. A box quiet for two hours free-runs against this Mac the whole
// time, so `median_offset - canonical` grows steadily whatever the box was
// doing when it left. Printing that in the same column as the live boxes puts
// a forty-millisecond number on a page whose spread is eight, and the only
// available conclusion is that the spread must be wrong.
void test_a_silent_box_is_listed_but_left_out_of_the_arithmetic() {
  Snapshot snap;
  snap.device.push_back(box("bmpcc", "BMPCC", true, -1.733));
  snap.device.push_back(box("f55", "F55", true, -1.741));
  snap.device.push_back(box("fs5", "FS5", true, -1.736));
  snap.device.push_back(box("krysta", "Krysta", true, -1.739));
  DeviceSnapshot gone = box("fs7", "FS7", false, -1.778);
  gone.age = 6600.0;
  snap.device.push_back(gone);

  const DeviceView v = view_of(snap, nullptr);

  // Four votes, and the median and spread of exactly those four. Were the
  // fifth in, the spread would be 45ms rather than 8.
  CHECK_EQ(v.contributing, 4);
  CHECK_NEAR(v.canonical_offset_s, -1.7375, 1e-9);
  CHECK_NEAR(v.canonical_spread_s, 0.008, 1e-9);
  CHECK_EQ(v.silent, 1);
  CHECK_EQ(v.hidden, 0);  // not the same thing, and not counted as one

  // Listed, and honest about why there is no number.
  const DeviceRow* row = find_row(v, "FS7");
  CHECK(row != nullptr);
  CHECK(row->link == LinkState::kOffTheAir);
  CHECK(!row->has_offset);
  CHECK(row->offset_is_stale);
  CHECK(!row->contributes);
  // The raw reading survives, because it is quoted against this Mac and so
  // does not drift out from under itself the way the difference does.
  CHECK(row->has_median);
  CHECK_NEAR(row->median_offset_s, -1.778, 1e-9);

  // The number that started all this, and it is nowhere on the page either
  // way. The arithmetic that explains it is --verbose only: somebody reading
  // the table does not need the axis restated on every run.
  const std::string text = strip_escapes(octo::render_devices(v, false, false));
  CHECK(!contains(text, "40.5ms"));
  CHECK(!contains(text, "canonical time"));
  CHECK(!contains(text, "off the air:"));

  const std::string loud = strip_escapes(octo::render_devices(v, true, false));
  CHECK(!contains(loud, "40.5ms"));
  CHECK(contains(loud, "across 4 timecode boxes on the air"));
  CHECK(contains(loud, "1 timecode box off the air"));
}

// Without --verbose there is the table and nothing above it. Pinned because
// the header is assembled conditionally now, and the failure mode of building
// a string that might be empty and then appending a separator to it is a file
// that opens with a blank line nobody put there.
// Colour carries meaning in this table, so which colour lands where is worth
// pinning rather than eyeballing once.
//
// Two rules. The headings are the one row on the page that is always true, so
// they are not drawn in the ink that means "do not trust this number" -- they
// were dim, which said the opposite of what they are. And a row nobody is
// hearing is dim all the way across, because every figure on it is a memory:
// the age is how long ago, the signal is how loud it was then, the timecode is
// what it said at the time. Half a row dim would read as a bug in the table;
// none of it dim reads as a device that is fine.
void test_colour_says_which_numbers_are_memories() {
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, 0.000));
  DeviceSnapshot gone = box("B", "Tentacle_B", false, 0.010);
  gone.age = 3600.0;
  snap.device.push_back(gone);
  const DeviceView v = view_of(snap, nullptr);

  for (int verbose = 0; verbose < 2; ++verbose) {
    const std::string out = octo::render_devices(v, verbose != 0, true);

    // Cyan, and specifically not the dim used for a stale figure.
    const size_t head = out.find("DEVICE");
    CHECK(head >= 5);
    if (head >= 5) CHECK(out.compare(head - 5, 5, "\033[36m") == 0);

    const std::vector<std::string> heard = columns_of(row_for(out, "Tentacle_A"));
    const std::vector<std::string> quiet = columns_of(row_for(out, "Tentacle_B"));
    CHECK(heard.size() >= 5);
    CHECK(quiet.size() == heard.size());
    if (heard.size() < 5 || quiet.size() < 5) continue;

    // The two the eye goes to first, named because they are what this is for.
    CHECK(!dimmed(heard[1]));  // age
    CHECK(!dimmed(heard[4]));  // signal
    CHECK(dimmed(quiet[1]));
    CHECK(dimmed(quiet[4]));

    // And the rest of the row with them, every column of it.
    for (size_t i = 0; i < quiet.size(); ++i) CHECK(dimmed(quiet[i]));
  }
}

void test_the_brief_view_is_the_table_and_nothing_else() {
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, 0.000));
  snap.device.push_back(box("B", "Tentacle_B", true, 0.002));

  const DeviceView v = view_of(snap, nullptr);
  const std::string text = strip_escapes(octo::render_devices(v, false, false));
  CHECK(text.compare(0, 6, "DEVICE") == 0);
  CHECK(!contains(text, "canonical"));
  CHECK(!contains(text, "vs this Mac"));

  // Signal is in the brief view, not behind the flag. "Why is this one not
  // being heard" is asked too often for the answer to need asking for.
  CHECK(contains(text, "RSSI"));
  CHECK(contains(text, "-55"));

  // And with it, the header is back and the table is still under it.
  const std::string loud = strip_escapes(octo::render_devices(v, true, false));
  CHECK(loud.compare(0, 6, "DEVICE") != 0);
  CHECK(contains(loud, "canonical time"));
  CHECK(contains(loud, "DEVICE"));

  // The brief table is the first columns of the verbose one, character for
  // character. Pinned because the two are one format string with a suffix, and
  // the moment they stop being that they become two tables to learn.
  const std::string brief_head = text.substr(0, text.find('\n'));
  const size_t at = loud.find("DEVICE");
  const std::string loud_head = loud.substr(at, loud.find('\n', at) - at);
  CHECK(brief_head.size() < loud_head.size());
  CHECK(loud_head.compare(0, brief_head.size(), brief_head) == 0);
}

void test_offsets_are_against_canonical_not_this_mac() {
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, 1000.000));
  snap.device.push_back(box("B", "Tentacle_B", true, 1000.020));

  DeviceSources from;
  from.bench = &snap;
  from.now_wall = kNow;
  const DeviceView v = octo::build_device_view(from);

  CHECK_NEAR(v.canonical_offset_s, 1000.010, 1e-9);
  CHECK_NEAR(find_row(v, "Tentacle_A")->offset_s, -0.010, 1e-9);
  CHECK_NEAR(find_row(v, "Tentacle_B")->offset_s, 0.010, 1e-9);
  // And the verbose view still has the raw figure, which is where the Mac's
  // own error shows up.
  CHECK_NEAR(find_row(v, "Tentacle_A")->median_offset_s, 1000.000, 1e-9);

  const std::string text = octo::render_devices(v, false, false);
  CHECK(contains(text, "-10.0ms"));
  CHECK(contains(text, "+10.0ms"));
  // This Mac is not mentioned at all in the brief view, and with --verbose
  // there is exactly one place it appears: the header.
  CHECK(!contains(text, "vs this Mac"));
  CHECK(contains(octo::render_devices(v, true, false), "vs this Mac"));
}

void test_no_live_boxes_means_no_offsets_at_all() {
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", false, 3.0));

  DeviceSources from;
  from.bench = &snap;
  from.now_wall = kNow;
  const DeviceView v = octo::build_device_view(from);

  CHECK(!v.has_canonical);
  CHECK_EQ(v.contributing, 0);
  CHECK_STR(v.canonical_source, "nothing");
  CHECK_EQ(static_cast<int>(v.rows.size()), 1);
  CHECK(!v.rows[0].has_offset);

  const std::string text = octo::render_devices(v, false, false);
  CHECK(contains(text, "--"));
  // Not a zero, and not the box's distance from the Mac dressed up as one.
  CHECK(!contains(text, "+0.0ms"));
  CHECK(!contains(text, "+3.000s"));
  CHECK(contains(text, "no canonical time"));
}

// ---------------------------------------------------------------- cameras

// A held camera stops advertising, so "not present" is what success looks
// like. Rendering it as off the air would send somebody to power-cycle a
// camera that is being written to at that moment.
void test_a_held_camera_reads_as_held() {
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, 0.5));

  Status status;
  CameraStatus c = camera("cam-1", "A:1EAE18A7");
  c.connected = true;
  c.present = false;
  c.has_last_seen = true;
  c.last_seen_wall = kNow - 45.0;  // stale, and deliberately ignored
  status.cameras.push_back(c);

  DeviceSources from;
  from.bench = &snap;
  from.cameras = &status;
  from.now_wall = kNow;
  const DeviceView v = octo::build_device_view(from);

  const DeviceRow* row = find_row(v, "A:1EAE18A7");
  CHECK(row != nullptr);
  CHECK(row->kind == DeviceKind::kCamera);
  CHECK(row->link == LinkState::kHeld);
  // Held means we are hearing it continuously, whatever the last
  // advertisement says.
  CHECK(row->has_age);
  CHECK_NEAR(row->age_s, 0.0, 1e-9);

  const std::string text = octo::render_devices(v, false, false);
  CHECK(contains(text, "held"));
  CHECK(!contains(text, "off the air"));
}

void test_a_quiet_camera_reads_as_off_the_air_with_an_age() {
  Status status;
  CameraStatus c = camera("cam-1", "A:1EAE18A7");
  c.has_last_seen = true;
  c.last_seen_wall = kNow - 30.0;
  status.cameras.push_back(c);
  status.bench.has = true;
  status.bench.boxes = 2;
  status.bench.offset_s = 0.25;

  DeviceSources from;
  from.cameras = &status;
  from.now_wall = kNow;
  const DeviceView v = octo::build_device_view(from);

  const DeviceRow* row = find_row(v, "A:1EAE18A7");
  CHECK(row != nullptr);
  CHECK(row->link == LinkState::kOffTheAir);
  CHECK(row->has_age);
  CHECK_NEAR(row->age_s, 30.0, 1e-6);
  CHECK_STR(octo::link_state_name(row->link), "off the air");
  CHECK(contains(octo::render_devices(v, false, false), "off the air"));
}

// The daemon's own timestamp wins, because it is absolute: an age computed by
// the daemon is already stale by the time it is drawn.
void test_camera_age_prefers_last_seen_over_the_snapshot() {
  Snapshot snap;
  snap.camera.reported = true;
  snap.camera.seen = true;
  snap.camera.present = true;
  snap.camera.id = "cam-1";
  snap.camera.name = "A:1EAE18A7";
  snap.camera.age = 40.0;

  Status status;
  CameraStatus c = camera("cam-1", "A:1EAE18A7");
  c.present = true;
  c.has_last_seen = true;
  c.last_seen_wall = kNow - 5.0;
  status.cameras.push_back(c);

  DeviceSources from;
  from.bench = &snap;
  from.cameras = &status;
  from.now_wall = kNow;
  DeviceView v = octo::build_device_view(from);
  CHECK_EQ(static_cast<int>(v.rows.size()), 1);
  CHECK_NEAR(v.rows[0].age_s, 5.0, 1e-6);

  // ...and with nothing to prefer, the snapshot's age is better than a blank.
  status.cameras[0].has_last_seen = false;
  v = octo::build_device_view(from);
  CHECK(v.rows[0].has_age);
  CHECK_NEAR(v.rows[0].age_s, 40.0, 1e-9);
}

void test_camera_error_is_shown_only_against_a_bench() {
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, 0.5));

  Status status;
  CameraStatus c = camera("cam-1", "A:1EAE18A7");
  c.present = true;
  c.has_error = true;
  c.error_s = 0.008;  // already camera-minus-bench; no arithmetic to do
  status.cameras.push_back(c);

  DeviceSources from;
  from.bench = &snap;
  from.cameras = &status;
  from.now_wall = kNow;
  DeviceView v = octo::build_device_view(from);
  CHECK(find_row(v, "A:1EAE18A7")->has_offset);
  CHECK_NEAR(find_row(v, "A:1EAE18A7")->offset_s, 0.008, 1e-9);

  // No canonical time, so no offset: an error measured against a bench nobody
  // can name is not a number worth printing.
  snap.device[0].live = false;
  v = octo::build_device_view(from);
  CHECK(!v.has_canonical);
  CHECK(!find_row(v, "A:1EAE18A7")->has_offset);
}

void test_camera_notes_follow_the_uis_precedence() {
  Status status;
  CameraStatus c = camera("cam-1", "A:1EAE18A7");
  c.action = "close enough";
  status.cameras.push_back(c);

  DeviceSources from;
  from.cameras = &status;
  from.now_wall = kNow;
  CHECK_STR(octo::build_device_view(from).rows[0].note, "close enough");

  status.cameras[0].has_source = true;
  status.cameras[0].source = 1;  // clip timecode
  CHECK_STR(octo::build_device_view(from).rows[0].note,
            "timecode does not follow the clock");

  // Recording beats everything: it is the one state in which nothing will be
  // written no matter what else is true.
  status.cameras[0].recording = true;
  CHECK_STR(octo::build_device_view(from).rows[0].note, "recording");
}

// A camera in the file that neither daemon has heard.
//
// Both camera sources are records of something being heard, so a camera
// somebody had named and asked to be warned about used to vanish from the
// list entirely the moment it stopped advertising -- and looked exactly like
// a camera that had never existed. Nothing said it was missing.
void test_a_configured_camera_is_listed_even_when_never_heard() {
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, -3.5));

  Status status;  // octomancer-sync is answering and has heard no camera

  CamConf conf = conf_with(
      "never-heard", "camera cam-1 writes=on name=A:1EAE18A7 warn=on\n");

  DeviceSources from;
  from.bench = &snap;
  from.cameras = &status;
  from.conf = &conf;
  from.now_wall = kNow;
  const DeviceView v = octo::build_device_view(from);

  const DeviceRow* row = find_row(v, "A:1EAE18A7");
  CHECK(row != nullptr);
  CHECK(row->kind == DeviceKind::kCamera);
  CHECK(row->link == LinkState::kOffTheAir);
  // No age and no offset. There is no instant to count from, and an age of
  // zero would render as "now", which is the opposite of true.
  CHECK(!row->has_age);
  CHECK(!row->has_offset);

  // And it is yellow, which is the whole point: the menu-bar blip had nothing
  // to colour before, so a camera that was switched off left the icon grey.
  CHECK(row->warn);
  CHECK(row->warn_level == WarnLevel::kUnsure);
  CHECK(v.worst_warning == WarnLevel::kUnsure);

  const std::string text = octo::render_devices(v, false, false);
  CHECK(contains(text, "A:1EAE18A7"));
  CHECK(contains(text, "off the air"));
}

// Only octomancer-sync goes looking for cameras. With it not answering,
// "off the air" would be a claim about a radio nobody was driving.
void test_a_configured_camera_is_unknown_when_sync_is_not_answering() {
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, -3.5));

  CamConf conf = conf_with(
      "no-sync", "camera cam-1 writes=on name=A:1EAE18A7 warn=on\n");

  DeviceSources from;
  from.bench = &snap;
  from.cameras = nullptr;
  from.conf = &conf;
  from.now_wall = kNow;
  const DeviceView v = octo::build_device_view(from);

  const DeviceRow* row = find_row(v, "A:1EAE18A7");
  CHECK(row != nullptr);
  CHECK(row->link == LinkState::kUnknown);
}

// A camera that is switched off in the file stays switched off. Being in the
// file is not on its own a reason to appear -- otherwise disabling one would
// be no relief from being told about it.
void test_a_configured_camera_that_is_disabled_is_still_hidden() {
  Status status;
  CamConf conf = conf_with(
      "off-not-heard", "camera cam-1 writes=off name=A:1EAE18A7 warn=on\n");

  DeviceSources from;
  from.cameras = &status;
  from.conf = &conf;
  from.now_wall = kNow;
  const DeviceView v = octo::build_device_view(from);

  CHECK(find_row(v, "A:1EAE18A7") == nullptr);
  CHECK_EQ(v.hidden, 1);
  CHECK(v.worst_warning == WarnLevel::kNone);
}

// ...and a camera that IS being heard gets one row, not two.
void test_a_configured_camera_that_is_heard_is_not_duplicated() {
  Status status;
  CameraStatus c = camera("cam-1", "A:1EAE18A7");
  c.present = true;
  c.has_last_seen = true;
  c.last_seen_wall = kNow - 2.0;
  status.cameras.push_back(c);

  CamConf conf = conf_with(
      "heard-once", "camera cam-1 writes=on name=A:1EAE18A7 warn=on\n");

  DeviceSources from;
  from.cameras = &status;
  from.conf = &conf;
  from.now_wall = kNow;
  const DeviceView v = octo::build_device_view(from);

  int cameras = 0;
  for (const DeviceRow& r : v.rows) {
    if (r.kind == DeviceKind::kCamera) ++cameras;
  }
  CHECK_EQ(cameras, 1);
  const DeviceRow* row = find_row(v, "A:1EAE18A7");
  CHECK(row != nullptr);
  CHECK(row->link == LinkState::kOnTheAir);
  CHECK(row->has_age);
}

void test_a_disabled_camera_is_hidden() {
  Status status;
  status.cameras.push_back(camera("cam-1", "A:1EAE18A7"));
  status.cameras.push_back(camera("cam-2", "B:2FBF29B8"));

  // Cameras default to off, so naming only the first enables only the first.
  CamConf conf = conf_with("one-camera", "camera cam-1 writes=on\n");
  DeviceSources from;
  from.cameras = &status;
  from.conf = &conf;
  from.now_wall = kNow;

  const DeviceView v = octo::build_device_view(from);
  CHECK_EQ(static_cast<int>(v.rows.size()), 1);
  CHECK_STR(v.rows[0].name, "A:1EAE18A7");
  CHECK_EQ(v.hidden, 1);
}

// ------------------------------------------------------------ absent daemons

void test_missing_octomancerd_borrows_the_other_bench() {
  Status status;
  status.bench.has = true;
  status.bench.source = "tentacle";
  status.bench.boxes = 3;
  status.bench.offset_s = 0.250;
  status.bench.spread_s = 0.004;
  CameraStatus c = camera("cam-1", "A:1EAE18A7");
  c.present = true;
  c.has_error = true;
  c.error_s = -0.002;
  status.cameras.push_back(c);

  DeviceSources from;
  from.cameras = &status;
  from.now_wall = kNow;
  const DeviceView v = octo::build_device_view(from);

  CHECK(v.has_canonical);
  CHECK_STR(v.canonical_source, "octomancer-sync");
  CHECK_EQ(v.contributing, 3);
  CHECK_NEAR(v.canonical_offset_s, 0.250, 1e-9);
  CHECK_EQ(static_cast<int>(v.rows.size()), 1);
  CHECK(v.rows[0].kind == DeviceKind::kCamera);
  CHECK_NEAR(v.rows[0].offset_s, -0.002, 1e-9);
  CHECK(contains(octo::render_devices(v, true, false), "octomancer-sync"));
}

void test_missing_octomancer_sync_still_lists_what_was_heard() {
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, 0.5));
  snap.camera.reported = true;
  snap.camera.seen = true;
  snap.camera.present = true;
  snap.camera.id = "cam-1";
  snap.camera.name = "A:1EAE18A7";
  snap.camera.age = 2.0;

  DeviceSources from;
  from.bench = &snap;
  from.now_wall = kNow;
  DeviceView v = octo::build_device_view(from);

  CHECK_EQ(static_cast<int>(v.rows.size()), 2);
  CHECK(v.rows[0].kind == DeviceKind::kTentacle);
  CHECK(v.rows[1].kind == DeviceKind::kCamera);
  CHECK(v.rows[1].link == LinkState::kOnTheAir);
  CHECK_NEAR(v.rows[1].age_s, 2.0, 1e-9);
  // Nothing knows what the camera's clock says without octomancer-sync.
  CHECK(!v.rows[1].has_offset);

  // Silence, with the daemon that might be holding the link unreachable: we
  // do not know which of the two it is, and saying "off the air" would be a
  // guess dressed as a measurement.
  snap.camera.present = false;
  v = octo::build_device_view(from);
  CHECK(find_row(v, "A:1EAE18A7")->link == LinkState::kUnknown);
  CHECK(contains(octo::render_devices(v, false, false), "unknown"));
}

void test_both_daemons_missing_is_an_empty_view() {
  DeviceSources from;
  from.now_wall = kNow;
  const DeviceView v = octo::build_device_view(from);

  CHECK(v.rows.empty());
  CHECK(!v.has_canonical);
  CHECK_EQ(v.hidden, 0);
  CHECK_STR(v.canonical_source, "nothing");
  const std::string text = octo::render_devices(v, true, false);
  CHECK(contains(text, "no devices"));
}

// An empty list explains itself when the radio can explain it.
//
// This is the 2026-08-30 outage written down. A bench of five timecode boxes
// read "no devices" and nothing else for a morning, while the snapshot the
// renderer was holding said `radio: unknown` the whole time. The bug was two
// bugs -- a dongle wrongly auto-selected, then a Bluetooth grant lost to a
// rebuild -- and the output was byte-identical for both, and for an empty
// room. That is the property being fixed: an empty table is the moment the
// question gets asked, so it is where the answer has to be.
void test_an_empty_list_says_why_the_radio_is_not_helping() {
  struct Case {
    const char* radio;
    const char* expect;  // "" means: say nothing beyond "no devices"
  } cases[] = {
      // The state that actually occurred, and the one worth the most words:
      // no callback ever arrived, which is what a refused permission looks
      // like on macOS. Nothing else in the system distinguishes it.
      {"unknown", "Bluetooth"},
      {"unauthorized", "Privacy & Security"},
      {"poweredOff", "switched off"},
      {"unsupported", "no Bluetooth Low Energy"},
      {"resetting", "resetting"},
      // A working radio and an empty room is not a fault, and saying anything
      // here would be noise on every quiet bench.
      {"poweredOn", ""},
      // No daemon answered. That already has its own line further up, and
      // repeating it as a radio complaint would be a second explanation for
      // one fact.
      {"", ""},
  };
  for (const Case& c : cases) {
    Snapshot snap;
    snap.radio = c.radio;
    DeviceSources from;
    // A bench with a radio state and no devices in it -- which is exactly the
    // shape octomancerd serves when it cannot hear anything.
    if (*c.radio != '\0') from.bench = &snap;
    from.now_wall = kNow;
    const DeviceView v = octo::build_device_view(from);
    CHECK(v.rows.empty());
    CHECK_STR(v.radio, c.radio);

    const std::string text = strip_escapes(octo::render_devices(v, false, false));
    CHECK(contains(text, "no devices"));
    if (*c.expect != '\0') {
      CHECK(contains(text, c.expect));
    } else {
      // Nothing beyond the two words. Checked by length rather than by
      // guessing at phrases the renderer might use.
      CHECK(text.find("Bluetooth") == std::string::npos);
      CHECK(text.find("radio") == std::string::npos);
    }
  }
}

// -------------------------------------------------------------- rendering

DeviceView busy_view() {
  Snapshot snap;
  Status status;
  snap.device.push_back(box("A", "Tentacle_A", true, 0.500));
  snap.device.push_back(box("B", "Tentacle_B", true, 0.520));
  snap.device.back().has_drift = true;
  snap.device.back().drift_ppm = -1.5;
  snap.device.back().resolution = "1080p24";
  snap.device.push_back(box("C", "Tentacle_C", false, 9.0));
  snap.device.back().alerting = true;

  CameraStatus c = camera("cam-1", "A:1EAE18A7");
  c.present = true;
  c.has_last_seen = true;
  c.last_seen_wall = kNow - 3.0;
  c.has_rssi = true;
  c.rssi = -61;
  c.timecode = "12:00:00:04";
  c.has_fps = true;
  c.fps = 24;
  c.has_error = true;
  c.error_s = 0.012;
  c.recording = true;
  status.cameras.push_back(c);

  DeviceSources from;
  from.bench = &snap;
  from.cameras = &status;
  from.now_wall = kNow;
  return octo::build_device_view(from);
}

void test_colour_only_adds_escapes() {
  const DeviceView v = busy_view();
  for (int verbose = 0; verbose < 2; ++verbose) {
    const std::string plain = octo::render_devices(v, verbose != 0, false);
    const std::string fancy = octo::render_devices(v, verbose != 0, true);
    CHECK(plain.find('\033') == std::string::npos);
    CHECK(fancy.find('\033') != std::string::npos);
    // Byte-identical minus the escapes, which means every escape sits outside
    // a field width rather than being padded along with the text.
    CHECK_STR(strip_escapes(fancy), plain);
    CHECK(contains(plain, "Tentacle_A"));
    CHECK(contains(plain, "A:1EAE18A7"));
  }
}

void test_verbose_adds_the_detail_and_stays_narrow() {
  const DeviceView v = busy_view();
  const std::string brief = octo::render_devices(v, false, false);
  const std::string full = octo::render_devices(v, true, false);

  CHECK(contains(brief, "OFFSET"));
  CHECK(!contains(brief, "DRIFT"));
  CHECK(contains(full, "RSSI"));
  CHECK(contains(full, "TIMECODE"));
  CHECK(contains(full, "MEDIAN"));
  CHECK(contains(full, "DRIFT"));
  CHECK(contains(full, "12:00:00:04"));
  CHECK(contains(full, "-1.5ppm"));
  CHECK(contains(full, "24fps"));
  CHECK(contains(full, "canonical source: octomancerd"));
  // Notes, which are sentences and so live under the table rather than in it.
  CHECK(contains(full, "recording"));
  CHECK(contains(full, "not jammed"));

  size_t start = 0;
  while (start <= full.size()) {
    const size_t nl = full.find('\n', start);
    const size_t end = nl == std::string::npos ? full.size() : nl;
    CHECK(end - start <= 110);
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
}

void test_link_state_names() {
  CHECK_STR(octo::link_state_name(LinkState::kUnknown), "unknown");
  CHECK_STR(octo::link_state_name(LinkState::kHeld), "held");
  CHECK_STR(octo::link_state_name(LinkState::kOnTheAir), "on the air");
  CHECK_STR(octo::link_state_name(LinkState::kOffTheAir), "off the air");
  CHECK(octo::link_is_live(LinkState::kHeld));
  CHECK(octo::link_is_live(LinkState::kOnTheAir));
  CHECK(!octo::link_is_live(LinkState::kOffTheAir));
  CHECK(!octo::link_is_live(LinkState::kUnknown));
}

// -------------------------------------------------------------- warnings
//
// A warning is a thing somebody asked for, one device at a time, and the
// tests below are mostly about the two ways of not answering: never warning
// about kit nobody claimed, and never dressing an old reading up as a current
// one. Red is a measurement we do not like. Yellow is the absence of one.

// Nobody asked, so nothing is said -- however wrong the device is. This is
// the setting's whole reason for existing: an indicator that lights up about
// every box in range is one people stop reading.
void test_a_device_nobody_asked_about_never_warns() {
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, 0.000));
  snap.device.push_back(box("B", "Tentacle_B", true, 0.000));
  snap.device.push_back(box("C", "Tentacle_C", true, 20.000));

  // No configuration at all: we do not know what anybody cares about.
  DeviceView v = view_of(snap, nullptr);
  CHECK_NEAR(find_row(v, "Tentacle_C")->offset_s, 20.0, 1e-9);
  CHECK(!find_row(v, "Tentacle_C")->warn);
  CHECK(find_row(v, "Tentacle_C")->warn_level == WarnLevel::kNone);
  CHECK(v.worst_warning == WarnLevel::kNone);
  CHECK_EQ(v.warned_out_of_sync, 0);
  CHECK_EQ(v.warned_unsure, 0);

  // A configuration that mentions it without asking for a warning is the
  // same answer.
  CamConf conf = conf_with("warn-silent", "box C enabled=on\n");
  v = view_of(snap, &conf);
  CHECK(!find_row(v, "Tentacle_C")->warn);
  CHECK(v.worst_warning == WarnLevel::kNone);
  CHECK_EQ(v.warned_out_of_sync, 0);
}

void test_a_warned_device_goes_red_only_when_it_is_out() {
  CamConf conf = conf_with("warn-c", "box C warn=on\n");

  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, 0.000));
  snap.device.push_back(box("B", "Tentacle_B", true, 0.000));
  // Fifty milliseconds: further out than a jammed bench, and well inside what
  // an hour of drift does to a camera. Not worth a red light.
  snap.device.push_back(box("C", "Tentacle_C", true, 0.050));

  DeviceView v = view_of(snap, &conf);
  CHECK(find_row(v, "Tentacle_C")->warn);
  CHECK(find_row(v, "Tentacle_C")->warn_level == WarnLevel::kNone);
  CHECK(v.worst_warning == WarnLevel::kNone);
  CHECK_EQ(v.warned_out_of_sync, 0);
  CHECK_EQ(v.warned_unsure, 0);

  // A hundred and fifty is past the threshold, and past two frames at 24.
  snap.device[2] = box("C", "Tentacle_C", true, 0.150);
  v = view_of(snap, &conf);
  CHECK(find_row(v, "Tentacle_C")->warn_level == WarnLevel::kOutOfSync);
  CHECK(v.worst_warning == WarnLevel::kOutOfSync);
  CHECK_EQ(v.warned_out_of_sync, 1);
  CHECK_EQ(v.warned_unsure, 0);
  // The devices around it were not asked about and stay quiet.
  CHECK(find_row(v, "Tentacle_A")->warn_level == WarnLevel::kNone);
}

// The point of having a yellow at all. This box was dead on the bench the
// last time anybody heard from it, an hour ago, and that is not evidence of
// anything now.
void test_a_stale_reading_is_yellow_however_good_it_looked() {
  CamConf conf = conf_with("warn-stale", "box C warn=on\n");

  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, 0.000));
  snap.device.push_back(box("B", "Tentacle_B", true, 0.000));
  DeviceSnapshot c = box("C", "Tentacle_C", false, 0.000);
  c.age = 3600.0;
  snap.device.push_back(c);

  DeviceView v = view_of(snap, &conf);
  const DeviceRow* row = find_row(v, "Tentacle_C");
  // Withheld rather than shown and disbelieved. It was a perfect number an
  // hour ago and it is worthless now, and the column has no way to say so.
  CHECK(!row->has_offset);
  CHECK(row->offset_is_stale);
  CHECK(row->warn_level == WarnLevel::kUnsure);
  CHECK(v.worst_warning == WarnLevel::kUnsure);
  CHECK_EQ(v.warned_unsure, 1);
  CHECK_EQ(v.warned_out_of_sync, 0);

  // Quiet for a couple of minutes is just duty cycling, and says nothing.
  snap.device[2].age = 200.0;
  v = view_of(snap, &conf);
  CHECK(find_row(v, "Tentacle_C")->warn_level == WarnLevel::kNone);
  CHECK_EQ(v.warned_unsure, 0);
}

// A held camera has stopped advertising because we are talking to it, so its
// last advertisement can be as old as it likes. Ageing it out would put a
// yellow light on the one device we are most certain about.
void test_a_held_camera_is_never_unsure_on_age() {
  CamConf conf = conf_with("warn-held", "camera cam-1 writes=on warn=on\n");

  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, 0.000));

  Status status;
  CameraStatus c = camera("cam-1", "A:1EAE18A7");
  c.connected = true;
  c.present = false;
  c.has_last_seen = true;
  c.last_seen_wall = kNow - 7200.0;  // two hours, and irrelevant
  c.has_error = true;
  c.error_s = 0.004;
  status.cameras.push_back(c);

  DeviceSources from;
  from.bench = &snap;
  from.cameras = &status;
  from.conf = &conf;
  from.now_wall = kNow;
  const DeviceView v = octo::build_device_view(from);

  const DeviceRow* row = find_row(v, "A:1EAE18A7");
  CHECK(row != nullptr);
  CHECK(row->link == LinkState::kHeld);
  CHECK(row->warn);
  CHECK(row->warn_level == WarnLevel::kNone);
  CHECK(v.worst_warning == WarnLevel::kNone);
}

// Heard from a moment ago and still no opinion: there is no canonical time to
// measure it against, so we cannot say it is fine, and saying nothing would
// be saying exactly that.
void test_a_warned_device_with_nothing_to_measure_against_is_yellow() {
  CamConf conf = conf_with("warn-nocanon", "box A warn=on\n");

  Snapshot snap;
  DeviceSnapshot a = box("A", "Tentacle_A", true, 0.000);
  a.has_time = false;  // being heard, but it has not said what time it is
  snap.device.push_back(a);

  const DeviceView v = view_of(snap, &conf);
  CHECK(!v.has_canonical);
  const DeviceRow* row = find_row(v, "Tentacle_A");
  CHECK(!row->has_offset);
  CHECK(row->warn_level == WarnLevel::kUnsure);
  CHECK_EQ(v.warned_unsure, 1);
}

// Switching a device off is somebody saying they are not working with it
// today. It must buy silence, or the only way left to stop the light would be
// to stop looking at it.
void test_a_disabled_device_raises_nothing() {
  CamConf conf = conf_with("warn-disabled", "box B enabled=off warn=on\n");

  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, 0.000));
  snap.device.push_back(box("B", "Tentacle_B", true, 20.000));

  const DeviceView v = view_of(snap, &conf);
  CHECK_EQ(static_cast<int>(v.rows.size()), 1);
  CHECK_EQ(v.hidden, 1);
  CHECK(find_row(v, "Tentacle_B") == nullptr);
  CHECK(v.worst_warning == WarnLevel::kNone);
  CHECK_EQ(v.warned_out_of_sync, 0);
  CHECK_EQ(v.warned_unsure, 0);
}

void test_worst_warning_is_the_loudest_of_them() {
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, 0.000));
  snap.device.push_back(box("B", "Tentacle_B", true, 0.000));
  DeviceSnapshot c = box("C", "Tentacle_C", false, 0.000);
  c.age = 3600.0;
  snap.device.push_back(c);
  snap.device.push_back(box("D", "Tentacle_D", true, 5.000));

  CamConf both = conf_with("warn-mix", "box C warn=on\nbox D warn=on\n");
  DeviceView v = view_of(snap, &both);
  CHECK(find_row(v, "Tentacle_C")->warn_level == WarnLevel::kUnsure);
  CHECK(find_row(v, "Tentacle_D")->warn_level == WarnLevel::kOutOfSync);
  CHECK_EQ(v.warned_unsure, 1);
  CHECK_EQ(v.warned_out_of_sync, 1);
  // Red beats yellow: a device we know is wrong outranks one we cannot say
  // anything about.
  CHECK(v.worst_warning == WarnLevel::kOutOfSync);

  // With only the quiet one asked about, yellow is the worst there is --
  // even though the wildly wrong box is still sitting there unasked-about.
  CamConf quiet = conf_with("warn-quiet", "box C warn=on\n");
  v = view_of(snap, &quiet);
  CHECK(v.worst_warning == WarnLevel::kUnsure);
  CHECK_EQ(v.warned_unsure, 1);
  CHECK_EQ(v.warned_out_of_sync, 0);
}

void test_warn_level_names() {
  CHECK_STR(octo::warn_level_name(WarnLevel::kNone), "none");
  CHECK_STR(octo::warn_level_name(WarnLevel::kUnsure), "unsure");
  CHECK_STR(octo::warn_level_name(WarnLevel::kOutOfSync), "out of sync");
}

// The terminal has to see what the menu bar sees, and it has to see it with
// colour switched off -- a marker that is only a colour is invisible in a log
// file, and to anybody who reads red and green the same way.
void test_the_table_marks_and_names_the_warned() {
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, 0.000));
  snap.device.push_back(box("B", "Tentacle_B", true, 0.000));
  DeviceSnapshot c = box("C", "Tentacle_C", false, 0.000);
  c.age = 3600.0;
  snap.device.push_back(c);
  snap.device.push_back(box("D", "Tentacle_D", true, 5.000));

  CamConf conf = conf_with("warn-render", "box C warn=on\nbox D warn=on\n");
  const DeviceView v = view_of(snap, &conf);

  for (int verbose = 0; verbose < 2; ++verbose) {
    const std::string plain = octo::render_devices(v, verbose != 0, false);
    const std::string fancy = octo::render_devices(v, verbose != 0, true);
    CHECK(plain.find('\033') == std::string::npos);
    // The markers are characters, not colours, so they survive the pipe.
    CHECK(contains(plain, "Tentacle_D !"));
    CHECK(contains(plain, "Tentacle_C ?"));
    // ...and the line under the table says which is which, by name.
    CHECK(contains(plain, "out of sync with the bench: Tentacle_D"));
    // Red always explains itself: an alarm that does not say what it is about
    // is not much of one. Yellow is named only when detail was asked for --
    // the row already carries the marker and the age that produced it, and a
    // second telling on every run is what stops the first from being read.
    CHECK(contains(plain, "not heard from recently enough to say: Tentacle_C") ==
          (verbose != 0));
    // The two that nobody asked about are not marked or named.
    CHECK(contains(plain, "Tentacle_A "));
    CHECK(!contains(plain, "Tentacle_A !"));
    CHECK(!contains(plain, "Tentacle_A ?"));
    // Colour adds red and yellow and changes nothing else.
    CHECK(contains(fancy, "\033[31m"));
    CHECK(contains(fancy, "\033[33m"));
    CHECK_STR(strip_escapes(fancy), plain);
  }
}

// ------------------------------------------------------- the second radio

// A dongle hears the same room from a different place, and its rows arrive in
// the same snapshot tagged with which radio heard them.
//
// The property that matters is that its rows read the same as ours even
// though its clock does not agree with ours -- and, in the normal standalone
// case, has never been set at all. Both radios say "this box is fourteen
// milliseconds ahead of its bench", because the arbitrary constant in a
// free-running clock is the same for every box that radio hears and therefore
// cancels when a row is quoted against its own radio's median.
//
// This is the whole reason for listening twice. If the two copies of a box
// disagree, that is a fact about the room; if they disagreed merely because
// one radio had a different idea of what time it was, the second radio would
// be telling us nothing at all.
DeviceSnapshot heard_by(const std::string& radio, const DeviceSnapshot& src,
                        double displaced) {
  DeviceSnapshot d = src;
  d.radio = radio;
  d.id = radio + ":" + src.id;
  d.median_offset += displaced;
  d.offset += displaced;
  return d;
}

Snapshot two_radios(double displaced) {
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, -0.500));
  snap.device.push_back(box("B", "Tentacle_B", true, -0.486));
  snap.device.push_back(box("C", "Tentacle_C", true, -0.514));
  const size_t local = snap.device.size();
  for (size_t i = 0; i < local; ++i) {
    snap.device.push_back(heard_by("dongle", snap.device[i], displaced));
  }
  return snap;
}

void test_a_box_heard_twice_reads_the_same_from_both_radios() {
  CamConf conf = plain_conf();
  // Eleven hours: a dongle that was plugged in this morning and never told
  // the time. Nothing about this number is special, which is the point.
  const DeviceView v = view_of(two_radios(39600.0), &conf);

  const DeviceRow* ours = find_row(v, "Tentacle_B");
  const DeviceRow* theirs = nullptr;
  for (const DeviceRow& r : v.rows) {
    if (r.name == "Tentacle_B" && r.radio == "dongle") theirs = &r;
  }
  CHECK(ours != nullptr);
  CHECK(theirs != nullptr);
  CHECK_STR(ours->radio, "");
  CHECK(ours->has_offset);
  CHECK(theirs->has_offset);
  // Tentacle_B is 14 ms ahead of the median, from either end of the room.
  CHECK_NEAR(ours->offset_s, 0.014, 1e-9);
  CHECK_NEAR(theirs->offset_s, 0.014, 1e-9);
}

// The header is about this machine, so a dongle's boxes must not be counted
// in it -- and its eleven-hour displacement must not reach the canonical time.
void test_the_header_is_about_this_machines_radio_only() {
  CamConf conf = plain_conf();
  const DeviceView v = view_of(two_radios(39600.0), &conf);

  CHECK(v.has_canonical);
  CHECK_EQ(v.contributing, 3);
  CHECK_NEAR(v.canonical_offset_s, -0.500, 1e-9);
  CHECK_NEAR(v.canonical_spread_s, 0.028, 1e-9);
  // Six rows: every box twice, because nothing can prove they are the same box.
  CHECK_EQ(static_cast<int>(v.rows.size()), 6);
}

void test_the_other_radio_is_listed_with_its_own_bench() {
  CamConf conf = plain_conf();
  const DeviceView v = view_of(two_radios(39600.0), &conf);

  CHECK_EQ(static_cast<int>(v.radios.size()), 1);
  CHECK_STR(v.radios[0].name, "dongle");
  CHECK(v.radios[0].has_canonical);
  CHECK_EQ(v.radios[0].contributing, 3);
  // Its own median, displacement and all -- which is the honest figure for a
  // radio that does not know what time it is, and is why the rows are quoted
  // against it rather than it being shown as an offset.
  CHECK_NEAR(v.radios[0].canonical_offset_s, 39600.0 - 0.500, 1e-9);
  // ...but it agrees with us exactly about how far apart the boxes are.
  CHECK_NEAR(v.radios[0].canonical_spread_s, v.canonical_spread_s, 1e-9);
}

// A dongle that has only just been plugged in has heard one box, or none. Its
// rows then have nothing to be quoted against, and the column stays empty
// rather than borrowing ours -- ours is in a different frame, and subtracting
// it would render eleven hours as a sync error.
void test_a_radio_with_no_bench_of_its_own_quotes_no_offsets() {
  CamConf conf = plain_conf();
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, -0.500));
  snap.device.push_back(box("B", "Tentacle_B", true, -0.486));
  // One box, heard by the dongle, with no time yet.
  DeviceSnapshot d = heard_by("dongle", snap.device[0], 39600.0);
  d.has_time = false;
  snap.device.push_back(d);

  const DeviceView v = view_of(snap, &conf);
  CHECK(v.has_canonical);
  CHECK_EQ(static_cast<int>(v.radios.size()), 1);
  CHECK(!v.radios[0].has_canonical);

  const DeviceRow* theirs = nullptr;
  for (const DeviceRow& r : v.rows) {
    if (r.radio == "dongle") theirs = &r;
  }
  CHECK(theirs != nullptr);
  CHECK(!theirs->has_offset);
  // Not "stale" either: it never said a time, which is a different thing from
  // having said one we are declining to quote.
  CHECK(!theirs->offset_is_stale);
}

// The table only grows a column when there is something to put in it. Almost
// nobody has a dongle plugged in, and a column of blanks on every row for the
// sake of the people who do is a worse table for everybody.
void test_the_table_is_unchanged_without_a_second_radio() {
  CamConf conf = plain_conf();
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, -0.500));
  snap.device.push_back(box("B", "Tentacle_B", true, -0.486));
  const std::string out =
      octo::render_devices(view_of(snap, &conf), false, false);

  CHECK(!contains(out, "VIA"));
  CHECK(!contains(out, "also hearing"));
  CHECK(contains(out, "DEVICE"));
}

void test_the_table_says_which_radio_heard_each_row() {
  CamConf conf = plain_conf();
  const DeviceView v = view_of(two_radios(39600.0), &conf);
  const std::string out = octo::render_devices(v, false, false);

  CHECK(contains(out, "VIA"));
  // Said out loud, because a bench that appears to have doubled overnight is
  // something a person would otherwise go and investigate.
  CHECK(contains(out, "also hearing via dongle"));
  CHECK(contains(out, "listed twice"));

  // Our own rows carry no label -- the column marks what came from elsewhere,
  // and writing "this Mac" on the majority would bury that under repetition.
  const std::string ours = row_for(out, "Tentacle_A ");
  CHECK(!contains(ours, "dongle"));
  // ...and the dongle's rows do.
  CHECK(contains(out, "dongle"));
}

// A box the dongle has never been told the name of is listed by its hardware
// address, and every box from one manufacturer shares the first three bytes.
// Cutting the column from the right would render four different boxes as four
// identical rows, which does not look like a truncation -- it looks like the
// table repeating itself.
void test_unnamed_boxes_keep_the_end_of_their_address() {
  CamConf conf = plain_conf();
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, -0.500));
  for (int i = 1; i <= 3; ++i) {
    DeviceSnapshot d = box("C4:1E:AE:18:A7:0" + std::to_string(i), "", true,
                           39599.5 + 0.001 * i);
    d.radio = "dongle";
    snap.device.push_back(d);
  }
  const std::string out =
      octo::render_devices(view_of(snap, &conf), false, false);

  CHECK(contains(out, "A7:01"));
  CHECK(contains(out, "A7:02"));
  CHECK(contains(out, "A7:03"));
  // Not three copies of the same prefix and nothing else.
  CHECK(!contains(out, "C4:1E:AE:18:A7 "));
}

void test_a_radio_that_has_heard_nothing_still_says_it_is_there() {
  CamConf conf = plain_conf();
  Snapshot snap;
  snap.device.push_back(box("A", "Tentacle_A", true, -0.500));
  DeviceSnapshot d = heard_by("dongle", snap.device[0], 0.0);
  d.has_time = false;
  snap.device.push_back(d);

  const std::string out =
      octo::render_devices(view_of(snap, &conf), false, false);
  CHECK(contains(out, "also listening via dongle"));
  CHECK(contains(out, "has not heard a timecode box yet"));
}

void test_the_second_radio_survives_colour_unchanged() {
  CamConf conf = plain_conf();
  const DeviceView v = view_of(two_radios(39600.0), &conf);
  CHECK_STR(strip_escapes(octo::render_devices(v, false, true)),
            octo::render_devices(v, false, false));
  CHECK_STR(strip_escapes(octo::render_devices(v, true, true)),
            octo::render_devices(v, true, false));
}

}  // namespace

int main() {
  test_canonical_is_a_median_of_enabled_live_boxes();
  test_a_silent_box_is_listed_but_left_out_of_the_arithmetic();
  test_the_brief_view_is_the_table_and_nothing_else();
  test_colour_says_which_numbers_are_memories();
  test_offsets_are_against_canonical_not_this_mac();
  test_no_live_boxes_means_no_offsets_at_all();
  test_a_held_camera_reads_as_held();
  test_a_quiet_camera_reads_as_off_the_air_with_an_age();
  test_camera_age_prefers_last_seen_over_the_snapshot();
  test_camera_error_is_shown_only_against_a_bench();
  test_camera_notes_follow_the_uis_precedence();
  test_a_configured_camera_is_listed_even_when_never_heard();
  test_a_configured_camera_is_unknown_when_sync_is_not_answering();
  test_a_configured_camera_that_is_disabled_is_still_hidden();
  test_a_configured_camera_that_is_heard_is_not_duplicated();
  test_a_disabled_camera_is_hidden();
  test_missing_octomancerd_borrows_the_other_bench();
  test_missing_octomancer_sync_still_lists_what_was_heard();
  test_both_daemons_missing_is_an_empty_view();
  test_an_empty_list_says_why_the_radio_is_not_helping();
  test_colour_only_adds_escapes();
  test_verbose_adds_the_detail_and_stays_narrow();
  test_link_state_names();
  test_a_device_nobody_asked_about_never_warns();
  test_a_warned_device_goes_red_only_when_it_is_out();
  test_a_stale_reading_is_yellow_however_good_it_looked();
  test_a_held_camera_is_never_unsure_on_age();
  test_a_warned_device_with_nothing_to_measure_against_is_yellow();
  test_a_disabled_device_raises_nothing();
  test_worst_warning_is_the_loudest_of_them();
  test_warn_level_names();
  test_the_table_marks_and_names_the_warned();
  test_a_box_heard_twice_reads_the_same_from_both_radios();
  test_the_header_is_about_this_machines_radio_only();
  test_the_other_radio_is_listed_with_its_own_bench();
  test_a_radio_with_no_bench_of_its_own_quotes_no_offsets();
  test_the_table_is_unchanged_without_a_second_radio();
  test_the_table_says_which_radio_heard_each_row();
  test_unnamed_boxes_keep_the_end_of_their_address();
  test_a_radio_that_has_heard_nothing_still_says_it_is_there();
  test_the_second_radio_survives_colour_unchanged();
  return octotest::report("devices");
}
