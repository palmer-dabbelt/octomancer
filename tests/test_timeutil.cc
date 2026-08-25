#include "../src/timeutil.h"
#include "harness.h"

using namespace octo;

int main() {
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
