// The protocol has to survive names that arrive from the air. A box called
// "Cam 1 = A" is entirely legal and would walk straight through a naive
// tokenizer.
#include "../src/proto.h"
#include "harness.h"

#include <string>

using namespace octo;

namespace {

void test_escaping_round_trip() {
  const char* cases[] = {
      "plain", "with space", "eq=sign", "percent%20already", "tab\there",
      "newline\nhere", "", "\xff\xfe binary", "Krysta", "Cam 1 = A",
  };
  for (const char* c : cases) {
    const std::string in(c);
    const std::string esc = escape(in);
    // Escaped form must be a single safe token, or the line parser splits it.
    for (char ch : esc) {
      CHECK(ch > 0x20 && ch < 0x7f);
      CHECK(ch != '=');
    }
    CHECK_STR(unescape(esc), in);
  }
}

Snapshot sample_snapshot() {
  Snapshot s;
  s.wall = 1756100000.25;
  s.uptime = 1234.5;
  s.radio = "poweredOn";
  s.adverts_total = 591;
  s.undecodable_total = 2;
  s.clock_steps = 1;
  s.devices = 2;
  s.live = 1;
  s.alerting = 1;
  s.alert_threshold = 60.0;
  s.has_bench = true;
  s.bench_offset = -6.2314;
  s.bench_spread = 0.0027;

  DeviceSnapshot a;
  a.id = "09EE26AF-D630-DB5A";
  a.name = "Cam 1 = A";       // hostile on purpose
  a.rssi = -40;
  a.adverts = 120;
  a.decoded = 118;
  a.age = 1.25;
  a.live = true;
  a.has_time = true;
  a.sod = 76783.187689;
  a.offset = -6.231400;
  a.median_offset = -6.231200;
  a.samples = 118;
  a.display = "21:19:43:04.021";
  a.resolution = "frame+us";
  a.fps = 24;
  a.has_drift = true;
  a.drift_ppm = -12.34;
  a.drift_span = 3600.0;
  a.alerting = true;
  a.alert_since_wall = 1756099000.0;
  s.device.push_back(a);

  DeviceSnapshot b;
  b.id = "B80D95C9";
  b.name = "Krysta";
  b.rssi = -53;
  b.adverts = 8;
  b.live = false;
  b.has_time = false;
  b.note = "static/info packet";
  b.resolution = "none";
  s.device.push_back(b);
  return s;
}

void test_text_round_trip() {
  const Snapshot in = sample_snapshot();
  Snapshot out;
  std::string err;
  CHECK(parse_text(render_text(in), &out, &err));
  if (!err.empty()) octotest::fail(__FILE__, __LINE__, err);

  CHECK_NEAR(out.wall, in.wall, 1e-3);
  CHECK_STR(out.radio, in.radio);
  CHECK_EQ(static_cast<long long>(out.adverts_total), 591LL);
  CHECK_EQ(out.live, 1);
  CHECK_EQ(out.alerting, 1);
  CHECK(out.has_bench);
  CHECK_NEAR(out.bench_offset, -6.2314, 1e-6);
  CHECK_EQ(static_cast<long long>(out.device.size()), 2LL);

  const DeviceSnapshot& a = out.device[0];
  CHECK_STR(a.name, "Cam 1 = A");
  CHECK_STR(a.display, "21:19:43:04.021");
  CHECK_NEAR(a.offset, -6.2314, 1e-6);
  CHECK_NEAR(a.median_offset, -6.2312, 1e-6);
  CHECK(a.has_drift);
  CHECK_NEAR(a.drift_ppm, -12.34, 1e-3);
  CHECK(a.alerting);
  CHECK(a.live);

  const DeviceSnapshot& b = out.device[1];
  CHECK_STR(b.name, "Krysta");
  CHECK(!b.has_time);
  CHECK(!b.live);
  CHECK_STR(b.note, "static/info packet");
}

// How a second radio is reached travels once, beside the rows rather than on
// them: it is a fact about the link, and repeating it on every row would be
// both noise and an invitation to disagree with itself.
void test_radio_links_round_trip() {
  Snapshot in = sample_snapshot();
  RadioLink usb;
  usb.name = "dongle";
  usb.way = "usb";
  usb.answering = true;
  usb.age = 1.25;
  usb.clock_is_real = false;
  in.radio_link.push_back(usb);
  RadioLink air;
  air.name = "spare";
  air.way = "bluetooth";
  air.answering = false;
  in.radio_link.push_back(air);

  Snapshot out;
  std::string err;
  CHECK(parse_text(render_text(in), &out, &err));
  CHECK_EQ(static_cast<long long>(out.radio_link.size()), 2LL);
  CHECK_STR(out.radio_link[0].name, "dongle");
  CHECK_STR(out.radio_link[0].way, "usb");
  CHECK(out.radio_link[0].answering);
  CHECK_NEAR(out.radio_link[0].age, 1.25, 1e-2);
  // The one that must survive intact: a dongle whose clock is its own boot
  // counter, so that nothing downstream quotes it as a time of day.
  CHECK(!out.radio_link[0].clock_is_real);
  CHECK_STR(out.radio_link[1].way, "bluetooth");
  CHECK(!out.radio_link[1].answering);

  const std::string json = render_json(in);
  CHECK(json.find("\"radio_link\"") != std::string::npos);
  CHECK(json.find("\"way\":\"usb\"") != std::string::npos);
  CHECK(json.find("\"clock_real\":false") != std::string::npos);
}

// A snapshot from a daemon with no dongle says so by saying nothing, and a
// reader must not invent a radio out of that.
void test_no_radio_line_means_no_second_radio() {
  Snapshot out;
  std::string err;
  CHECK(parse_text(render_text(sample_snapshot()), &out, &err));
  CHECK(out.radio_link.empty());
}

void test_rejects_junk() {
  Snapshot out;
  std::string err;
  CHECK(!parse_text("", &out, &err));
  CHECK(!parse_text("garbage\n", &out, &err));
  // A truncated stream must not look like an empty but valid bench.
  CHECK(!parse_text("octomancer 1\nsnapshot devices=1\n", &out, &err));
  // A future protocol is refused rather than misread.
  CHECK(!parse_text("octomancer 99\nsnapshot\nend\n", &out, &err));
  CHECK(!parse_text("octomancer 1\nerror not right now\n", &out, &err));
}

void test_ignores_unknown_keys() {
  // Forward compatibility: a newer daemon adds fields and verbs, and an older
  // client must keep working rather than falling over.
  const std::string text =
      "octomancer 1\n"
      "snapshot wall=1.0 devices=1 live=1 has_bench=0 future_key=7\n"
      "weather sunny=1\n"
      "device id=x name=y live=1 has_time=1 offset=-6.5 gizmo=42\n"
      "end\n";
  Snapshot out;
  std::string err;
  CHECK(parse_text(text, &out, &err));
  CHECK_EQ(static_cast<long long>(out.device.size()), 1LL);
  CHECK_NEAR(out.device[0].offset, -6.5, 1e-9);
}

void test_json_is_wellformed_enough() {
  const std::string json = render_json(sample_snapshot());
  CHECK(json.front() == '{');
  CHECK(json.back() == '}');
  CHECK(json.find("\"name\":\"Cam 1 = A\"") != std::string::npos);
  CHECK(json.find("\"drift_ppm\":null") != std::string::npos);  // stale box
  CHECK(json.find("\"alerting\":true") != std::string::npos);
  // Balanced braces and brackets, which catches the usual comma/format slips.
  int braces = 0, brackets = 0;
  bool in_string = false, escaped = false;
  for (char c : json) {
    if (in_string) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') in_string = true;
    else if (c == '{') braces++;
    else if (c == '}') braces--;
    else if (c == '[') brackets++;
    else if (c == ']') brackets--;
    CHECK(braces >= 0);
    CHECK(brackets >= 0);
  }
  CHECK_EQ(braces, 0);
  CHECK_EQ(brackets, 0);
  CHECK(!in_string);
}


// The camera line has to survive the round trip, and -- more importantly --
// its absence has to be distinguishable from a camera that is switched off.
// Getting that wrong costs a twenty-second scan every cycle, or a night of
// never looking.
void test_camera_round_trip() {
  octo::Snapshot in;
  in.camera.reported = true;
  in.camera.seen = true;
  in.camera.present = true;
  in.camera.id = "B80D95C9-7D0B-140A-0351-2F4D55A1114E";
  in.camera.name = "Pocket Cinema Camera 6K Pro";
  in.camera.rssi = -62;
  in.camera.age = 1.25;
  in.camera.since = 4210.5;
  in.camera.sessions = 3;
  in.camera.adverts = 51234;
  in.camera.up_wall = 1787562773.5;

  octo::Snapshot out;
  std::string err;
  CHECK(octo::parse_text(octo::render_text(in), &out, &err));
  CHECK(out.camera.reported);
  CHECK(out.camera.seen);
  CHECK(out.camera.present);
  CHECK_STR(out.camera.id, in.camera.id);
  CHECK_STR(out.camera.name, in.camera.name);
  CHECK_EQ(out.camera.rssi, -62);
  CHECK_NEAR(out.camera.since, 4210.5, 0.01);
  CHECK_EQ(out.camera.sessions, 3u);
  CHECK_EQ(out.camera.adverts, 51234u);
  CHECK_NEAR(out.camera.up_wall, 1787562773.5, 0.01);
}

// A daemon that is watching and has heard nothing still says so. "No camera"
// and "no idea" are different answers and the reader acts differently on them.
void test_camera_absent_is_not_camera_unknown() {
  octo::Snapshot watching;
  watching.camera.reported = true;   // as every live daemon reports
  watching.camera.seen = false;

  octo::Snapshot out;
  std::string err;
  CHECK(octo::parse_text(octo::render_text(watching), &out, &err));
  CHECK(out.camera.reported);
  CHECK(!out.camera.seen);
  CHECK(!out.camera.present);

  // An older daemon emits no camera line at all, and that must not be read as
  // "there is no camera".
  octo::Snapshot old;
  std::string text = octo::render_text(old);
  CHECK(text.find("\ncamera ") == std::string::npos);
  CHECK(octo::parse_text(text, &out, &err));
  CHECK(!out.camera.reported);
}

// The camera's name comes off the air and is user-set, so it goes through the
// same escaping as a box name.
void test_hostile_camera_name() {
  octo::Snapshot in;
  in.camera.reported = true;
  in.camera.seen = true;
  in.camera.name = "cam era=\"one\" \\ two";

  octo::Snapshot out;
  std::string err;
  CHECK(octo::parse_text(octo::render_text(in), &out, &err));
  CHECK_STR(out.camera.name, in.camera.name);
}
}  // namespace

int main() {
  test_escaping_round_trip();
  test_text_round_trip();
  test_radio_links_round_trip();
  test_no_radio_line_means_no_second_radio();
  test_rejects_junk();
  test_ignores_unknown_keys();
  test_json_is_wellformed_enough();
  test_camera_round_trip();
  test_camera_absent_is_not_camera_unknown();
  test_hostile_camera_name();
  return octotest::report("test_proto");
}
