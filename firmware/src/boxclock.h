// What time the box thinks it is.
//
// The box has no wall clock and no way to acquire one alone: no network, no
// battery-backed RTC, no user to ask. src/loop.h already says so -- "on the
// box there is no wall clock at boot at all" -- and everything that matters
// here is measured against one. A Tentacle broadcasts a local time of day, and
// an offset is that time minus ours; without a shared idea of "ours" the two
// radios in doc/box-notes.md would report offsets that could not be compared,
// which is the entire experiment.
//
// So the host tells it, over the same cable as everything else, and this holds
// the answer. Monotonic time comes from the loop and is trustworthy from the
// first instant; wall time is that plus a constant, and the constant is
// unknown until somebody says.
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

  // False until the host has said. Worth asking rather than assuming: an
  // offset computed against an unset clock is not a small error, it is a
  // number about 1970.
  bool known() const { return known_; }

  void set(double unix_seconds) {
    offset_ = unix_seconds - loop_->now();
    known_ = true;
  }

  // Zero when unknown, which is what src/timeutil.h's callers already treat as
  // "no wall clock": returning loop time instead would look like a plausible
  // date in 1970 and be silently wrong.
  double wall() const { return known_ ? loop_->now() + offset_ : 0.0; }

  // For a reading timestamped when it arrived rather than when it was
  // processed. An advertisement can sit in a queue for tens of milliseconds,
  // and charging the box for that wait is the same mistake syncd.cc's
  // error_from() exists to avoid.
  double wall_at(double mono) const { return known_ ? mono + offset_ : 0.0; }

 private:
  Loop* loop_ = nullptr;
  bool known_ = false;
  double offset_ = 0.0;
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
