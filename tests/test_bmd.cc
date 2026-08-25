// The packet encoder, checked against the worked examples on p105 of
// doc/BlackmagicCameraControl.pdf ("Example Protocol Packets") and the BCD
// time and date encodings on p102.
//
// These are the published bytes, not bytes this program produced and then
// enshrined. That distinction is the whole value of the file: a golden test
// written from your own output only proves the code has not changed.
#include <cstdint>
#include <string>
#include <vector>

#include "bmd.h"
#include "harness.h"

using octo::bmd::Civil;
using octo::bmd::Timecode;

namespace {

std::vector<uint8_t> bytes(std::initializer_list<int> v) {
  return std::vector<uint8_t>(v.begin(), v.end());
}

void check_packet(const char* what, const std::vector<uint8_t>& got,
                  const std::vector<uint8_t>& want) {
  if (got == want) return;
  octotest::fail(__FILE__, __LINE__,
                 std::string(what) + ": got [" + octo::bmd::to_hex(got) +
                     "] want [" + octo::bmd::to_hex(want) + "]");
}

void test_p105_examples() {
  check_packet("trigger instantaneous auto focus on camera 4",
               octo::bmd::build_packet(0, 1, 0, 0, {}, 4),
               bytes({4, 4, 0, 0, 0, 1, 0, 0}));

  check_packet("turn on OIS on all cameras",
               octo::bmd::build_packet(0, 6, 0, 0, bytes({1}), 255),
               bytes({255, 5, 0, 0, 0, 6, 0, 0, 1, 0, 0, 0}));

  // 10000 as int32 little-endian is 10 27 00 00.
  check_packet("set exposure to 10 ms on camera 4",
               octo::bmd::build_packet(1, 5, 3, 0, bytes({0x10, 0x27, 0, 0}), 4),
               bytes({4, 8, 0, 0, 1, 5, 3, 0, 0x10, 0x27, 0x00, 0x00}));

  check_packet("add 15% to zebra level",
               octo::bmd::build_packet(4, 2, 128, 1, bytes({0x33, 0x01}), 4),
               bytes({4, 6, 0, 0, 4, 2, 128, 1, 0x33, 0x01, 0, 0}));

  check_packet("select 1080p 23.98 mode on all cameras",
               octo::bmd::build_packet(1, 0, 1, 0, bytes({24, 1, 3, 0, 0}), 255),
               bytes({255, 9, 0, 0, 1, 0, 1, 0, 24, 1, 3, 0, 0, 0, 0, 0}));

  check_packet("subtract 0.3 from gamma adjust for green & blue",
               octo::bmd::build_packet(
                   8, 1, 128, 1, bytes({0, 0, 0x9A, 0xFD, 0x9A, 0xFD, 0, 0}), 4),
               bytes({4, 12, 0, 0, 8, 1, 128, 1, 0, 0, 0x9A, 0xFD, 0x9A, 0xFD, 0,
                      0}));
}

void test_bcd() {
  CHECK_EQ(octo::bmd::encode_time(9, 12, 53, 10), 0x09125310u);
  CHECK_EQ(octo::bmd::encode_date(2026, 8, 24), 0x20260824u);
  CHECK_EQ(octo::bmd::encode_time(23, 59, 59, 29), 0x23595929u);
  CHECK_EQ(octo::bmd::encode_date(1999, 12, 31), 0x19991231u);

  std::string round;
  CHECK(octo::bmd::decode_bcd_timecode(octo::bmd::encode_time(9, 12, 53, 10),
                                       &round));
  CHECK_STR(round, "09:12:53:10");

  // Non-BCD nibbles must be refused rather than silently mangled: a hex word
  // decoded as BCD yields a believable wrong time, which is worse than an
  // admission that the bytes were not understood.
  CHECK(!octo::bmd::decode_bcd_timecode(0xAABBCCDDu, &round));
  CHECK(!octo::bmd::decode_bcd_timecode(0x0A125310u, &round));
}

void test_rtc_packet() {
  Civil when;
  when.year = 2026;
  when.month = 8;
  when.day = 24;
  when.hour = 9;
  when.minute = 12;
  when.second = 53;

  const std::vector<uint8_t> pkt = octo::bmd::rtc_packet(when, 10, 255);
  check_packet("Real Time Clock (2026-08-24 09:12:53:10 UTC)", pkt,
               bytes({255, 12, 0, 0, 7, 0, 3, 0,
                      0x10, 0x53, 0x12, 0x09,    // time 0x09125310 LE
                      0x24, 0x08, 0x26, 0x20})); // date 0x20260824 LE

  // The length field covers the body only, and the packet is padded to a
  // 32-bit boundary that the length must not include.
  CHECK_EQ(static_cast<int>(pkt[1]), 12);
  CHECK_EQ(pkt.size(), static_cast<size_t>(16));
  CHECK_EQ(pkt.size() % 4, static_cast<size_t>(0));
}

void test_utc_civil() {
  // 2026-08-24T09:12:53Z. Fractions truncate rather than round, because the
  // caller decides where the boundary is: see camsync's aligned_value, which
  // rounds deliberately before calling this.
  const Civil c = octo::bmd::utc_civil(1787562773.75);
  CHECK_EQ(c.year, 2026);
  CHECK_EQ(c.month, 8);
  CHECK_EQ(c.day, 24);
  CHECK_EQ(c.hour, 9);
  CHECK_EQ(c.minute, 12);
  CHECK_EQ(c.second, 53);
}

void test_parse_timecode() {
  // A real notification from a Pocket 6K Pro: the camera wraps the timecode in
  // a whole SDI message rather than sending the bare BCD word the document
  // describes.
  const std::vector<uint8_t> notif =
      bytes({0xff, 0x08, 0x00, 0xff, 0x09, 0x04, 0x03, 0x00, 0x18, 0x14, 0x55,
             0x17});
  Timecode tc;
  CHECK(octo::bmd::parse_timecode(notif, &tc));
  CHECK_STR(octo::bmd::format_timecode(tc), "17:55:14:18");

  // Frames count towards the seconds-of-day at the reported rate, and are the
  // reason the reading is quantised at all.
  CHECK_NEAR(octo::bmd::timecode_sod(tc, 24), 17 * 3600 + 55 * 60 + 14 + 18 / 24.0,
             1e-9);
  CHECK_NEAR(octo::bmd::timecode_sod(tc, 0), 17 * 3600 + 55 * 60 + 14, 1e-9);

  // Garbage must not produce a plausible time.
  CHECK(!octo::bmd::parse_timecode(bytes({0x01, 0x02}), &tc));
  CHECK(!octo::bmd::parse_timecode(
      bytes({0xff, 0x08, 0x00, 0xff, 0x09, 0x04, 0x03, 0x00, 0xAA, 0xBB, 0xCC,
             0xDD}),
      &tc));
}

void test_parse_stream() {
  // Two messages back to back, the first padded to a 32-bit boundary. Getting
  // the padding wrong silently loses the second message.
  std::vector<uint8_t> buf =
      bytes({255, 5, 0, 0, 0, 6, 0, 0, 1, 0, 0, 0,
             4, 4, 0, 0, 0, 1, 0, 0});
  const std::vector<octo::bmd::Message> msgs = octo::bmd::parse_stream(buf);
  CHECK_EQ(msgs.size(), static_cast<size_t>(2));
  if (msgs.size() == 2) {
    CHECK_EQ(static_cast<int>(msgs[0].dest), 255);
    CHECK_EQ(static_cast<int>(msgs[1].dest), 4);
  }

  // A truncated trailing message is dropped, not half-decoded.
  buf.push_back(4);
  buf.push_back(40);
  buf.push_back(0);
  buf.push_back(0);
  CHECK_EQ(octo::bmd::parse_stream(buf).size(), static_cast<size_t>(2));
}

void test_decode_value() {
  // Transport mode: 10.1 int8, the gate that stops a write mid-take.
  const std::vector<octo::bmd::Message> msgs =
      octo::bmd::parse_stream(bytes({255, 5, 0, 0, 10, 1, 1, 0, 2, 0, 0, 0}));
  CHECK_EQ(msgs.size(), static_cast<size_t>(1));
  if (msgs.empty()) return;
  octo::bmd::Value v;
  CHECK(octo::bmd::decode_value(msgs[0], &v));
  CHECK_EQ(static_cast<int>(v.group), 10);
  CHECK_EQ(static_cast<int>(v.param), 1);
  CHECK(!v.ints.empty());
  if (!v.ints.empty()) CHECK_EQ(v.ints[0], octo::bmd::kTransportRecord);

  // int8 is signed: a frame rate byte that read as 200 rather than -56 would
  // be a plausible number, so the sign matters even where it never fires.
  const std::vector<octo::bmd::Message> neg =
      octo::bmd::parse_stream(bytes({255, 5, 0, 0, 1, 9, 1, 0, 0xC8, 0, 0, 0}));
  CHECK(octo::bmd::decode_value(neg[0], &v));
  CHECK_EQ(v.ints[0], static_cast<int64_t>(-56));

  // fixed16 is a signed 16-bit value over 2048.
  const std::vector<octo::bmd::Message> fx =
      octo::bmd::parse_stream(bytes({4, 6, 0, 0, 4, 2, 128, 1, 0x00, 0x08, 0, 0}));
  CHECK(octo::bmd::decode_value(fx[0], &v));
  CHECK_EQ(v.reals.size(), static_cast<size_t>(1));
  if (!v.reals.empty()) CHECK_NEAR(v.reals[0], 1.0, 1e-9);
}

}  // namespace

int main() {
  test_p105_examples();
  test_bcd();
  test_rtc_packet();
  test_utc_civil();
  test_parse_timecode();
  test_parse_stream();
  test_decode_value();
  return octotest::report("test_bmd");
}
