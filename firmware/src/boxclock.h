// What time the box thinks it is, and whether that is a real time.
//
// The box has no wall clock and no way to acquire one alone: no network, no
// battery-backed RTC, no user to ask. src/loop.h already says so -- "on the
// box there is no wall clock at boot at all".
//
// What matters is that this does not stop it working. An offset is a
// difference, and a difference does not need either side to be a real time --
// it needs both sides measured against the *same* thing. So the clock runs
// from zero at boot and the box starts measuring immediately: the offsets it
// reports are all shifted by one unknown constant, and every difference
// between them -- which is to say the spread across the bench, the thing the
// box exists to measure -- is exact. A dongle in a bag with no computer
// attached is doing useful work from the moment it powers up.
//
// What the constant costs is the *absolute* question: "is this box right?"
// rather than "do these boxes agree?". That answer needs a real clock, and
// until a host provides one this says so through known(), so that nothing
// downstream reports a relative number as though it were an absolute one.
// See Snapshot::wall_is_real, which is where that travels.
//
// Monotonic time comes from the loop and is trustworthy from the first
// instant; wall time is that plus a constant, and the constant is zero until
// somebody says otherwise.
//
// Deliberately not smoothed or slewed. A host that corrects the box by a
// second should move it by a second: this clock is a shared reference for
// comparing two radios, not a timebase anything is disciplined against, and a
// filter here would put an invented lag between the two halves of the
// measurement.
#ifndef OCTO_FW_BOXCLOCK_H
#define OCTO_FW_BOXCLOCK_H

#include "loop.h"

namespace octo {

class BoxClock {
 public:
  explicit BoxClock(Loop* loop) : loop_(loop) {}

  // Whether this is a real time or a free-running reference.
  //
  // False until a host says. Worth asking rather than assuming, but not worth
  // *waiting* for: an offset measured against the free-running clock is a
  // perfectly good offset, it is only the absolute reading that is missing.
  bool known() const { return known_; }

  void set(double unix_seconds) {
    offset_ = unix_seconds - loop_->now();
    known_ = true;
  }

  // Seconds east of UTC, as the host reported them.
  //
  // Separate from the instant because it answers a separate question, and the
  // box cannot answer it alone: there is no timezone database here, and a
  // Tentacle broadcasts a *local* time of day. Comparing that against UTC is
  // not a rounding error, it is the whole offset -- seven hours on this bench,
  // which reads as a spectacularly broken box rather than a missing field.
  //
  // Zero until a host says, and zero is also a perfectly good answer (a bench
  // in London in winter), so `zone_known()` is the question to ask.
  void set_zone(int seconds_east) {
    zone_ = seconds_east;
    zone_known_ = true;
  }
  bool zone_known() const { return zone_known_; }
  int zone() const { return zone_; }

  // Loop time plus whatever the host said, which before a host says anything
  // is loop time -- a clock that reads a few seconds past the epoch and keeps
  // honest time from there.
  //
  // This deliberately does not return zero to mean "unknown". A caller reading
  // a small number cannot tell it from a real reading, so the two would blur
  // whichever value was picked; known() is what separates them, and it is not
  // optional. What the small number buys is that everything measured against
  // it is measured against the same thing, which is all an offset needs.
  double wall() const { return loop_->now() + offset_; }

  // For a reading timestamped when it arrived rather than when it was
  // processed. An advertisement can sit in a queue for tens of milliseconds,
  // and charging the box for that wait is the same mistake syncd.cc's
  // error_from() exists to avoid.
  double wall_at(double mono) const { return mono + offset_; }

 private:
  Loop* loop_ = nullptr;
  bool known_ = false;
  double offset_ = 0.0;
  bool zone_known_ = false;
  int zone_ = 0;
};

// Make this the clock the C library answers from.
//
// std::chrono::system_clock -- which is what src/timeutil.cc's wall_now() is
// built on -- links against gettimeofday, so the symbol has to exist on this
// target whether or not anything calls it. Zephyr can supply one, from a
// system clock nothing on this device ever sets; that links, and then
// wall_now() quietly answers 1970 for the rest of the program's life.
//
// Pointing the C library at this clock instead costs a global and removes the
// trap. There is exactly one wall clock on a dongle, every part of the program
// should agree about what it says, and a caller that reaches for the ordinary
// portable spelling gets the right answer rather than a plausible wrong one.
//
// Call once, from main(), before anything can ask. Passing null puts it back.
void install_box_clock(BoxClock* clock);

}  // namespace octo

#endif  // OCTO_FW_BOXCLOCK_H
