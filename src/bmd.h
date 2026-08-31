// The Blackmagic SDI Camera Control Protocol, as spoken over Bluetooth LE.
//
// The BLE "Outgoing Camera Control" characteristic is a plain tunnel: whatever
// bytes go in are handed to the same protocol the SDI pins carry, so setting a
// camera's Real Time Clock over the air is a matter of encoding one SDI packet
// correctly. Reference is doc/BlackmagicCameraControl.pdf (August 2025), where
// the packet layout is p96-97, the worked examples p105, the BCD time and date
// encodings p102, and the BLE service p109-110.
//
// Everything here is pure byte arithmetic with no radio in it, which is what
// lets tests/test_bmd.cc check it against the published examples on a machine
// with no camera anywhere near it.
//
// One trap is worth stating up front, because getting it backwards produces a
// plausible wrong time rather than an obvious error: Blackmagic sends timecode
// as packed **BCD**, so the byte 0x15 means 21. The Tentacle boxes decoded in
// tentacle.h send plain binary, where 0x15 means 15. The two halves of this
// program therefore decode their times in opposite ways on purpose.
#ifndef OCTO_BMD_H
#define OCTO_BMD_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace octo {
namespace bmd {

// ------------------------------------------------------------- UUIDs (p109)

extern const char kServiceCamera[];
extern const char kCharOutgoingControl[];  // write    (encrypted)
extern const char kCharIncomingControl[];  // notify   (encrypted)
extern const char kCharTimecode[];         // notify   (encrypted)
extern const char kCharCameraStatus[];     // r/w/notify
extern const char kCharDeviceName[];
extern const char kCharProtocolVersion[];

// ------------------------------------------------------ protocol constants

inline constexpr uint8_t kCmdChangeConfig = 0;
inline constexpr uint8_t kTypeVoid = 0;
inline constexpr uint8_t kTypeInt8 = 1;
inline constexpr uint8_t kTypeInt16 = 2;
inline constexpr uint8_t kTypeInt32 = 3;
inline constexpr uint8_t kTypeInt64 = 4;
inline constexpr uint8_t kTypeUtf8 = 5;
inline constexpr uint8_t kTypeFixed16 = 128;
inline constexpr uint8_t kOpAssign = 0;
inline constexpr uint8_t kBroadcast = 255;

inline constexpr uint8_t kGroupConfig = 7;
inline constexpr uint8_t kParamRtc = 0;
inline constexpr uint8_t kParamTimezone = 2;

// Group 9 is not in the published parameter table -- the document jumps from 8
// (Colour Correction) to 10 (Media). A Pocket 6K Pro nonetheless reports its
// running timecode as 9.4 int32, BCD HHMMSSFF, on the Timecode characteristic.
inline constexpr uint8_t kGroupStatus = 9;
inline constexpr uint8_t kParamTimecode = 4;

// 10.1[0]: 0 Preview, 1 Play, 2 Record (p103). The only gate that matters more
// than accuracy: jumping timecode mid-take corrupts the take.
inline constexpr uint8_t kGroupMedia = 10;
inline constexpr uint8_t kParamTransport = 1;
inline constexpr int64_t kTransportRecord = 2;

// 1.9[0] is the sensor/recording frame rate on the bodies seen here.
inline constexpr uint8_t kGroupVideo = 1;
inline constexpr uint8_t kParamFrameRate = 9;

// 4.7 is not in the published parameter table either, but a Pocket 6K Pro
// volunteers it on connect as int8 and accepts a write to it -- the only write
// this project has ever found that moves the timecode generator. See
// doc/timecode-write-sweep.md.
//
// The two values are named for what they were observed to do rather than for
// what Blackmagic calls them, because we do not know what Blackmagic calls
// them. With 0 the timecode free-runs as time of day and follows the RTC,
// which is the mode this whole program depends on. With 1 it parks at
// 00:00:00:00 and stops, and writing the RTC has no effect on it at all.
// Anything above 1 is clamped by the camera to 1.
inline constexpr uint8_t kGroupOutput = 4;
inline constexpr uint8_t kParamTimecodeSource = 7;
inline constexpr int64_t kTimecodeSourceTimeOfDay = 0;
inline constexpr int64_t kTimecodeSourceClip = 1;

// ------------------------------------------------------------ encoding

// Wrap a change-configuration command in the 4-byte header. The length field
// covers category/parameter/type/operation plus payload: it excludes the
// header itself and any trailing padding added to reach 32-bit alignment.
std::vector<uint8_t> build_packet(uint8_t category, uint8_t parameter,
                                  uint8_t data_type, uint8_t operation,
                                  const std::vector<uint8_t>& payload,
                                  uint8_t dest = kBroadcast);

// Two decimal digits to one packed BCD byte. Digits above 99 cannot be
// represented and are not silently truncated -- callers pass calendar fields.
uint8_t bcd2(int n);

uint32_t encode_time(int h, int m, int s, int f);   // BCD HHMMSSFF   (p102)
uint32_t encode_date(int y, int mo, int d);         // BCD YYYYMMDD   (p102)

// A UTC calendar instant. The RTC is specified in UTC and the camera applies
// its own Timezone parameter before display, so writing local time gets the
// offset applied twice -- an hours-sized error that looks like a failed write.
struct Civil {
  int year = 1970;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;
};

// Break a Unix timestamp into UTC calendar fields, truncating the fraction.
Civil utc_civil(double unix_seconds);

// Move a calendar date by whole days, correctly across months, years and leap
// years. Integer arithmetic only: this is compiled for the dongle too, where
// the C library is picolibc and a date routine that reaches for a timezone is
// a hazard rather than a convenience -- see doc/dongle-notes.md, where
// localtime_r cost an evening.
//
// Only the date fields move; the time of day is left exactly as it was, which
// is what every caller here wants.
void add_days(Civil* when, int days);

// Real Time Clock: group 7 parameter 0, int32[2] = {time, date}, each field
// little-endian per the p105 examples.
std::vector<uint8_t> rtc_packet(const Civil& when, int frames,
                                uint8_t dest = kBroadcast);

// ------------------------------------------------------------ decoding

// 0x09125310 -> "09:12:53:10". Returns false if any nibble is not a decimal
// digit, because a non-BCD word decoded as BCD yields a believable wrong time.
bool decode_bcd_timecode(uint32_t word, int* h, int* m, int* s, int* f);

// 0x20260831 -> 2026, 8, 31. The other half of encode_date, and the reason it
// exists is worth stating: a box that has never been told the date can still
// write a camera's clock, because the camera knows its own date and only the
// time of day is wrong. Reading it back is what makes that possible.
//
// Refuses anything that is not four BCD bytes *and* not a plausible calendar
// date. A camera that has never had its clock set reports zeros, and a zeroth
// day of a zeroth month written back as though it were a date is worse than
// admitting we do not know: it would take a camera whose date was merely
// unset and give it one that is wrong.
bool decode_bcd_date(uint32_t word, int* y, int* mo, int* d);
bool decode_bcd_timecode(uint32_t word, std::string* out);

struct Message {
  uint8_t dest = 0;
  uint8_t cmd = 0;
  std::vector<uint8_t> body;  // category, parameter, type, operation, payload
};

// Split a run of concatenated SDI messages (p96 allows up to 32 per packet).
// Stops at the first truncated message rather than guessing at the remainder.
std::vector<Message> parse_stream(const uint8_t* data, size_t len);
std::vector<Message> parse_stream(const std::vector<uint8_t>& data);

// The decoded payload of one message. Integers of every width land in `ints`
// so callers do not have to switch on the type to read a transport mode.
struct Value {
  uint8_t group = 0;
  uint8_t param = 0;
  uint8_t type = 0;
  uint8_t op = 0;
  std::vector<int64_t> ints;
  std::vector<double> reals;  // fixed16 only
  std::string text;           // utf8 only
};

bool decode_value(const Message& msg, Value* out);

// The Real Time Clock as the camera reports it: the same int32[2] pair that
// rtc_packet writes, {time, date}. Fills whichever of `date` and `sod` is
// asked for, and returns false unless *both* halves decode -- a camera that
// answers with half a clock has told us nothing we can safely use, and the
// caller's next move either way is to leave its clock alone.
//
// `sod` is seconds since midnight, from the time half, ignoring the frames
// field: the RTC's frame count is not a clock, it is whatever the camera
// happened to be showing.
bool decode_rtc(const Value& value, Civil* date, double* sod);

struct Timecode {
  int hours = 0;
  int minutes = 0;
  int seconds = 0;
  int frames = 0;
};

// Pull the timecode out of a Timecode-characteristic notification. The camera
// wraps it in a whole SDI message rather than sending the bare 32-bit BCD word
// the documentation describes:
//
//   ff 08 00 ff | 09 04 03 00 | 18 14 55 17
//   dest/len/cmd  grp 9 par 4   BCD little-endian -> 17:55:14:18
bool parse_timecode(const uint8_t* data, size_t len, Timecode* out);
bool parse_timecode(const std::vector<uint8_t>& data, Timecode* out);

// Seconds since midnight for a timecode read at `fps`. Frames are the camera's
// own unit and it reports whole ones, so this is quantised to 1/fps.
double timecode_sod(const Timecode& tc, int fps);

std::string format_timecode(const Timecode& tc);
std::string to_hex(const std::vector<uint8_t>& data);

// Render incoming camera-control traffic as one human-readable line per
// message, for the probe modes.
std::vector<std::string> describe(const std::vector<uint8_t>& data);

}  // namespace bmd
}  // namespace octo

#endif  // OCTO_BMD_H
