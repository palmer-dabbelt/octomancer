#include "timeutil.h"

#include <cmath>
#include <cstdio>
#include <ctime>

namespace octo {

double mono_now() {
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration<double>(now).count();
}

double wall_now() {
  auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration<double>(now).count();
}

double local_seconds_of_day(double unix_seconds) {
  double whole = std::floor(unix_seconds);
  double frac = unix_seconds - whole;
  std::time_t secs = static_cast<std::time_t>(whole);
  struct tm parts;
  localtime_r(&secs, &parts);
  return parts.tm_hour * 3600.0 + parts.tm_min * 60.0 + parts.tm_sec + frac;
}

double seconds_of_day_at_offset(double unix_seconds, int zone_offset_seconds) {
  const double local = unix_seconds + zone_offset_seconds;
  double sod = std::fmod(local, 86400.0);
  if (sod < 0.0) sod += 86400.0;  // fmod keeps the sign of the dividend
  return sod;
}

double wrap_delta(double seconds) {
  double wrapped = std::fmod(seconds + 43200.0, 86400.0);
  if (wrapped < 0.0) wrapped += 86400.0;  // fmod keeps the sign of the dividend
  return wrapped - 43200.0;
}

std::string format_sod(double sod) {
  if (!std::isfinite(sod)) return "--:--:--.---";
  double day = std::fmod(sod, 86400.0);
  if (day < 0.0) day += 86400.0;
  int h = static_cast<int>(day / 3600.0);
  int m = static_cast<int>(std::fmod(day / 60.0, 60.0));
  double s = std::fmod(day, 60.0);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d:%02d:%06.3f", h, m, s);
  return buf;
}

std::string format_age(double seconds) {
  char buf[32];
  if (!std::isfinite(seconds) || seconds < 0.0) return "-";
  if (seconds < 60.0) {
    std::snprintf(buf, sizeof buf, "%.0fs", seconds);
  } else if (seconds < 3600.0) {
    std::snprintf(buf, sizeof buf, "%dm%02ds", static_cast<int>(seconds / 60),
                  static_cast<int>(std::fmod(seconds, 60.0)));
  } else {
    std::snprintf(buf, sizeof buf, "%dh%02dm", static_cast<int>(seconds / 3600),
                  static_cast<int>(std::fmod(seconds / 60.0, 60.0)));
  }
  return buf;
}

std::string format_local(double unix_seconds) {
  double whole = std::floor(unix_seconds);
  std::time_t secs = static_cast<std::time_t>(whole);
  struct tm parts;
  localtime_r(&secs, &parts);
  char buf[64];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
                parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                parts.tm_hour, parts.tm_min, parts.tm_sec,
                static_cast<int>((unix_seconds - whole) * 1000.0));
  return buf;
}

}  // namespace octo
