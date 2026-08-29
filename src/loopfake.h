// A loop whose clock is a variable.
//
// This is the reason the event loop was worth abstracting at all. With time as
// a number the caller sets, a test can drive an hour of drift, a missed sync
// window or a camera that never answers, and do it in no wall-clock time and
// with no radio attached. The alternative -- sleeping in tests -- produces
// suites that are slow when they pass and flaky when they fail.
//
// It lives in the library rather than in tests/ on purpose. Replaying a
// captured HCI log through the real host, at whatever speed, is a debugging
// tool this project has wished for more than once; see the Zoom investigation
// in doc/zoom-bta1-notes.md, which was slow precisely because there was no way
// to re-run what had happened.
#ifndef OCTO_LOOPFAKE_H
#define OCTO_LOOPFAKE_H

#include <deque>
#include <memory>
#include <vector>

#include "loop.h"

namespace octo {

class FakeLoop : public Loop {
 public:
  explicit FakeLoop(double start = 1000.0);
  ~FakeLoop() override;

  // A handle for a source that is not a file descriptor. Readiness is
  // whatever the test says it is.
  Handle handle() { return Handle{-1, this}; }

  // Make `id` ready `in` seconds from now. The loop will not advance its clock
  // past this without delivering it.
  void deliver(SourceId id, int interest, double in = 0.0);

  // Report `id` as failed `in` seconds from now.
  void fail(SourceId id, double in = 0.0);

  // Run ticks until the clock has moved forward by `seconds`. Timers due in
  // that span fire, in order, at their own deadlines rather than all at the
  // end -- which is the whole point of a fake clock and is what lets a test
  // assert on the interleaving.
  void advance(double seconds);

  // Move the clock forward without dispatching anything. This is how a test
  // says "the handler ran long": call it from inside a handler and the loop
  // sees exactly what it would have seen if the work had really taken that
  // long. Nothing else can express that, because everything else in this
  // class advances time only in order to deliver something.
  void jump(double seconds) { now_ += seconds; }

  // How many times the loop has waited. A test that asserts a predicate was
  // already true checks this is zero.
  int waits() const { return waits_; }

 protected:
  double clock() const override { return now_; }
  void wait(double max_wait, std::vector<std::pair<SourceId, int>>* ready,
            std::vector<SourceId>* failed) override;
  void wake_backend() override { woken_ = true; }

 private:
  struct Pending {
    double at = 0.0;
    SourceId id = kNoSource;
    int interest = 0;
    bool is_failure = false;
  };

  double now_ = 0.0;
  int waits_ = 0;
  bool woken_ = false;
  std::deque<Pending> pending_;
};

}  // namespace octo

#endif  // OCTO_LOOPFAKE_H
