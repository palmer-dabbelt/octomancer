// The judgement half of the watchdog, which is the half that can be wrong.
//
// A false positive here reboots a working dongle in the middle of a shoot, and
// a false negative is decoration on a box that has already stopped. Both are
// cheap to check with a clock that is a variable, and neither is checkable at
// all on hardware without waiting for a real fault.
#include "../src/watchdog.h"

#include <string>

#include "harness.h"

using namespace octo;

namespace {

void test_every_check_passing_feeds_every_channel() {
  WatchdogPolicy policy;
  CHECK(policy.watch("loop", []() { return true; }));
  CHECK(policy.watch("wire", []() { return true; }));
  CHECK_EQ(static_cast<int>(policy.size()), 2);

  CHECK_EQ(static_cast<long long>(policy.poll(100.0)), 3LL);  // both bits
  CHECK_STR(policy.worst(100.0), "");
}

// The point of having more than one channel: a check that stops holding leaves
// its own channel hungry and nothing else. The hardware resets because that one
// register went unfed, so one stuck thread is enough even when the rest of the
// box is perfectly lively.
void test_one_failing_check_leaves_only_its_own_channel_hungry() {
  bool wire = true;
  WatchdogPolicy policy;
  policy.watch("loop", []() { return true; });
  policy.watch("wire", [&wire]() { return wire; });

  CHECK_EQ(static_cast<long long>(policy.poll(100.0)), 3LL);
  wire = false;
  CHECK_EQ(static_cast<long long>(policy.poll(101.0)), 1LL);  // loop only
  CHECK_STR(policy.worst(101.0), "wire");
}

void test_how_long_a_check_has_been_failing_is_measured_from_the_first_one() {
  bool ok = true;
  WatchdogPolicy policy;
  policy.watch("wire", [&ok]() { return ok; });

  policy.poll(100.0);
  CHECK_NEAR(policy.failing_for(0, 100.0), 0.0, 1e-9);

  ok = false;
  policy.poll(101.0);
  policy.poll(102.0);
  policy.poll(103.0);
  // Three failures, but two seconds: the question is how long it has been
  // stuck, not how many times it was asked.
  CHECK_NEAR(policy.failing_for(0, 103.0), 2.0, 1e-9);
}

// A check that fails, recovers and fails again is not accumulating towards a
// reset. A watchdog is about the machine being stuck now, not about it having
// had a bad afternoon.
void test_recovery_resets_the_clock() {
  bool ok = true;
  WatchdogPolicy policy;
  policy.watch("wire", [&ok]() { return ok; });

  policy.poll(100.0);
  ok = false;
  policy.poll(101.0);
  CHECK_NEAR(policy.failing_for(0, 105.0), 4.0, 1e-9);

  ok = true;
  policy.poll(106.0);
  CHECK_NEAR(policy.failing_for(0, 106.0), 0.0, 1e-9);
  CHECK_STR(policy.worst(106.0), "");

  ok = false;
  policy.poll(107.0);
  CHECK_NEAR(policy.failing_for(0, 108.0), 1.0, 1e-9);  // not six
}

void test_the_worst_check_is_the_one_stuck_longest() {
  bool a = true, b = true;
  WatchdogPolicy policy;
  policy.watch("first", [&a]() { return a; });
  policy.watch("second", [&b]() { return b; });

  b = false;
  policy.poll(100.0);
  a = false;
  policy.poll(105.0);
  CHECK_STR(policy.worst(110.0), "second");
}

void test_more_checks_than_channels_is_refused_rather_than_ignored() {
  WatchdogPolicy policy;
  for (size_t i = 0; i < WatchdogPolicy::kMaxChecks; ++i) {
    CHECK(policy.watch("c", []() { return true; }));
  }
  // The ninth would be a condition nobody is checking, which is worse than not
  // having asked for it.
  CHECK(!policy.watch("overflow", []() { return true; }));
}

// ------------------------------------------------------- the idle-thread case

// Something that legitimately does nothing for minutes cannot be watched by
// "has it run lately": the answer is no, and that is correct. So it gets asked.
void test_an_idle_thread_is_poked_and_answers() {
  ProbeLiveness probe(5.0, 2.0);
  uint32_t ticks = 0;
  bool poke = false;

  CHECK(probe.poll(100.0, ticks, &poke));
  CHECK(!poke);  // the first call is a baseline, not a question

  // Nothing asked until the period is up, however idle it is.
  CHECK(probe.poll(104.0, ticks, &poke));
  CHECK(!poke);

  CHECK(probe.poll(105.0, ticks, &poke));
  CHECK(poke);

  // It runs. Whatever it did, the counter moved, which is the whole answer.
  ++ticks;
  CHECK(probe.poll(105.5, ticks, &poke));
  CHECK(!poke);
}

// Busy is not stuck. A workqueue that takes a moment to get to us has not
// failed, and rebooting the box over it would be the false positive that
// matters most.
void test_a_slow_answer_is_not_a_failure() {
  ProbeLiveness probe(5.0, 2.0);
  uint32_t ticks = 0;
  bool poke = false;

  probe.poll(100.0, ticks, &poke);
  probe.poll(105.0, ticks, &poke);
  CHECK(poke);

  CHECK(probe.poll(106.0, ticks, &poke));  // one second late: fine
  CHECK(probe.poll(106.9, ticks, &poke));  // still within patience
  ++ticks;
  CHECK(probe.poll(107.0, ticks, &poke));
}

// ...and no answer at all, for longer than anyone would wait, is.
void test_an_unanswered_poke_eventually_fails() {
  ProbeLiveness probe(5.0, 2.0);
  uint32_t ticks = 7;
  bool poke = false;

  probe.poll(100.0, ticks, &poke);
  probe.poll(105.0, ticks, &poke);
  CHECK(poke);

  CHECK(probe.poll(106.5, ticks, &poke));
  CHECK(!probe.poll(107.5, ticks, &poke));
  CHECK(!probe.poll(120.0, ticks, &poke));

  // And it recovers if the thing comes back, rather than staying condemned.
  ++ticks;
  CHECK(probe.poll(121.0, ticks, &poke));
}

}  // namespace

int main() {
  test_every_check_passing_feeds_every_channel();
  test_one_failing_check_leaves_only_its_own_channel_hungry();
  test_how_long_a_check_has_been_failing_is_measured_from_the_first_one();
  test_recovery_resets_the_clock();
  test_the_worst_check_is_the_one_stuck_longest();
  test_more_checks_than_channels_is_refused_rather_than_ignored();
  test_an_idle_thread_is_poked_and_answers();
  test_a_slow_answer_is_not_a_failure();
  test_an_unanswered_poke_eventually_fails();
  return octotest::report("test_watchdog");
}
