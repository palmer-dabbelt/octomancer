#include "../src/timeutil.h"
#include "harness.h"

#include <cstdlib>
#include <ctime>

using namespace octo;

namespace {

// seconds_of_day_at_offset exists so a dongle can answer the question a Mac
// answers with a timezone database. The claim it rests on is that for a zone
// which is a fixed offset from UTC, the arithmetic and the C library agree
// exactly -- so pin it against the C library rather than against itself.
//
// Fixed-offset zones only, and spelled the POSIX way round: in TZ, "UTC+8"
// means eight hours *behind* UTC, which is the opposite of how everyone says
// it out loud and exactly the sort of sign error this test is here to catch.
void check_against_libc(const char* tz, int offset_east) {
  setenv("TZ", tz, 1);
  tzset();
  // A spread of instants, including one either side of a midnight in the
  // shifted zone, so a wrap that only works in the middle of the day fails.
  const double instants[] = {1756500000.0, 1756500000.0 + 43200.0,
                             1756500000.0 + 86399.0, 1756500000.0 + 86400.5,
                             0.5, 1e9 + 0.25};
  for (double t : instants) {
    CHECK_NEAR(seconds_of_day_at_offset(t, offset_east),
               local_seconds_of_day(t), 1e-6);
  }
}

}  // namespace

int main() {
  check_against_libc("UTC0", 0);
  check_against_libc("XXX8", -8 * 3600);        // eight hours behind UTC
  check_against_libc("XXX-5:30", 5 * 3600 + 1800);  // and five and a half ahead
  // Nothing below cares about the zone, but leaving a test's leftovers in the
  // environment is how a suite starts depending on the order it runs in.
  unsetenv("TZ");
  tzset();

  // The whole point of wrap_delta: a box one second past midnight is one
  // second ahead of a host at 23:59:59, not 86398 seconds behind it.
  CHECK_NEAR(wrap_delta(1.0 - 86399.0), 2.0, 1e-9);
  CHECK_NEAR(wrap_delta(86399.0 - 1.0), -2.0, 1e-9);
  CHECK_NEAR(wrap_delta(0.0), 0.0, 1e-9);
  CHECK_NEAR(wrap_delta(-6.231), -6.231, 1e-9);
  // Exactly antipodal is the one ambiguous case; keep it in range.
  CHECK(wrap_delta(43200.0) <= 43200.0);
  CHECK(wrap_delta(-43200.0) >= -43200.0);
  CHECK_NEAR(wrap_delta(86400.0), 0.0, 1e-9);

  CHECK_STR(format_sod(0.0), "00:00:00.000");
  CHECK_STR(format_sod(76783.187), "21:19:43.187");
  CHECK_STR(format_sod(86399.999), "23:59:59.999");

  CHECK_STR(format_age(4.2), "4s");
  CHECK_STR(format_age(192.0), "3m12s");
  CHECK_STR(format_age(7440.0), "2h04m");

  // Monotonic time must not be derived from the wall clock, or an NTP step
  // would show up as drift. They should not be equal.
  CHECK(mono_now() != wall_now());
  return octotest::report("test_timeutil");
}
