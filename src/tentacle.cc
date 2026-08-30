#include "tentacle.h"

#include <cmath>

#include <cstdarg>
#include <cstdio>

namespace octo {

const char kFdacUuid[] = "0000fdac-0000-1000-8000-00805f9b34fb";

const char* resolution_name(Resolution r) {
  switch (r) {
    case Resolution::kFrame: return "frame";
    case Resolution::kFrameMicros: return "frame+us";
    case Resolution::kMicrosecond: return "microsecond";
    case Resolution::kNone: break;
  }
  return "none";
}

namespace {

std::string note_fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

std::string note_fmt(const char* fmt, ...) {
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return buf;
}

}  // namespace

Decoded decode(const uint8_t* data, size_t len) {
  Decoded out;
  if (data == nullptr || len == 0) {
    out.note = "empty";
    return out;
  }
  out.has_type = true;
  out.type = data[0];

  if (data[0] == kTypeTimecode && len >= 7) {
    const int h = data[3], m = data[4], s = data[5], f = data[6];
    // Range-check rather than trusting the layout. A box that violates these
    // bounds is telling us the format guess is wrong, and saying so is more
    // useful than emitting 61 minutes past the hour as though it were a fact.
    if (h > 23 || m > 59 || s > 59) {
      out.note = note_fmt("out of range: %d:%d:%d", h, m, s);
      return out;
    }
    const int fps = data[2] & 0x3F;
    if (fps < 1 || fps > 60) {
      out.note = note_fmt("implausible frame rate %d", fps);
      return out;
    }

    double sod = h * 3600.0 + m * 60.0 + s + f / static_cast<double>(fps);
    out.fps = fps;
    out.flags = data[1];
    out.frames = f;
    out.resolution = Resolution::kFrame;

    char buf[48];
    // Bytes 7-8 are microseconds within the frame. Including them takes this
    // from frame resolution (~42 ms at 24 fps) to a few ms: fitting decoded
    // time against the host clock, the residual drops 3-18x on every box
    // tested. There is an unexplained ~3.6 ms floor in the field -- its
    // observed minimum is ~3600, not 0 -- so the absolute value carries a
    // small constant bias. The rate is what matters for sync, and the rate is
    // unaffected by a constant.
    if (len >= 9) {
      const uint32_t micros =
          static_cast<uint32_t>(data[7]) << 8 | static_cast<uint32_t>(data[8]);
      out.has_micros = true;
      out.micros = micros;
      out.resolution = Resolution::kFrameMicros;
      sod += micros / 1e6;
      std::snprintf(buf, sizeof buf, "%02d:%02d:%02d:%02d.%03d", h, m, s, f,
                    static_cast<int>(micros / 1000));
    } else {
      std::snprintf(buf, sizeof buf, "%02d:%02d:%02d:%02d", h, m, s, f);
    }
    out.display = buf;
    out.sod = sod;
    out.ok = true;
    return out;
  }

  if (data[0] == kTypeMicros && len >= 8) {
    uint64_t micros = 0;
    for (int i = 3; i < 8; ++i) micros = (micros << 8) | data[i];
    const double sod = static_cast<double>(micros) / 1e6;
    if (sod >= 86400.0) {
      out.note = note_fmt("counter out of day range (%llu)",
                          static_cast<unsigned long long>(micros));
      return out;
    }
    out.flags = data[1];
    out.sod = sod;
    out.resolution = Resolution::kMicrosecond;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%06.3f", static_cast<int>(sod / 3600),
                  static_cast<int>(sod / 60) % 60, sod - 60.0 * static_cast<int>(sod / 60));
    out.display = buf;
    out.ok = true;
    return out;
  }

  if (data[0] == kTypeStatic) {
    out.note = "static/info packet";
    return out;
  }

  out.note = note_fmt("unknown type 0x%02x (%zuB)", data[0], len);
  return out;
}

Decoded decode(const std::vector<uint8_t>& data) {
  return decode(data.data(), data.size());
}

namespace {

// Seconds since midnight, wrapped into a day. A fake box that has drifted past
// midnight is a case somebody will want to arrange on purpose.
double wrap_day(double sod) {
  const double day = 86400.0;
  sod = std::fmod(sod, day);
  if (sod < 0.0) sod += day;
  return sod;
}

}  // namespace

std::vector<uint8_t> encode_timecode(double sod, int fps, bool sub_frame,
                                     uint8_t flags) {
  if (fps < 1) fps = 1;
  if (fps > 60) fps = 60;
  sod = wrap_day(sod);

  const int whole = static_cast<int>(std::floor(sod));
  const double frac = sod - whole;
  // The epsilon is not a fudge. A frame boundary is a ratio -- 7/24 -- and
  // multiplying it back by 24 lands a hair under 7 in binary floating point,
  // so a plain truncation reports frame 6 for a time that is exactly frame 7.
  int frames = static_cast<int>(frac * fps + 1e-9);
  if (frames >= fps) frames = fps - 1;

  std::vector<uint8_t> out;
  out.push_back(kTypeTimecode);
  out.push_back(flags);
  out.push_back(static_cast<uint8_t>(fps & 0x3F));
  out.push_back(static_cast<uint8_t>(whole / 3600));
  out.push_back(static_cast<uint8_t>((whole / 60) % 60));
  out.push_back(static_cast<uint8_t>(whole % 60));
  out.push_back(static_cast<uint8_t>(frames));
  if (!sub_frame) return out;  // seven bytes: frame resolution only

  // Whatever the frame number could not express. The decoder *adds* this to
  // the frame time rather than replacing it, so it is the remainder and not
  // the fraction -- writing the whole fraction here counts it twice and puts
  // the box up to a frame ahead of where it was asked to be.
  double remainder = frac - static_cast<double>(frames) / fps;
  if (remainder < 0.0) remainder = 0.0;
  uint32_t us = static_cast<uint32_t>(remainder * 1e6 + 0.5);
  // Only reachable below about 16 fps, where one frame is longer than the
  // field can count. No real box is down there.
  if (us > 0xFFFF) us = 0xFFFF;
  out.push_back(static_cast<uint8_t>(us >> 8));
  out.push_back(static_cast<uint8_t>(us & 0xFF));
  return out;
}

std::vector<uint8_t> encode_micros(double sod, uint8_t flags) {
  sod = wrap_day(sod);
  uint64_t us = static_cast<uint64_t>(sod * 1e6 + 0.5);
  if (us >= 86400ull * 1000000ull) us = 86400ull * 1000000ull - 1;

  std::vector<uint8_t> out;
  out.push_back(kTypeMicros);
  out.push_back(flags);
  out.push_back(0x00);  // byte 2 carries no frame rate in this layout
  for (int shift = 32; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((us >> shift) & 0xFF));
  }
  return out;
}

std::vector<uint8_t> encode_static() {
  return std::vector<uint8_t>{kTypeStatic, 0x3d, 0x00, 0x00,
                              0x00,        0x00, 0x00, 0x00, 0x00};
}

std::string to_hex(const uint8_t* data, size_t len) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 0x0F]);
  }
  return out;
}

std::string to_hex(const std::vector<uint8_t>& data) {
  return to_hex(data.data(), data.size());
}

}  // namespace octo
