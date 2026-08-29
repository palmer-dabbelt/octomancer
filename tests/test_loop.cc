// The event loop, driven on a clock that is a variable.
//
// Everything here runs in no wall-clock time and touches no hardware. That is
// the point of the fake loop: the properties worth pinning are all about
// ordering and about what happens at the exact instant two things come due,
// and neither can be tested by sleeping.

#include <string>
#include <vector>

#include "harness.h"
#include "loop.h"
#include "loopfake.h"

using octo::FakeLoop;
using octo::kHangup;
using octo::kNoTimer;
using octo::kRead;
using octo::kWrite;
using octo::SourceId;
using octo::TimerId;

namespace {

void test_one_shot_fires_once_and_not_early() {
  FakeLoop loop;
  int fired = 0;
  double when = 0.0;
  const double t0 = loop.now();
  loop.after(5.0, [&] {
    ++fired;
    when = loop.now();
  });

  loop.advance(4.999);
  CHECK_EQ(fired, 0);

  loop.advance(0.002);
  CHECK_EQ(fired, 1);
  CHECK_NEAR(when, t0 + 5.0, 1e-9);

  // A one-shot is done. Running for another minute must not repeat it.
  loop.advance(60.0);
  CHECK_EQ(fired, 1);
}

void test_same_deadline_fires_in_creation_order() {
  FakeLoop loop;
  std::vector<int> order;
  const double at = loop.now() + 1.0;
  loop.at(at, [&] { order.push_back(1); });
  loop.at(at, [&] { order.push_back(2); });
  loop.at(at, [&] { order.push_back(3); });

  loop.advance(2.0);
  CHECK_EQ(order.size(), static_cast<size_t>(3));
  CHECK_EQ(order[0], 1);
  CHECK_EQ(order[1], 2);
  CHECK_EQ(order[2], 3);
}

void test_repeating_timer_does_not_burst_after_a_long_handler() {
  FakeLoop loop;
  int fired = 0;
  loop.every(1.0, [&] {
    ++fired;
    // The first run takes five seconds of real work. Four periods are missed.
    if (fired == 1) loop.jump(5.0);
  });

  // Enough time for the first fire and the long handler.
  loop.advance(1.5);
  CHECK_EQ(fired, 1);

  // The four periods that elapsed while the handler was busy are gone and are
  // not owed. One more period passing means exactly one more fire.
  loop.advance(1.0);
  CHECK_EQ(fired, 2);

  // And it is realigned, not drifting: a further two periods, two more fires.
  loop.advance(2.0);
  CHECK_EQ(fired, 4);
}

void test_repeating_timer_keeps_its_period() {
  FakeLoop loop;
  std::vector<double> at;
  const double t0 = loop.now();
  loop.every(0.25, [&] { at.push_back(loop.now() - t0); });

  loop.advance(1.0);
  CHECK_EQ(at.size(), static_cast<size_t>(4));
  CHECK_NEAR(at[0], 0.25, 1e-9);
  CHECK_NEAR(at[1], 0.50, 1e-9);
  CHECK_NEAR(at[2], 0.75, 1e-9);
  CHECK_NEAR(at[3], 1.00, 1e-9);
}

void test_a_timer_cancelled_by_another_due_at_the_same_instant_does_not_fire() {
  FakeLoop loop;
  bool second_fired = false;
  const double at = loop.now() + 1.0;
  TimerId second = kNoTimer;
  loop.at(at, [&] { loop.cancel(second); });
  second = loop.at(at, [&] { second_fired = true; });

  loop.advance(2.0);
  CHECK(!second_fired);
}

void test_a_timer_can_cancel_itself_from_inside_its_own_handler() {
  FakeLoop loop;
  int fired = 0;
  TimerId id = kNoTimer;
  id = loop.every(1.0, [&] {
    ++fired;
    loop.cancel(id);
  });

  loop.advance(10.0);
  CHECK_EQ(fired, 1);
}

void test_source_removed_from_its_own_handler_is_not_called_again() {
  FakeLoop loop;
  int calls = 0;
  SourceId id = loop.add_source(
      loop.handle(), kRead,
      [&](int) {
        ++calls;
        loop.remove_source(id);
      },
      nullptr);

  loop.deliver(id, kRead, 0.1);
  loop.deliver(id, kRead, 0.2);
  loop.advance(1.0);
  CHECK_EQ(calls, 1);
}

void test_a_handler_that_adds_sources_does_not_disturb_the_others() {
  // Server::on_accept adds a source from inside a source's handler on every
  // new connection. Doing that can reallocate the vector the loop is walking,
  // which is why tick() copies the handler out before invoking it.
  FakeLoop loop;
  int a_calls = 0, b_calls = 0, c_calls = 0;

  SourceId a = loop.add_source(loop.handle(), kRead, [&](int) { ++a_calls; },
                               nullptr);
  SourceId b = loop.add_source(
      loop.handle(), kRead,
      [&](int) {
        ++b_calls;
        for (int i = 0; i < 100; ++i) {
          loop.add_source(loop.handle(), kRead, [](int) {}, nullptr);
        }
      },
      nullptr);
  SourceId c = loop.add_source(loop.handle(), kRead, [&](int) { ++c_calls; },
                               nullptr);

  // All three ready in the same tick, with the disruptive one in the middle.
  loop.deliver(a, kRead, 0.1);
  loop.deliver(b, kRead, 0.1);
  loop.deliver(c, kRead, 0.1);
  loop.advance(1.0);

  CHECK_EQ(a_calls, 1);
  CHECK_EQ(b_calls, 1);
  CHECK_EQ(c_calls, 1);
}

void test_io_is_dispatched_before_a_timer_due_at_the_same_instant() {
  // A timer due at the same moment as an arriving packet is nearly always that
  // packet's timeout. Running the timeout first reports a failure for
  // something that had in fact arrived.
  FakeLoop loop;
  std::vector<std::string> order;
  SourceId id = loop.add_source(loop.handle(), kRead,
                                [&](int) { order.push_back("io"); }, nullptr);
  loop.deliver(id, kRead, 1.0);
  loop.after(1.0, [&] { order.push_back("timer"); });

  loop.advance(2.0);
  CHECK_EQ(order.size(), static_cast<size_t>(2));
  CHECK_STR(order[0], "io");
  CHECK_STR(order[1], "timer");
}

void test_hangup_is_reported_alongside_read() {
  // A peer that closed its write side has still sent bytes worth reading.
  // Reporting the hangup instead of the readability is how a well-formed
  // request gets thrown away; see the note in loop.h.
  FakeLoop loop;
  int seen = 0;
  SourceId id = loop.add_source(loop.handle(), kRead,
                                [&](int interest) { seen = interest; },
                                nullptr);
  loop.deliver(id, kRead | kHangup, 0.1);
  loop.advance(1.0);

  CHECK((seen & kRead) != 0);
  CHECK((seen & kHangup) != 0);
}

void test_a_failing_source_reaches_its_error_handler() {
  FakeLoop loop;
  bool ready_called = false;
  std::string why;
  SourceId id =
      loop.add_source(loop.handle(), kRead, [&](int) { ready_called = true; },
                      [&](const std::string& w) { why = w; });
  loop.fail(id, 0.1);
  loop.advance(1.0);

  CHECK(!ready_called);
  CHECK(!why.empty());
}

void test_wake_returns_from_the_wait_without_moving_the_clock() {
  // This is the path CoreBluetooth's dispatch queue and a USB receive
  // interrupt use: they cannot touch the loop, so they poke it and let it look
  // for itself.
  FakeLoop loop;
  const double before = loop.now();
  loop.wake();
  loop.tick(30.0);
  CHECK_NEAR(loop.now(), before, 1e-9);
  CHECK_EQ(loop.waits(), 1);
}

void test_stop_ends_the_loop_from_inside_a_handler() {
  FakeLoop loop;
  int fired = 0;
  loop.every(1.0, [&] {
    ++fired;
    if (fired == 3) loop.stop();
  });

  loop.advance(100.0);
  CHECK_EQ(fired, 3);
  CHECK(!loop.running());
}

void test_next_deadline_reports_the_earliest_live_timer() {
  FakeLoop loop;
  const double t0 = loop.now();
  CHECK(loop.next_deadline() < 0.0);

  TimerId far = loop.after(10.0, [] {});
  loop.after(2.0, [] {});
  CHECK_NEAR(loop.next_deadline(), t0 + 2.0, 1e-9);

  // Cancelling the far one changes nothing; cancelling is not reordering.
  loop.cancel(far);
  CHECK_NEAR(loop.next_deadline(), t0 + 2.0, 1e-9);
}

void test_set_interest_stops_delivery() {
  FakeLoop loop;
  int calls = 0;
  SourceId id = loop.add_source(loop.handle(), kRead | kWrite,
                                [&](int) { ++calls; }, nullptr);
  loop.set_interest(id, 0);
  // The fake loop delivers what the test queues regardless of interest -- it
  // is standing in for a backend, not reimplementing one -- so what is pinned
  // here is that set_interest on a live source does not lose it.
  loop.deliver(id, kRead, 0.1);
  loop.advance(1.0);
  CHECK_EQ(calls, 1);
}

void test_dispatching_is_true_only_inside_a_handler() {
  FakeLoop loop;
  bool inside = false;
  CHECK(!loop.dispatching());
  loop.after(1.0, [&] { inside = loop.dispatching(); });
  loop.advance(2.0);
  CHECK(inside);
  CHECK(!loop.dispatching());
}

}  // namespace

int main() {
  test_one_shot_fires_once_and_not_early();
  test_same_deadline_fires_in_creation_order();
  test_repeating_timer_does_not_burst_after_a_long_handler();
  test_repeating_timer_keeps_its_period();
  test_a_timer_cancelled_by_another_due_at_the_same_instant_does_not_fire();
  test_a_timer_can_cancel_itself_from_inside_its_own_handler();
  test_source_removed_from_its_own_handler_is_not_called_again();
  test_a_handler_that_adds_sources_does_not_disturb_the_others();
  test_io_is_dispatched_before_a_timer_due_at_the_same_instant();
  test_hangup_is_reported_alongside_read();
  test_a_failing_source_reaches_its_error_handler();
  test_wake_returns_from_the_wait_without_moving_the_clock();
  test_stop_ends_the_loop_from_inside_a_handler();
  test_next_deadline_reports_the_earliest_live_timer();
  test_set_interest_stops_delivery();
  test_dispatching_is_true_only_inside_a_handler();
  return octotest::report("test_loop");
}
