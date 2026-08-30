// See firmware/src/boxclock.h -- specifically install_box_clock, which is the
// only reason this file is not header-only.
#include "boxclock.h"

#include <sys/time.h>

#include <ctime>

namespace octo {
namespace {

// One antenna, one cable, one clock. A registry of them would be ceremony for
// a device that cannot have two.
BoxClock* g_clock = nullptr;

}  // namespace

void install_box_clock(BoxClock* clock) { g_clock = clock; }

}  // namespace octo

// The C library's, answered from the clock the host gave us.
//
// Zephyr will supply this if CONFIG_XSI_SINGLE_PROCESS is on, and it is
// deliberately left off: that one reads a system clock nothing on this device
// ever sets, so it would link and then answer 1970 forever. See the header.
//
// Before the host has said anything this returns zero rather than uptime,
// because zero is what src/timeutil.h's callers already read as "no wall
// clock". Uptime would be a small, plausible, wrong number.
extern "C" int gettimeofday(struct timeval* tv, void* tz) {
  (void)tz;
  if (tv == nullptr) return -1;
  const double now = octo::g_clock != nullptr ? octo::g_clock->wall() : 0.0;
  tv->tv_sec = static_cast<time_t>(now);
  tv->tv_usec =
      static_cast<suseconds_t>((now - static_cast<double>(tv->tv_sec)) * 1e6);
  return 0;
}

// The C library's, answered from the zone the host gave us.
//
// picolibc links a real localtime_r, and it works: it reads TZ, finds nothing,
// and answers UTC. That is the trap. src/registry.cc compares a Tentacle's
// *local* time of day against local_seconds_of_day(), so a box quietly working
// in UTC reports every box on the bench as seven hours out -- a confident
// number, in the right units, produced by code that did exactly what it was
// told. Nothing downstream can tell it from a real reading.
//
// A fixed offset is the whole of what a zone can mean here, and it is enough:
// the host is on the other end of the cable and says what the offset is now.
// So shift the instant and let picolibc do the calendar, which is the part
// worth not writing twice.
//
// Before a host has said, this is UTC -- but the clock is unknown then too, so
// src/registry.cc has already declined to compute an offset at all. See
// wall_known() in src/timeutil.h.
extern "C" struct tm* localtime_r(const time_t* t, struct tm* out) {
  if (t == nullptr || out == nullptr) return nullptr;
  const int zone =
      octo::g_clock != nullptr && octo::g_clock->zone_known()
          ? octo::g_clock->zone()
          : 0;
  const time_t shifted = *t + zone;
  if (::gmtime_r(&shifted, out) == nullptr) return nullptr;
  out->tm_isdst = 0;
  return out;
}
