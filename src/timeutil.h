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

// Whether a wall-clock reading is a reading at all.
//
// A box has no wall clock until a host tells it the time -- src/loop.h, and
// firmware/src/boxclock.h, which answers zero rather than loop time until then
// so that "unknown" cannot be mistaken for a plausible date in 1970. Anything
// that subtracts a wall clock from something has to ask this first: an offset
// taken against the missing one is not approximately right, it is nonsense
// with a confident number attached, and nothing downstream can tell the two
// apart.
//
// The cut is generous on purpose. Real timestamps on any machine this runs on
// are around 1.7e9, so no plausible reading is anywhere near it, and a host
// whose clock genuinely reads 1970 has a problem this code cannot fix anyway.
inline bool wall_known(double unix_seconds) { return unix_seconds > 1e6; }

// Seconds since local midnight, including a fraction. Tentacle boxes carry a
// local time of day, so this is what their timecode is comparable against.
double local_seconds_of_day(double unix_seconds);

// Seconds since local midnight for a zone that is a fixed offset from UTC.
//
// The same answer as local_seconds_of_day, for a machine that has no timezone
// database to get it from. A dongle has none: picolibc links a localtime_r
// that reads a TZ nobody sets, so it answers UTC, and a box comparing a
// Tentacle's local time of day against UTC is wrong by the whole offset --
// seven hours here, and confidently so.
//
// Fixed offset only. No box is going to observe a daylight-saving transition
// mid-shoot without a host noticing, and the host is on the other end of the
// cable saying what the offset is right now.
double seconds_of_day_at_offset(double unix_seconds, int zone_offset_seconds);

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
