#include "bmd.h"

#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace octo {
namespace bmd {

const char kServiceCamera[] = "291d567a-6d75-11e6-8b77-86f30ca893d3";
const char kCharOutgoingControl[] = "5dd3465f-1aee-4299-8493-d2eca2f8e1bb";
const char kCharIncomingControl[] = "b864e140-76a0-416a-bf30-5876504537d9";
const char kCharTimecode[] = "6d8f2110-86f1-41bf-9afb-451d87e976c8";
const char kCharCameraStatus[] = "7fe8691d-95dc-4fc5-8abd-ca74339b51b9";
const char kCharDeviceName[] = "ffac0c52-c9fb-41a0-b063-cc76282eb89c";
const char kCharProtocolVersion[] = "8f1fd018-b508-456f-8f82-3d392bee2706";

namespace {

// Little-endian, because that is what the p105 worked examples show even
// though the surrounding prose does not say so anywhere.
void put_u32(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

int64_t read_le(const uint8_t* p, int width, bool sign) {
  uint64_t v = 0;
  for (int i = width - 1; i >= 0; --i) v = (v << 8) | p[i];
  if (!sign) return static_cast<int64_t>(v);
  const int bits = width * 8;
  if (bits < 64 && (v & (1ULL << (bits - 1)))) {
    v |= ~((1ULL << bits) - 1);
  }
  return static_cast<int64_t>(v);
}

}  // namespace

std::vector<uint8_t> build_packet(uint8_t category, uint8_t parameter,
                                  uint8_t data_type, uint8_t operation,
                                  const std::vector<uint8_t>& payload,
                                  uint8_t dest) {
  std::vector<uint8_t> body;
  body.reserve(4 + payload.size());
  body.push_back(category);
  body.push_back(parameter);
  body.push_back(data_type);
  body.push_back(operation);
  body.insert(body.end(), payload.begin(), payload.end());

  std::vector<uint8_t> pkt;
  pkt.reserve(4 + body.size() + 3);
  pkt.push_back(dest);
  pkt.push_back(static_cast<uint8_t>(body.size()));
  pkt.push_back(kCmdChangeConfig);
  pkt.push_back(0);
  pkt.insert(pkt.end(), body.begin(), body.end());
  while (pkt.size() % 4 != 0) pkt.push_back(0);
  return pkt;
}

uint8_t bcd2(int n) {
  if (n < 0) n = 0;
  n %= 100;
  return static_cast<uint8_t>(((n / 10) << 4) | (n % 10));
}

uint32_t encode_time(int h, int m, int s, int f) {
  return (static_cast<uint32_t>(bcd2(h)) << 24) |
         (static_cast<uint32_t>(bcd2(m)) << 16) |
         (static_cast<uint32_t>(bcd2(s)) << 8) | bcd2(f);
}

uint32_t encode_date(int y, int mo, int d) {
  return (static_cast<uint32_t>(bcd2(y / 100)) << 24) |
         (static_cast<uint32_t>(bcd2(y % 100)) << 16) |
         (static_cast<uint32_t>(bcd2(mo)) << 8) | bcd2(d);
}

Civil utc_civil(double unix_seconds) {
  const time_t secs = static_cast<time_t>(std::floor(unix_seconds));
  struct tm tm_utc;
  ::gmtime_r(&secs, &tm_utc);
  Civil c;
  c.year = tm_utc.tm_year + 1900;
  c.month = tm_utc.tm_mon + 1;
  c.day = tm_utc.tm_mday;
  c.hour = tm_utc.tm_hour;
  c.minute = tm_utc.tm_min;
  c.second = tm_utc.tm_sec;
  return c;
}

namespace {

// Howard Hinnant's civil-from-days pair, which is exact for every date in
// range and needs no library. Days are counted from 1970-01-01.
int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy =
      (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yr = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp + (mp < 10 ? 3 : -9);
  *y = static_cast<int>(yr + (*m <= 2));
}

}  // namespace

void add_days(Civil* when, int days) {
  if (when == nullptr || days == 0) return;
  const int64_t z = days_from_civil(when->year, when->month, when->day) + days;
  unsigned m = 0, d = 0;
  civil_from_days(z, &when->year, &m, &d);
  when->month = static_cast<int>(m);
  when->day = static_cast<int>(d);
}

std::vector<uint8_t> rtc_packet(const Civil& when, int frames, uint8_t dest) {
  std::vector<uint8_t> payload;
  payload.reserve(8);
  put_u32(&payload, encode_time(when.hour, when.minute, when.second, frames));
  put_u32(&payload, encode_date(when.year, when.month, when.day));
  return build_packet(kGroupConfig, kParamRtc, kTypeInt32, kOpAssign, payload,
                      dest);
}

bool decode_bcd_timecode(uint32_t word, int* h, int* m, int* s, int* f) {
  int out[4];
  const int shifts[4] = {24, 16, 8, 0};
  for (int i = 0; i < 4; ++i) {
    const uint8_t byte = static_cast<uint8_t>((word >> shifts[i]) & 0xFF);
    const int hi = byte >> 4, lo = byte & 0x0F;
    if (hi > 9 || lo > 9) return false;
    out[i] = hi * 10 + lo;
  }
  if (h) *h = out[0];
  if (m) *m = out[1];
  if (s) *s = out[2];
  if (f) *f = out[3];
  return true;
}

bool decode_bcd_date(uint32_t word, int* y, int* mo, int* d) {
  int part[4];
  const int shifts[4] = {24, 16, 8, 0};
  for (int i = 0; i < 4; ++i) {
    const uint8_t byte = static_cast<uint8_t>((word >> shifts[i]) & 0xFF);
    const int hi = byte >> 4, lo = byte & 0x0F;
    if (hi > 9 || lo > 9) return false;
    part[i] = hi * 10 + lo;
  }
  const int year = part[0] * 100 + part[1];
  const int month = part[2];
  const int day = part[3];
  // A calendar check rather than only a BCD one. Zeros are what a camera that
  // has never had its clock set reports, and they decode as perfectly valid
  // BCD; writing 0000-00-00 back would replace "unset" with "wrong".
  if (year < 2000 || year > 2099) return false;
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > 31) return false;
  if (y) *y = year;
  if (mo) *mo = month;
  if (d) *d = day;
  return true;
}

bool decode_rtc(const Value& value, Civil* date, double* sod) {
  if (value.ints.size() < 2) return false;
  const uint32_t time_word = static_cast<uint32_t>(value.ints[0] & 0xFFFFFFFF);
  const uint32_t date_word = static_cast<uint32_t>(value.ints[1] & 0xFFFFFFFF);

  int y = 0, mo = 0, d = 0;
  if (!decode_bcd_date(date_word, &y, &mo, &d)) return false;
  int h = 0, m = 0, s = 0, f = 0;
  if (!decode_bcd_timecode(time_word, &h, &m, &s, &f)) return false;
  // A BCD word can hold 99 hours; a clock cannot.
  if (h > 23 || m > 59 || s > 59) return false;

  if (date != nullptr) {
    date->year = y;
    date->month = mo;
    date->day = d;
    date->hour = h;
    date->minute = m;
    date->second = s;
  }
  if (sod != nullptr) *sod = h * 3600.0 + m * 60.0 + s;
  return true;
}

bool decode_bcd_timecode(uint32_t word, std::string* out) {
  int h, m, s, f;
  if (!decode_bcd_timecode(word, &h, &m, &s, &f)) return false;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d:%02d:%02d:%02d", h, m, s, f);
  if (out) *out = buf;
  return true;
}

std::vector<Message> parse_stream(const uint8_t* data, size_t len) {
  std::vector<Message> msgs;
  size_t i = 0;
  while (i + 4 <= len) {
    Message msg;
    msg.dest = data[i];
    const size_t length = data[i + 1];
    msg.cmd = data[i + 2];
    // A truncated trailing message is not a parse failure worth reporting: BLE
    // hands over whole notifications, so this means the shape is not what we
    // think it is. Stop, and let the caller fall back to showing raw bytes.
    if (i + 4 + length > len) break;
    msg.body.assign(data + i + 4, data + i + 4 + length);
    msgs.push_back(std::move(msg));
    size_t step = 4 + length;
    step += (4 - step % 4) % 4;  // skip the implicit 32-bit alignment padding
    if (step == 0) break;
    i += step;
  }
  return msgs;
}

std::vector<Message> parse_stream(const std::vector<uint8_t>& data) {
  return parse_stream(data.data(), data.size());
}

bool decode_value(const Message& msg, Value* out) {
  if (msg.cmd != kCmdChangeConfig || msg.body.size() < 4) return false;
  Value v;
  v.group = msg.body[0];
  v.param = msg.body[1];
  v.type = msg.body[2];
  v.op = msg.body[3];
  const uint8_t* p = msg.body.data() + 4;
  const size_t n = msg.body.size() - 4;
  switch (v.type) {
    case kTypeVoid:
      for (size_t i = 0; i < n; ++i) v.ints.push_back(p[i]);
      break;
    case kTypeInt8:
      for (size_t i = 0; i < n; ++i) v.ints.push_back(read_le(p + i, 1, true));
      break;
    case kTypeInt16:
      for (size_t i = 0; i + 2 <= n; i += 2)
        v.ints.push_back(read_le(p + i, 2, true));
      break;
    case kTypeInt32:
      for (size_t i = 0; i + 4 <= n; i += 4)
        v.ints.push_back(read_le(p + i, 4, true));
      break;
    case kTypeInt64:
      for (size_t i = 0; i + 8 <= n; i += 8)
        v.ints.push_back(read_le(p + i, 8, true));
      break;
    case kTypeUtf8:
      v.text.assign(reinterpret_cast<const char*>(p), n);
      break;
    case kTypeFixed16:
      for (size_t i = 0; i + 2 <= n; i += 2)
        v.reals.push_back(read_le(p + i, 2, true) / 2048.0);
      break;
    default:
      for (size_t i = 0; i < n; ++i) v.ints.push_back(p[i]);
      break;
  }
  if (out) *out = std::move(v);
  return true;
}

bool parse_timecode(const uint8_t* data, size_t len, Timecode* out) {
  for (const Message& msg : parse_stream(data, len)) {
    if (msg.cmd != kCmdChangeConfig) continue;
    if (msg.body.size() < 8 || msg.body[2] != kTypeInt32) continue;
    const uint32_t word =
        static_cast<uint32_t>(read_le(msg.body.data() + 4, 4, false));
    Timecode tc;
    if (!decode_bcd_timecode(word, &tc.hours, &tc.minutes, &tc.seconds,
                             &tc.frames)) {
      continue;
    }
    if (out) *out = tc;
    return true;
  }
  return false;
}

bool parse_timecode(const std::vector<uint8_t>& data, Timecode* out) {
  return parse_timecode(data.data(), data.size(), out);
}

double timecode_sod(const Timecode& tc, int fps) {
  double sod = tc.hours * 3600.0 + tc.minutes * 60.0 + tc.seconds;
  if (fps > 0) sod += tc.frames / static_cast<double>(fps);
  return sod;
}

std::string format_timecode(const Timecode& tc) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d:%02d:%02d:%02d", tc.hours, tc.minutes,
                tc.seconds, tc.frames);
  return buf;
}

std::string to_hex(const std::vector<uint8_t>& data) {
  std::string out;
  out.reserve(data.size() * 3);
  char buf[4];
  for (size_t i = 0; i < data.size(); ++i) {
    std::snprintf(buf, sizeof buf, "%02x", data[i]);
    if (i) out.push_back(' ');
    out += buf;
  }
  return out;
}

namespace {

const char* type_name(uint8_t t) {
  switch (t) {
    case kTypeVoid: return "void/bool";
    case kTypeInt8: return "int8";
    case kTypeInt16: return "int16";
    case kTypeInt32: return "int32";
    case kTypeInt64: return "int64";
    case kTypeUtf8: return "utf8";
    case kTypeFixed16: return "fixed16";
    default: return "?";
  }
}

const char* param_name(uint8_t g, uint8_t p) {
  if (g == kGroupVideo && p == kParamFrameRate) return "Video/frame rate";
  if (g == kGroupConfig && p == kParamRtc) return "Config/real time clock";
  if (g == kGroupConfig && p == kParamTimezone) return "Config/timezone";
  if (g == kGroupStatus && p == kParamTimecode) return "Status/timecode";
  if (g == kGroupOutput && p == kParamTimecodeSource) return "Output/timecode source";
  if (g == kGroupMedia && p == kParamTransport) return "Media/transport mode";
  return nullptr;
}

}  // namespace

std::vector<std::string> describe(const std::vector<uint8_t>& data) {
  std::vector<std::string> lines;
  char buf[256];
  for (const Message& msg : parse_stream(data)) {
    Value v;
    if (!decode_value(msg, &v)) {
      std::snprintf(buf, sizeof buf, "dest=%u cmd=%u raw=%s", msg.dest, msg.cmd,
                    to_hex(msg.body).c_str());
      lines.push_back(buf);
      continue;
    }

    // The RTC echo is worth spelling out, because "did the value we sent come
    // back" is the whole question the probe modes exist to answer.
    if (v.group == kGroupConfig && v.param == kParamRtc && v.ints.size() >= 2) {
      std::string tc;
      const uint32_t t = static_cast<uint32_t>(v.ints[0]);
      if (!decode_bcd_timecode(t, &tc)) {
        std::snprintf(buf, sizeof buf, "?%08x", t);
        tc = buf;
      }
      std::snprintf(buf, sizeof buf, "RTC  time=%s  date=%08x", tc.c_str(),
                    static_cast<uint32_t>(v.ints[1]));
      lines.push_back(buf);
      continue;
    }

    std::string vals;
    for (size_t i = 0; i < v.ints.size(); ++i) {
      std::snprintf(buf, sizeof buf, "%s%lld", i ? ", " : "",
                    static_cast<long long>(v.ints[i]));
      vals += buf;
    }
    for (size_t i = 0; i < v.reals.size(); ++i) {
      std::snprintf(buf, sizeof buf, "%s%.4f", i ? ", " : "", v.reals[i]);
      vals += buf;
    }
    if (!v.text.empty()) vals += v.text;

    char label[64];
    const char* named = param_name(v.group, v.param);
    if (named) {
      std::snprintf(label, sizeof label, "%s", named);
    } else {
      std::snprintf(label, sizeof label, "%u.%u", v.group, v.param);
    }
    std::snprintf(buf, sizeof buf, "%-24s [%s] op=%u [%s]", label,
                  type_name(v.type), v.op, vals.c_str());
    lines.push_back(buf);
  }
  if (lines.empty()) {
    lines.push_back("raw=" + to_hex(data));
  }
  return lines;
}

}  // namespace bmd
}  // namespace octo
