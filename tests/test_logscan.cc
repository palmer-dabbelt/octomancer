// Reading back what the daemon wrote.
//
// The scanner is deliberately not a general JSON parser, so the thing worth
// testing is where it draws the line: it must read every shape octomancer
// emits, including a box name with a quote in it, and it must refuse anything
// it does not understand rather than returning a half-parsed record that reads
// as valid data.
#include <map>
#include <string>

#include "harness.h"
#include "logscan.h"

namespace {

void test_flat_record() {
  octo::LogRecord rec;
  CHECK(octo::parse_record(
      "{\"event\":\"cycle\",\"wall\":1787908373.500,\"error_s\":-0.0380,"
      "\"verified\":true,\"action\":\"write:ok\",\"rtc_bias\":-75}",
      &rec));
  CHECK_STR(rec.text("event"), "cycle");
  CHECK_STR(rec.text("action"), "write:ok");
  CHECK_NEAR(rec.number("error_s"), -0.038, 1e-9);
  CHECK_EQ(static_cast<int>(rec.number("rtc_bias")), -75);
  CHECK(rec.flag("verified"));

  // A field an older log never wrote must read as its fallback, not as zero
  // dressed up as a measurement.
  CHECK(!rec.has("anchor_drift_ppm"));
  CHECK_NEAR(rec.number("anchor_drift_ppm", -1.0), -1.0, 1e-9);
  CHECK_STR(rec.text("missing", "n/a"), "n/a");
  CHECK(rec.flag("missing", true));
}

void test_nested_boxes() {
  octo::LogRecord rec;
  CHECK(octo::parse_record(
      "{\"event\":\"cycle\",\"tentacles\":5,\"boxes\":{\"Krysta\":"
      "{\"offset_s\":-6.2050,\"adverts\":42,\"rssi\":-60,"
      "\"resolution\":\"microsecond\"},\"FS7\":{\"offset_s\":-6.2080,"
      "\"adverts\":38,\"rssi\":-71,\"resolution\":\"frame+us\"}},"
      "\"error_s\":0.5}",
      &rec));
  // The nested object must be captured whole: a scanner that stopped at the
  // first closing brace would silently drop error_s and every field after it.
  CHECK_NEAR(rec.number("error_s"), 0.5, 1e-9);

  std::map<std::string, std::string> boxes;
  CHECK(octo::parse_object(rec.raw("boxes"), &boxes));
  CHECK_EQ(boxes.size(), static_cast<size_t>(2));

  std::map<std::string, std::string> krysta;
  CHECK(octo::parse_object(boxes["Krysta"], &krysta));
  octo::LogRecord box;
  for (const auto& e : krysta) box.set(e.first, e.second);
  CHECK_NEAR(box.number("offset_s"), -6.205, 1e-9);
  CHECK_STR(box.text("resolution"), "microsecond");
}

void test_hostile_box_names() {
  // Box names are user-set and arrive from the air. A name with a quote in it
  // must not be able to end the string early and turn the rest of the line
  // into keys and values of its own choosing.
  octo::LogRecord rec;
  CHECK(octo::parse_record(
      "{\"event\":\"cycle\",\"boxes\":{\"say \\\"hi\\\", \\\\ ok\":"
      "{\"offset_s\":-1.5}},\"error_s\":2.0}",
      &rec));
  CHECK_NEAR(rec.number("error_s"), 2.0, 1e-9);

  std::map<std::string, std::string> boxes;
  CHECK(octo::parse_object(rec.raw("boxes"), &boxes));
  CHECK_EQ(boxes.size(), static_cast<size_t>(1));
  if (!boxes.empty()) {
    CHECK_STR(boxes.begin()->first, "say \"hi\", \\ ok");
  }

  // A control character escaped as \\u001f decodes back to one byte.
  std::string out;
  CHECK(octo::json_string("\"a\\u001fb\"", &out));
  CHECK_EQ(out.size(), static_cast<size_t>(3));
}

void test_malformed_is_refused() {
  octo::LogRecord rec;
  // A line cut in half by a machine losing power mid-write. Skipping it is
  // right; reading the fragment as a cycle is not.
  CHECK(!octo::parse_record("{\"event\":\"cycle\",\"error_s\":-0.0", &rec));
  CHECK(!octo::parse_record("{\"event\":\"cycle\"", &rec));
  CHECK(!octo::parse_record("not json at all", &rec));
  CHECK(!octo::parse_record("", &rec));
  CHECK(!octo::parse_record("{\"unterminated\":\"abc}", &rec));
  CHECK(!octo::parse_record("{\"boxes\":{\"a\":{\"b\":1}}", &rec));

  // An empty object is valid and simply carries nothing.
  octo::LogRecord empty;
  CHECK(octo::parse_record("{}", &empty));
  CHECK(empty.fields().empty());
}

void test_both_timestamp_spellings() {
  // The current daemon writes a number...
  octo::LogRecord now;
  CHECK(octo::parse_record("{\"event\":\"cycle\",\"wall\":1787908373.5}", &now));
  double t = 0.0;
  CHECK(octo::record_time(now, &t));
  CHECK_NEAR(t, 1787908373.5, 1e-6);

  // ...and the Python daemon this replaced wrote a local ISO-8601 string.
  // Both must read, or the nights of drift data already collected are lost.
  octo::LogRecord then;
  CHECK(octo::parse_record(
      "{\"event\":\"cycle\",\"wall\":\"2026-08-24T21:42:05.250\"}", &then));
  double t2 = 0.0;
  CHECK(octo::record_time(then, &t2));

  // Compared against another record rather than an absolute value: the answer
  // depends on the host's timezone, and the property that matters is that
  // differences between records come out right.
  octo::LogRecord later;
  CHECK(octo::parse_record(
      "{\"event\":\"cycle\",\"wall\":\"2026-08-24T21:43:05.250\"}", &later));
  double t3 = 0.0;
  CHECK(octo::record_time(later, &t3));
  CHECK_NEAR(t3 - t2, 60.0, 1e-3);

  octo::LogRecord none;
  CHECK(octo::parse_record("{\"event\":\"cycle\"}", &none));
  CHECK(!octo::record_time(none, &t));
}

}  // namespace

int main() {
  test_flat_record();
  test_nested_boxes();
  test_hostile_box_names();
  test_malformed_is_refused();
  test_both_timestamp_spellings();
  return octotest::report("test_logscan");
}
