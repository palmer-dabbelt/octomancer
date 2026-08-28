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
  // The header is the only place the Mac is mentioned at all.
  CHECK(contains(text, "vs this Mac"));
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

}  // namespace

int main() {
  test_canonical_is_a_median_of_enabled_live_boxes();
  test_offsets_are_against_canonical_not_this_mac();
  test_no_live_boxes_means_no_offsets_at_all();
  test_a_held_camera_reads_as_held();
  test_a_quiet_camera_reads_as_off_the_air_with_an_age();
  test_camera_age_prefers_last_seen_over_the_snapshot();
  test_camera_error_is_shown_only_against_a_bench();
  test_camera_notes_follow_the_uis_precedence();
  test_a_disabled_camera_is_hidden();
  test_missing_octomancerd_borrows_the_other_bench();
  test_missing_octomancer_sync_still_lists_what_was_heard();
  test_both_daemons_missing_is_an_empty_view();
  test_colour_only_adds_escapes();
  test_verbose_adds_the_detail_and_stays_narrow();
  test_link_state_names();
  return octotest::report("devices");
}
