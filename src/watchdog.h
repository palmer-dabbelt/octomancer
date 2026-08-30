// The judgement half of a hardware watchdog.
//
// A watchdog is two pieces: a timer in silicon that resets the machine, and a
// decision about what "still working" means. The first is four register writes
// and lives in firmware/src/hwwatchdog.cc. The second is this, and it is the
// half that can be wrong -- a check that is too eager reboots a working dongle,
// and one that is too generous is decoration.
//
// The shape comes from the hardware. The nRF52840 watchdog has eight reload
// registers and reloads only when *every* allocated one has been fed, which is
// exactly the primitive for "several separate things must all still be alive".
// So a check here is a channel there, and the machine resets when any single
// check stops holding -- not when all of them do.
//
// Why more than one is needed: firmware/src/cdcpeer.cc's handler does not run
// on the loop. Zephyr's CDC ACM calls it from a workqueue, so "the loop is
// going round" and "the USB side is going round" are two different claims and
// a box can lose either one alone. The first evening this firmware ran ended
// with a dongle that still enumerated, still blinked, and would not open --
// which is what losing only the second looks like, and no single-threaded
// heartbeat would have caught it.
#ifndef OCTO_WATCHDOG_H
#define OCTO_WATCHDOG_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace octo {

class WatchdogPolicy {
 public:
  // Must keep returning true. Called from the loop, so it may not block and
  // may not be expensive: this runs several times a second forever.
  using Check = std::function<bool()>;

  // Eight reload registers on this part, so eight conditions. Nothing is
  // close to needing that many; the limit is here so exceeding it is a
  // compile-time-ish error rather than a channel that silently never feeds.
  static constexpr size_t kMaxChecks = 8;

  // Returns false if there is no channel left for it.
  bool watch(std::string name, Check check);

  size_t size() const { return checks_.size(); }
  const std::string& name(size_t index) const;

  // Runs every check and returns a bitmask of the ones that passed. The caller
  // feeds exactly those channels and leaves the rest hungry.
  uint32_t poll(double now);

  // How long a check has been failing continuously, in seconds. Zero when it
  // is passing. For a reset to be able to say more than "something stopped".
  double failing_for(size_t index, double now) const;

  // The name of whichever check has been failing longest, or empty when all
  // is well. This is what the box tells the next host it sees.
  std::string worst(double now) const;

 private:
  struct Entry {
    std::string name;
    Check check;
    bool failing = false;
    double since = 0.0;
  };
  std::vector<Entry> checks_;
};

// Whether something that is idle by design is still capable of running.
//
// The awkward case behind this: a thread that legitimately does nothing for
// minutes cannot be watched by "has it run lately", because the answer is no
// and that is correct. So ask it to. Poke it on a period, and require that it
// have run within a patience afterwards -- which distinguishes "nothing to do"
// from "cannot do anything", and those are the two states that look identical
// from outside.
class ProbeLiveness {
 public:
  // `period` between pokes, `patience` for an answer. Patience should be
  // comfortably shorter than the watchdog's own timeout, or this never gets to
  // report anything before the machine resets for a different reason.
  ProbeLiveness(double period, double patience);

  // `ticks` is a counter the watched thing increments whenever it runs; it
  // does not matter by how much, only that it moves. Sets *poke when the
  // caller should provoke it. Returns false once a poke has gone unanswered
  // for longer than the patience.
  bool poll(double now, uint32_t ticks, bool* poke);

 private:
  double period_ = 0.0;
  double patience_ = 0.0;
  bool started_ = false;
  bool waiting_ = false;
  double last_poke_ = 0.0;
  uint32_t ticks_at_poke_ = 0;
};

}  // namespace octo

#endif  // OCTO_WATCHDOG_H
