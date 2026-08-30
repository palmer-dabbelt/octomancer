// Pin the decoder to real hardware.
//
// The expectations in tests/data/adverts.golden were produced by the Python
// implementation that was validated against a bench of five boxes, so this is
// not a test of the C++ against itself: it is a test that the rewrite did not
// quietly change what the field data means. Payload decoding is exactly the
// place where a rewrite goes plausibly wrong -- a byte read as BCD instead of
// binary yields a valid-looking time, never an error.
#include "../src/tentacle.h"
#include "harness.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace octo;

namespace {

std::vector<uint8_t> from_hex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return out;
}

std::string golden_path() {
  // automake runs tests with srcdir set to the top of the source tree; a
  // developer running the binary by hand has neither that nor a fixed cwd.
  std::vector<std::string> tries;
  if (const char* dir = std::getenv("srcdir")) {
    if (*dir) {
      tries.push_back(std::string(dir) + "/tests/data/adverts.golden");
      tries.push_back(std::string(dir) + "/data/adverts.golden");
    }
  }
  tries.push_back("tests/data/adverts.golden");
  tries.push_back("data/adverts.golden");
  for (const std::string& path : tries) {
    std::ifstream probe(path);
    if (probe) return path;
  }
  return tries.front();
}

void test_golden() {
  std::ifstream in(golden_path());
  if (!in) {
    octotest::fail(__FILE__, __LINE__, "cannot open " + golden_path());
    return;
  }
  std::string line;
  int checked = 0, rejected = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string hex, verdict;
    fields >> hex >> verdict;

    const std::vector<uint8_t> bytes = from_hex(hex);
    const Decoded got = decode(bytes);

    if (verdict == "bad") {
      std::string note;
      std::getline(fields, note);
      if (!note.empty() && note[0] == ' ') note.erase(0, 1);
      CHECK(!got.ok);
      CHECK_STR(got.note, note);
      ++rejected;
      continue;
    }

    double sod = 0;
    std::string display, us;
    int fps = 0, frames = 0;
    fields >> sod >> display >> fps >> frames >> us;

    CHECK(got.ok);
    if (!got.ok) {
      octotest::fail(__FILE__, __LINE__, hex + ": rejected, note=" + got.note);
      continue;
    }
    // A microsecond is the finest thing either side claims to represent, so
    // anything above float noise here is a real disagreement.
    CHECK_NEAR(got.sod, sod, 1e-6);
    CHECK_STR(got.display, display);
    CHECK_EQ(got.fps, fps);
    CHECK_EQ(got.frames, frames);
    if (us == "us=-") {
      CHECK(!got.has_micros);
    } else {
      CHECK(got.has_micros);
      CHECK_EQ(static_cast<long>(got.micros),
               std::stol(us.substr(3)));
    }
    ++checked;
  }
  std::fprintf(stderr, "  golden: %d decoded, %d correctly rejected\n", checked,
               rejected);
  // Guard against the corpus silently going missing and the test passing by
  // vacuously checking nothing.
  CHECK(checked > 200);
  CHECK(rejected > 0);
}

void test_binary_not_bcd() {
  // The single most likely way to get this wrong. 0x22 header, then hours
  // 0x15: binary 21, not BCD 15. If someone "fixes" the decoder to read BCD
  // this test says so immediately.
  const std::vector<uint8_t> pkt = {0x22, 0x3d, 0x18, 0x15, 0x13, 0x2b, 0x00};
  const Decoded d = decode(pkt);
  CHECK(d.ok);
  CHECK_NEAR(d.sod, 21 * 3600 + 19 * 60 + 43, 1e-9);
  CHECK_EQ(d.fps, 24);
}

void test_rejects_rather_than_guesses() {
  // Out of range fields mean the layout guess is wrong; say so.
  const std::vector<uint8_t> bad_hour = {0x22, 0x3d, 0x18, 0x63, 0x00, 0x00, 0x00};
  CHECK(!decode(bad_hour).ok);
  const std::vector<uint8_t> bad_min = {0x22, 0x3d, 0x18, 0x01, 0x3c, 0x00, 0x00};
  CHECK(!decode(bad_min).ok);
  // A zero frame rate would divide by zero if it were trusted.
  const std::vector<uint8_t> no_fps = {0x22, 0x3d, 0x00, 0x01, 0x02, 0x03, 0x04};
  CHECK(!decode(no_fps).ok);
  const std::vector<uint8_t> empty;
  CHECK(!decode(empty).ok);
  CHECK_STR(decode(empty).note, "empty");
  // Truncated payloads must not read off the end.
  const std::vector<uint8_t> stub = {0x22, 0x3d, 0x18};
  CHECK(!decode(stub).ok);
  const std::vector<uint8_t> micros_stub = {0x32, 0x3d, 0x18, 0x00};
  CHECK(!decode(micros_stub).ok);
}

void test_subframe_micros_matter() {
  // Same frame, different sub-frame microseconds: the decoded times must
  // differ, or the sub-frame field is being dropped and every box collapses
  // back to 42 ms of quantisation.
  const std::vector<uint8_t> a = {0x22, 0x3d, 0x18, 0x15, 0x13, 0x2b, 0x04, 0x00, 0x00};
  const std::vector<uint8_t> b = {0x22, 0x3d, 0x18, 0x15, 0x13, 0x2b, 0x04, 0x52, 0x1f};
  const Decoded da = decode(a), db = decode(b);
  CHECK(da.ok && db.ok);
  CHECK_NEAR(db.sod - da.sod, 0.021023, 1e-9);
  CHECK_EQ(static_cast<int>(da.resolution), static_cast<int>(Resolution::kFrameMicros));
  // Without the sub-frame bytes it is honest about being frame resolution.
  const std::vector<uint8_t> c = {0x22, 0x3d, 0x18, 0x15, 0x13, 0x2b, 0x04};
  CHECK_EQ(static_cast<int>(decode(c).resolution), static_cast<int>(Resolution::kFrame));
}

// ------------------------------------------------- the encoder, and its point
//
// encode_* exists so a synthetic bench can transmit something this decoder
// will accept. That makes it dangerous in one specific way: if the encoder
// were written from the same misreading of the format as the decoder, the two
// would agree perfectly and a fake box would prove nothing at all. So the
// encoder is held to the golden vectors as well -- a payload it builds for a
// time taken *from* a real box has to come back as that same time.

// The strong one, and the reason the encoder can be trusted at all: take a
// payload captured off real hardware, decode it, and re-encode from the time
// that came out. The bytes have to come back identical.
//
// A round trip through the two functions in this file could pass while both
// of them misread the format in the same way -- that is the failure a fake
// bench would hide, because every box on it would be wrong in exactly the way
// the decoder expects. Anchoring one end to bytes a real box transmitted
// closes that off: the encoder is now pinned to the hardware, not to its
// neighbour.
void test_the_encoder_reproduces_real_adverts() {
  std::ifstream in(golden_path());
  if (!in) {
    octotest::fail(__FILE__, __LINE__, "cannot open " + golden_path());
    return;
  }
  std::string line;
  int checked = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string hex, verdict;
    fields >> hex >> verdict;
    if (verdict == "bad") continue;

    const std::vector<uint8_t> bytes = from_hex(hex);
    const Decoded d = decode(bytes);
    if (!d.ok || d.type != kTypeTimecode) continue;  // 0x32 has its own layout

    const std::vector<uint8_t> again =
        encode_timecode(d.sod, d.fps, d.has_micros, d.flags);
    CHECK_EQ(again.size(), bytes.size());
    if (again.size() != bytes.size()) continue;

    // Bytes 0-5 -- type, flags, rate, and the whole seconds -- have exactly
    // one correct value and are compared as bytes.
    CHECK_STR(to_hex(again.data(), 6), to_hex(bytes.data(), 6));

    // The frame number and the sub-frame field are *not* compared, because
    // the format does not determine them uniquely and the golden capture
    // proves it: 11 of its 212 timecode packets carry a sub-frame value
    // larger than one frame period -- up to 45262 us where a 24 fps frame is
    // 41667 -- so a box can and does describe an instant as frame 3 plus 45 ms
    // where this encoder writes frame 4 plus 3.6 ms. Both decode to the same
    // microsecond. Demanding identical bytes would be asserting that a real
    // box's own choice is the only one, which the file in front of it
    // disproves.
    //
    // What is asserted instead is the thing that has to be true: re-encoding a
    // time a real box transmitted gives back that same time. That still
    // anchors the encoder to the hardware rather than to the decoder beside
    // it, which is the whole reason for reading this file here.
    const Decoded round = decode(again);
    CHECK(round.ok);
    CHECK_NEAR(round.sod, d.sod, 1e-6);
    ++checked;
  }
  // The file is the point; an empty one passing would be the test lying.
  CHECK(checked > 100);
}

// The plain round trip. Weaker than the above, but it is the property the
// fake radio depends on directly, and it covers times no box happened to
// transmit while the golden capture was running.
void test_an_encoded_timecode_decodes_back() {
  struct Case {
    double sod;
    int fps;
  } cases[] = {
      {0.0, 24},                              // midnight
      {3661.5, 25},                           // an ordinary time
      {86399.9, 30},                          // the last second of the day
      {45296.0 + 7.0 / 24.0, 24},             // exactly on a frame boundary
      {45296.0 + 7.0 / 24.0 + 0.0036, 24},    // the ~3.6 ms floor seen in the field
      {12345.678, 60},                        // the fastest rate the format allows
  };
  for (const Case& c : cases) {
    const std::vector<uint8_t> packet = octo::encode_timecode(c.sod, c.fps);
    const octo::Decoded d = octo::decode(packet);
    CHECK(d.ok);
    CHECK_EQ(d.fps, c.fps);
    CHECK(d.has_micros);
    CHECK(d.resolution == octo::Resolution::kFrameMicros);
    // Within a microsecond: the frame field quantises and the sub-frame field
    // carries the remainder, so the two together reconstruct the input.
    CHECK_NEAR(d.sod, c.sod, 2e-6);
  }
}

// Omitting the sub-frame field is how a box that reports frame resolution is
// made. The decoder has a separate branch for it and a different Resolution,
// and a fake bench wants to be able to exercise both.
void test_a_short_packet_is_frame_resolution() {
  const double sod = 3661.0 + 7.0 / 24.0;
  const std::vector<uint8_t> packet = octo::encode_timecode(sod, 24, /*sub_frame=*/false);
  CHECK_EQ((int)packet.size(), 7);
  const octo::Decoded d = octo::decode(packet);
  CHECK(d.ok);
  CHECK(!d.has_micros);
  CHECK(d.resolution == octo::Resolution::kFrame);
  CHECK_EQ(d.frames, 7);
  CHECK_NEAR(d.sod, sod, 1e-9);
}

void test_an_encoded_microsecond_counter_decodes_back() {
  for (double sod : {0.0, 1.5, 43200.000001, 86399.999}) {
    const std::vector<uint8_t> packet = octo::encode_micros(sod);
    CHECK_EQ((int)packet.size(), 8);
    const octo::Decoded d = octo::decode(packet);
    CHECK(d.ok);
    CHECK(d.resolution == octo::Resolution::kMicrosecond);
    CHECK_NEAR(d.sod, sod, 2e-6);
  }
}

// A box that has drifted past midnight is a case worth being able to arrange,
// so the encoder wraps rather than refusing or emitting an hour of 24.
void test_the_encoder_wraps_around_midnight() {
  const octo::Decoded after = octo::decode(octo::encode_timecode(86400.5, 24));
  CHECK(after.ok);
  CHECK_NEAR(after.sod, 0.5, 2e-6);

  const octo::Decoded before = octo::decode(octo::encode_timecode(-0.5, 24));
  CHECK(before.ok);
  CHECK_NEAR(before.sod, 86399.5, 2e-6);
}

// The payload that carries no clock. The decoder must refuse it, and a fake
// bench with one on it is how that refusal gets exercised outside a unit test.
void test_a_static_payload_carries_no_time() {
  const octo::Decoded d = octo::decode(octo::encode_static());
  CHECK(!d.ok);
  CHECK(d.has_type);
  CHECK_EQ((int)d.type, (int)octo::kTypeStatic);
}

}  // namespace

int main() {
  test_golden();
  test_binary_not_bcd();
  test_rejects_rather_than_guesses();
  test_subframe_micros_matter();
  test_the_encoder_reproduces_real_adverts();
  test_an_encoded_timecode_decodes_back();
  test_a_short_packet_is_frame_resolution();
  test_an_encoded_microsecond_counter_decodes_back();
  test_the_encoder_wraps_around_midnight();
  test_a_static_payload_carries_no_time();
  return octotest::report("test_tentacle");
}
