#include "loopfake.h"

namespace octo {

FakeLoop::FakeLoop(double start) : now_(start) {}
FakeLoop::~FakeLoop() = default;

void FakeLoop::deliver(SourceId id, int interest, double in) {
  Pending p;
  p.at = now_ + in;
  p.id = id;
  p.interest = interest;
  pending_.push_back(p);
}

void FakeLoop::fail(SourceId id, double in) {
  Pending p;
  p.at = now_ + in;
  p.id = id;
  p.is_failure = true;
  pending_.push_back(p);
}

void FakeLoop::wait(double max_wait, std::vector<std::pair<SourceId, int>>* ready,
                    std::vector<SourceId>* failed) {
  ++waits_;

  // A wake is a request to stop waiting, not a reason to move time.
  if (woken_) {
    woken_ = false;
    return;
  }

  // The earliest thing the test has queued.
  double soonest = -1.0;
  for (const auto& p : pending_) {
    if (soonest < 0.0 || p.at < soonest) soonest = p.at;
  }

  const bool bounded = max_wait >= 0.0;
  const double limit = bounded ? now_ + max_wait : -1.0;

  if (soonest >= 0.0 && (!bounded || soonest <= limit)) {
    // Something arrives before the deadline: move to it and deliver
    // everything due at that instant.
    if (soonest > now_) now_ = soonest;
  } else if (bounded) {
    // Nothing arrives. The wait expires, which is what lets a timer be due.
    now_ = limit;
  } else {
    // No deadline and nothing queued: a real loop would block here forever.
    // Returning without moving is the honest answer -- the test has asked to
    // wait for something that cannot happen, and it will see waits() climb
    // rather than hang.
    return;
  }

  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->at <= now_) {
      if (it->is_failure) {
        failed->push_back(it->id);
      } else {
        ready->emplace_back(it->id, it->interest);
      }
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
}

void FakeLoop::advance(double seconds) {
  const double until = now_ + seconds;
  // tick() caps its own wait at the next timer, so handing it the whole
  // remaining span each time is what makes timers fire at their deadlines
  // instead of all together at the end.
  while (running() && now_ < until) {
    tick(until - now_);
  }
}

}  // namespace octo
