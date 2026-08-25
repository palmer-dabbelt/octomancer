// Clock arithmetic shared by the daemon, the control tool and the UI.
//
// Two clocks are in play and they are not interchangeable. Wall time answers
// "what time is it", and only wall time can be compared against a Tentacle,
// which broadcasts a local time of day. Monotonic time answers "how long since"
// and is the only safe basis for ages, rates and drift, because it does not
// step when NTP corrects the host or when daylight saving moves.
#ifndef OCTO_TIMEUTIL_H
#define OCTO_TIMEUTIL_H

#include <chrono>
#include <string>

namespace octo {

// Seconds on a monotonic clock, as a double. Origin is arbitrary and means
// nothing across processes; only differences are meaningful.
double mono_now();

// Seconds since the Unix epoch, including a fraction.
double wall_now();

// Seconds since local midnight, including a fraction. Tentacle boxes carry a
// local time of day, so this is what their timecode is comparable against.
double local_seconds_of_day(double unix_seconds);

// Shortest signed distance around a 24-hour clock, in (-43200, +43200].
// Without this, a box one second past midnight looks 86399 seconds fast.
double wrap_delta(double seconds);

// "HH:MM:SS.mmm" for a seconds-since-midnight value.
std::string format_sod(double sod);

// A duration a human reads at a glance: "4s", "3m12s", "2h04m".
std::string format_age(double seconds);

// Local ISO-8601 to milliseconds, for log lines.
std::string format_local(double unix_seconds);

}  // namespace octo

#endif  // OCTO_TIMEUTIL_H
