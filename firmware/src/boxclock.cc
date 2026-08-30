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
