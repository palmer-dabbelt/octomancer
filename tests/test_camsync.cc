// The gates, the drift baselines and the bias feedback.
//
// This is the file that exists because of a bug that would only have shown
// itself overnight. Tightening the trigger threshold to half a frame, without
// also separating it from the threshold a *completed* write is judged against,
// makes every successful write look like a failure -- a good write lands tens
// of milliseconds out, and half a frame at 24 fps is 21 ms. Three of those in
// a row and the daemon concludes an external source owns the camera and stops
// writing for the rest of the night. It is caught here in microseconds.
#include <cmath>

#include "camsync.h"
#include "harness.h"

using octo::Action;
using octo::Decision;
using octo::SyncOptions;
using octo::SyncState;
using octo::Verdict;

namespace {

SyncOptions defaults() {
  return SyncOptions();  // the shipped defaults, deliberately not overridden
}

void test_tolerance_scales_with_frame_rate() {
  SyncOptions opt = defaults();
  // Half a frame is a different number of seconds at every rate, which is the
  // whole reason the threshold is expressed in frames.
  CHECK_NEAR(octo::trigger_tolerance(opt, 24), 0.5 / 24.0, 1e-12);
  CHECK_NEAR(octo::trigger_tolerance(opt, 60), 0.5 / 60.0, 1e-12);

  // A camera that reports no rate falls back to the configured guess, never
  // to a division by zero.
  opt.fps = 25;
  CHECK_NEAR(octo::trigger_tolerance(opt, 0), 0.5 / 25.0, 1e-12);
  opt.fps = 0;
  CHECK_NEAR(octo::trigger_tolerance(opt, 0), 0.5 / 24.0, 1e-12);

  // An explicit seconds value wins over the frame count.
  opt.has_tolerance = true;
  opt.tolerance = 0.25;
  CHECK_NEAR(octo::trigger_tolerance(opt, 60), 0.25, 1e-12);
}

void test_recording_beats_everything() {
  SyncOptions opt = defaults();
  SyncState state;
  // Wildly wrong, never written, not rate limited: still refused, because a
  // timecode jump mid-take corrupts the take.
  const Decision d = octo::decide(opt, state, 30.0, 24, /*recording=*/true, 1000.0);
  CHECK(d.action == Action::kSkipRecording);
}

void test_external_source_backs_off() {
  SyncOptions opt = defaults();
  SyncState state;
  state.failures = opt.max_failures;
  const Decision d = octo::decide(opt, state, 30.0, 24, false, 1000.0);
  CHECK(d.action == Action::kSkipExternal);

  // One short of the limit still tries: giving up early is its own failure.
  state.failures = opt.max_failures - 1;
  CHECK(octo::decide(opt, state, 30.0, 24, false, 1000.0).action == Action::kWrite);
}

void test_half_frame_threshold() {
  SyncOptions opt = defaults();
  SyncState state;

  // 38 ms and 23 ms are the errors seen in a real log. Under the old
  // one-second tolerance both were "no change"; at half a frame both are
  // out of tolerance and worth acting on.
  CHECK(octo::decide(opt, state, 0.038, 24, false, 1000.0).action == Action::kWrite);
  CHECK(octo::decide(opt, state, 0.023, 24, false, 1000.0).action == Action::kWrite);

  // Just inside half a frame at 24 fps (20.8 ms) is left alone, in both
  // directions -- the sign of the error must not change the verdict.
  CHECK(octo::decide(opt, state, 0.020, 24, false, 1000.0).action ==
        Action::kSkipInTolerance);
  CHECK(octo::decide(opt, state, -0.020, 24, false, 1000.0).action ==
        Action::kSkipInTolerance);

  // At 60 fps the same 20 ms error is now more than half a frame.
  CHECK(octo::decide(opt, state, 0.020, 60, false, 1000.0).action == Action::kWrite);
}

void test_rate_limit_holds_between_writes() {
  SyncOptions opt = defaults();
  SyncState state;
  state.has_last_write = true;
  state.last_write_mono = 1000.0;

  // Out of tolerance, but written two minutes ago: hold. This is what keeps
  // free-running stretches long enough to measure drift across.
  const Decision held = octo::decide(opt, state, 0.5, 24, false, 1120.0);
  CHECK(held.action == Action::kSkipRateLimited);
  CHECK_NEAR(held.since_write, 120.0, 1e-9);

  // An hour and a second later, it writes.
  CHECK(octo::decide(opt, state, 0.5, 24, false, 1000.0 + 3601.0).action ==
        Action::kWrite);

  // The rate limit is not allowed to override the recording gate.
  CHECK(octo::decide(opt, state, 0.5, 24, true, 1120.0).action ==
        Action::kSkipRecording);

  // In-tolerance is decided before the rate limit, so a clock that is already
  // right reports why it was left alone rather than blaming the interval.
  CHECK(octo::decide(opt, state, 0.001, 24, false, 1120.0).action ==
        Action::kSkipInTolerance);
}

void test_dry_run_decides_but_does_not_write() {
  SyncOptions opt = defaults();
  opt.dry_run = true;
  SyncState state;
  const Decision d = octo::decide(opt, state, 5.0, 24, false, 1000.0);
  CHECK(d.action == Action::kSkipDryRun);
}

// The regression this whole file is here for.
void test_good_write_is_not_a_failure() {
  SyncOptions opt = defaults();
  SyncState state;

  // A write that landed 118 ms out -- an actual observed result, and a good
  // one. It is five times the half-frame trigger threshold, so judging it
  // against that threshold would call it a failure.
  const double trigger = octo::trigger_tolerance(opt, 24);
  CHECK(std::fabs(-0.118) > trigger);

  const octo::WriteOutcome out =
      octo::judge_write(opt, &state, -0.829, -0.118, 2000.0);
  CHECK(out.verdict == Verdict::kOk);
  CHECK(out.verified);
  CHECK_EQ(state.failures, 0);

  // ...and the sub-second residual must not move the bias, which is a whole
  // number of seconds. Otherwise every write logs a no-op adjustment.
  CHECK(!out.bias_changed);
  CHECK_EQ(state.rtc_bias, 0);
}

void test_three_bad_writes_stop_the_daemon() {
  SyncOptions opt = defaults();
  opt.adapt_bias = false;  // no attempts left to make
  SyncState state;

  for (int i = 0; i < opt.max_failures; ++i) {
    const octo::WriteOutcome out =
        octo::judge_write(opt, &state, 40.0, 40.0, 2000.0 + i);
    CHECK(out.verdict == Verdict::kNoEffect);
    CHECK(!out.verified);
  }
  CHECK_EQ(state.failures, opt.max_failures);
  CHECK(octo::decide(opt, state, 40.0, 24, false, 9000.0).action ==
        Action::kSkipExternal);
}

void test_bias_is_learned_then_settles() {
  SyncOptions opt = defaults();
  SyncState state;

  // A write that lands 75 s slow: the offset seen on this body before a power
  // cycle. It is out of write_tolerance and no better than before, so the
  // bias absorbs it rather than counting as a failure.
  octo::WriteOutcome out = octo::judge_write(opt, &state, -75.0, -75.0, 2000.0);
  CHECK(out.verdict == Verdict::kAdapting);
  CHECK_EQ(out.bias_after, 75);
  CHECK_EQ(state.rtc_bias, 75);
  CHECK_EQ(state.failures, 0);

  // With the bias applied the next write lands, and the adaptation counter
  // resets so a later problem gets a full set of attempts of its own.
  out = octo::judge_write(opt, &state, -75.0, -0.05, 2100.0);
  CHECK(out.verdict == Verdict::kOk);
  CHECK_EQ(state.adapts, 0);
  CHECK_EQ(state.rtc_bias, 75);  // sub-second residual leaves it alone

  // A single correction is capped, so one wild reading cannot throw the bias
  // somewhere it will take all night to walk back from.
  SyncState wild;
  out = octo::judge_write(opt, &wild, -5000.0, -5000.0, 3000.0);
  CHECK_EQ(out.bias_after, opt.max_bias_step);
}

void test_improvement_counts_even_outside_tolerance() {
  SyncOptions opt = defaults();
  SyncState state;
  // "The error did not change" is not evidence a write was ignored, and the
  // converse matters too: a write that halved a large error clearly landed,
  // even though it is still outside write_tolerance.
  const octo::WriteOutcome out =
      octo::judge_write(opt, &state, 10.0, -3.0, 2000.0);
  CHECK(out.verdict == Verdict::kOk);
  CHECK(out.verified);
}

void test_drift_uses_the_write_anchor() {
  SyncOptions opt = defaults();
  SyncState state;

  // First observation: nothing to compare against yet.
  octo::Drift d = octo::observe(opt, &state, 0.000, 0.0);
  CHECK(!d.has_step);
  CHECK(!d.has_anchor);

  // One poll interval later. The step figure exists but must not be shown:
  // 42 ms of frame quantisation over 60 s invents ~700 ppm of nothing.
  d = octo::observe(opt, &state, 0.042, 60.0);
  CHECK(d.has_step);
  CHECK(!d.step_shown);
  CHECK(d.has_anchor);
  CHECK(!d.anchor_shown);

  // Half an hour on, the anchor has a long enough lever arm to be believed.
  d = octo::observe(opt, &state, 0.900, 1800.0);
  CHECK(d.has_anchor);
  CHECK(d.anchor_shown);
  CHECK_NEAR(d.anchor_span, 1800.0, 1e-9);
  CHECK_NEAR(d.anchor_ppm, 0.900 / 1800.0 * 1e6, 1e-6);

  // A write resets both baselines: fitting across a correction would measure
  // the correction rather than the drift.
  octo::judge_write(opt, &state, 0.900, -0.010, 1810.0);
  d = octo::observe(opt, &state, 0.000, 1900.0);
  CHECK(!d.has_step);  // wrote_since_obs suppresses the step figure
  CHECK(d.has_anchor);
  CHECK_NEAR(d.anchor_span, 90.0, 1e-9);
}

void test_aligned_write_lands_on_a_whole_second() {
  SyncOptions opt = defaults();

  // Two thirds of the way through a second, aiming at the next boundary and
  // sending `lead` early.
  const double now = 1000000.666;
  const double wait = octo::aligned_wait(now, 0.0, 0, opt.lead);
  CHECK_NEAR(wait, (1000001.0 - now) - opt.lead, 1e-9);

  // Sending at that moment must produce the whole second aimed at, not the one
  // below it. Truncating instead of rounding here lands the camera a full
  // second slow on every single write.
  const octo::bmd::Civil when = octo::aligned_value(now + wait, 0.0, 0);
  const octo::bmd::Civil want = octo::bmd::utc_civil(1000001.0);
  CHECK_EQ(when.second, want.second);
  CHECK_EQ(when.minute, want.minute);
  CHECK_EQ(when.hour, want.hour);

  // Too close to the boundary to make it: skip to the next one rather than
  // sending late.
  const double late = 1000000.99;
  const double wait2 = octo::aligned_wait(late, 0.0, 0, opt.lead);
  CHECK_NEAR(wait2, (1000002.0 - late) - opt.lead, 1e-9);
  CHECK(wait2 > 0.0);

  // The offset and the bias both shift where the boundary falls.
  const octo::bmd::Civil biased = octo::aligned_value(now, 0.0, 75);
  CHECK_EQ(biased.second, octo::bmd::utc_civil(1000000.666 + 75.5).second);
}

void test_format_span() {
  CHECK_STR(octo::format_span(38.0), "38s");
  CHECK_STR(octo::format_span(124.0), "2m");
  CHECK_STR(octo::format_span(3540.0), "59m");
  // The default hour reads as "60m", not "1.0h": minutes stay readable up to
  // 90 of them, which keeps "holding for 58m" in the same units as the
  // interval it is counting down from.
  CHECK_STR(octo::format_span(3600.0), "60m");
  CHECK_STR(octo::format_span(7200.0), "2.0h");
}

}  // namespace

int main() {
  test_tolerance_scales_with_frame_rate();
  test_recording_beats_everything();
  test_external_source_backs_off();
  test_half_frame_threshold();
  test_rate_limit_holds_between_writes();
  test_dry_run_decides_but_does_not_write();
  test_good_write_is_not_a_failure();
  test_three_bad_writes_stop_the_daemon();
  test_bias_is_learned_then_settles();
  test_improvement_counts_even_outside_tolerance();
  test_drift_uses_the_write_anchor();
  test_aligned_write_lands_on_a_whole_second();
  test_format_span();
  return octotest::report("test_camsync");
}
