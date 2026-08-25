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

}  // namespace

int main() {
  test_golden();
  test_binary_not_bcd();
  test_rejects_rather_than_guesses();
  test_subframe_micros_matter();
  return octotest::report("test_tentacle");
}
