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
using octo::Conditions;
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
  const Decision d = octo::decide(opt, state, 30.0, 24, Conditions(true), 1000.0);
  CHECK(d.action == Action::kSkipRecording);
}

void test_external_source_backs_off() {
  SyncOptions opt = defaults();
  SyncState state;
  state.failures = opt.max_failures;
  const Decision d = octo::decide(opt, state, 30.0, 24, Conditions(false), 1000.0);
  CHECK(d.action == Action::kSkipExternal);

  // One short of the limit still tries: giving up early is its own failure.
  state.failures = opt.max_failures - 1;
  CHECK(octo::decide(opt, state, 30.0, 24, Conditions(false), 1000.0).action == Action::kWrite);
}

void test_half_frame_threshold() {
  SyncOptions opt = defaults();
  SyncState state;

  // 38 ms and 23 ms are the errors seen in a real log. Under the old
  // one-second tolerance both were "no change"; at half a frame both are
  // out of tolerance and worth acting on.
  CHECK(octo::decide(opt, state, 0.038, 24, Conditions(false), 1000.0).action == Action::kWrite);
  CHECK(octo::decide(opt, state, 0.023, 24, Conditions(false), 1000.0).action == Action::kWrite);

  // Just inside half a frame at 24 fps (20.8 ms) is left alone, in both
  // directions -- the sign of the error must not change the verdict.
  CHECK(octo::decide(opt, state, 0.020, 24, Conditions(false), 1000.0).action ==
        Action::kSkipInTolerance);
  CHECK(octo::decide(opt, state, -0.020, 24, Conditions(false), 1000.0).action ==
        Action::kSkipInTolerance);

  // At 60 fps the same 20 ms error is now more than half a frame.
  CHECK(octo::decide(opt, state, 0.020, 60, Conditions(false), 1000.0).action == Action::kWrite);
}

void test_rate_limit_holds_between_writes() {
  SyncOptions opt = defaults();
  SyncState state;
  state.has_last_write = true;
  state.last_write_mono = 1000.0;

  // Out of tolerance, but written two minutes ago: hold. This is what keeps
  // free-running stretches long enough to measure drift across.
  const Decision held = octo::decide(opt, state, 0.5, 24, Conditions(false), 1120.0);
  CHECK(held.action == Action::kSkipRateLimited);
  CHECK_NEAR(held.since_write, 120.0, 1e-9);

  // An hour and a second later, it writes.
  CHECK(octo::decide(opt, state, 0.5, 24, Conditions(false), 1000.0 + 3601.0).action ==
        Action::kWrite);

  // The rate limit is not allowed to override the recording gate.
  CHECK(octo::decide(opt, state, 0.5, 24, Conditions(true), 1120.0).action ==
        Action::kSkipRecording);

  // In-tolerance is decided before the rate limit, so a clock that is already
  // right reports why it was left alone rather than blaming the interval.
  CHECK(octo::decide(opt, state, 0.001, 24, Conditions(false), 1120.0).action ==
        Action::kSkipInTolerance);
}

// 4.7. A camera parked in the mode where its timecode does not follow the RTC
// reports 00:00:00:00 and stops, which reaches the gates as a clock roughly
// half a day wrong. Without this gate that is answered with an hourly RTC
// write that cannot help, three of which in a row make the daemon conclude an
// external source owns the camera and give up for the night.
void test_timecode_source_gate() {
  SyncOptions opt = defaults();
  SyncState state;

  Conditions cond;
  cond.has_timecode_source = true;
  cond.timecode_source = octo::bmd::kTimecodeSourceClip;

  // The error here is the real shape of the problem: midnight against a
  // mid-morning bench. Nothing about its size should get it past the gate.
  const Decision d = octo::decide(opt, state, -43000.0, 24, cond, 1000.0);
  CHECK(d.action == Action::kSkipTimecodeSource);

  // Time-of-day is the mode the whole program depends on, and must not be
  // gated.
  cond.timecode_source = octo::bmd::kTimecodeSourceTimeOfDay;
  CHECK(octo::decide(opt, state, -43000.0, 24, cond, 1000.0).action ==
        Action::kWrite);

  // Silence is not a refusal. A camera that has never mentioned 4.7 -- an
  // older body, or one whose firmware does not carry the parameter -- must
  // still be synced, or this gate strands every camera but the one it was
  // found on.
  Conditions quiet;
  CHECK(!quiet.has_timecode_source);
  CHECK(octo::decide(opt, state, -43000.0, 24, quiet, 1000.0).action ==
        Action::kWrite);

  // Anything the camera clamps to 1 is equally unhelpful, so the gate asks
  // whether the source *is* time-of-day rather than whether it is Clip.
  cond.has_timecode_source = true;
  cond.timecode_source = 7;
  CHECK(octo::decide(opt, state, -43000.0, 24, cond, 1000.0).action ==
        Action::kSkipTimecodeSource);

  // Recording still outranks it. Both gates refuse, but a take in progress is
  // the more important thing to say.
  cond.recording = true;
  CHECK(octo::decide(opt, state, -43000.0, 24, cond, 1000.0).action ==
        Action::kSkipRecording);
}

void test_timecode_follows_rtc() {
  Conditions cond;
  CHECK(octo::timecode_follows_rtc(cond));  // unknown

  cond.has_timecode_source = true;
  cond.timecode_source = octo::bmd::kTimecodeSourceTimeOfDay;
  CHECK(octo::timecode_follows_rtc(cond));

  cond.timecode_source = octo::bmd::kTimecodeSourceClip;
  CHECK(!octo::timecode_follows_rtc(cond));
}

void test_dry_run_decides_but_does_not_write() {
  SyncOptions opt = defaults();
  opt.dry_run = true;
  SyncState state;
  const Decision d = octo::decide(opt, state, 5.0, 24, Conditions(false), 1000.0);
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
  CHECK(octo::decide(opt, state, 40.0, 24, Conditions(false), 9000.0).action ==
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


// --- power cycles ----------------------------------------------------------

// The step this bench actually recorded: -0.023s at 06:32, -3.897s at 06:47.
// Fitted as drift that is 4300 ppm, which would tell the scheduler the clock
// crosses half a frame every five seconds and pin the poll to its floor for
// the rest of the night -- and would be wrong, because nothing drifted.
void test_power_cycle_is_not_drift() {
  SyncOptions opt = defaults();
  SyncState state;

  octo::observe(opt, &state, -0.0228, 1000.0);
  // Let a real drift figure accumulate first, so there is something to lose.
  octo::observe(opt, &state, -0.0700, 1000.0 + 1900.0);
  CHECK(state.drift.has);

  const octo::Drift d = octo::observe(opt, &state, -3.8973, 1000.0 + 2836.0);
  CHECK(d.restarted);
  CHECK_NEAR(d.restart_step, -3.8273, 1e-6);
  CHECK(!state.drift.has);
  // ...and the anchor restarts here, so the next stretch is measured against
  // the clock that is actually running.
  CHECK(state.has_anchor);
  CHECK_NEAR(state.anchor_error, -3.8973, 1e-9);
  CHECK_NEAR(state.anchor_mono, 1000.0 + 2836.0, 1e-9);
}

void test_ordinary_movement_is_still_drift() {
  SyncOptions opt = defaults();
  SyncState state;
  octo::observe(opt, &state, 0.020, 0.0);
  // 40 ms in a minute is a lot, but it is a frame of quantisation, not a
  // camera being switched off and on again.
  const octo::Drift d = octo::observe(opt, &state, 0.060, 60.0);
  CHECK(!d.restarted);
  CHECK(d.has_step);
}

void test_forgetting_reopens_the_external_gate() {
  SyncOptions opt = defaults();
  SyncState state;
  state.failures = 5;   // the daemon had given up
  state.adapts = 2;
  state.rtc_bias = -75; // ...but this cost hours to learn

  octo::forget_drift(&state);

  CHECK_EQ(state.failures, 0);
  CHECK_EQ(state.adapts, 0);
  CHECK_EQ(state.rtc_bias, -75);
  const Decision d = octo::decide(opt, state, 2.0, 24, Conditions(false), 0.0);
  CHECK(d.action == Action::kWrite);
}

// --- convergence -----------------------------------------------------------

// judge_write() prints "retrying next cycle", and the rate limit used to turn
// that into "retrying next hour" -- four bias steps would have taken half a
// day to converge.
void test_adapting_write_is_not_held_by_the_rate_limit() {
  SyncOptions opt = defaults();
  SyncState state;
  state.has_last_write = true;
  state.last_write_mono = 0.0;

  state.adapts = 0;
  Decision held = octo::decide(opt, state, 2.0, 24, Conditions(false), 60.0);
  CHECK(held.action == Action::kSkipRateLimited);

  state.adapts = 1;  // mid-convergence
  Decision retry = octo::decide(opt, state, 2.0, 24, Conditions(false), 60.0);
  CHECK(retry.action == Action::kWrite);
}

// --- the poll schedule -----------------------------------------------------

void test_no_drift_figure_polls_at_the_floor() {
  SyncOptions opt = defaults();
  SyncState state;
  const octo::PollPlan plan = octo::next_poll(opt, state, 0.0, 24, 0.0);
  CHECK_NEAR(plan.seconds, opt.poll, 1e-9);
  CHECK_STR(plan.reason, "floor");
}

void test_fixed_poll_ignores_everything() {
  SyncOptions opt = defaults();
  opt.adaptive_poll = false;
  SyncState state;
  state.drift.has = true;
  state.drift.ppm = 1.0;
  state.drift.span = 7200.0;
  state.has_last_write = true;
  state.last_write_mono = 0.0;

  const octo::PollPlan plan = octo::next_poll(opt, state, 0.0, 24, 10.0);
  CHECK_NEAR(plan.seconds, opt.poll, 1e-9);
  CHECK_STR(plan.reason, "fixed");
}

// The measured figure for this camera: -24.8 ppm over most of a night.
void test_measured_drift_stretches_the_interval() {
  SyncOptions opt = defaults();
  SyncState state;
  state.drift.has = true;
  state.drift.ppm = -24.8;
  state.drift.span = 3600.0;

  // A frame at 24 fps over an hour is 11.6 ppm the measurement cannot resolve,
  // so schedule against 36.4, not 24.8.
  const double bound = octo::drift_bound_ppm(opt, state.drift, 24);
  CHECK_NEAR(bound, 24.8 + (1.0 / 24.0) / 3600.0 * 1e6, 1e-6);

  const octo::PollPlan plan = octo::next_poll(opt, state, 0.0, 24, 0.0);
  const double tol = octo::trigger_tolerance(opt, 24);
  CHECK_NEAR(plan.until_actionable, tol / (bound * 1e-6), 1e-6);
  CHECK_NEAR(plan.seconds, plan.until_actionable / opt.poll_slices, 1e-6);
  CHECK_STR(plan.reason, "drift");
  // Concretely: minutes rather than the fixed one.
  CHECK(plan.seconds > 120.0);
}

void test_interval_tightens_as_the_threshold_nears() {
  SyncOptions opt = defaults();
  SyncState state;
  state.drift.has = true;
  state.drift.ppm = -24.8;
  state.drift.span = 3600.0;

  const double far = octo::next_poll(opt, state, 0.000, 24, 0.0).seconds;
  const double near = octo::next_poll(opt, state, 0.018, 24, 0.0).seconds;
  CHECK(near < far);
  // Past the threshold there is no headroom left, so it drops to the floor.
  const octo::PollPlan over = octo::next_poll(opt, state, 0.500, 24, 0.0);
  CHECK_NEAR(over.seconds, opt.poll, 1e-9);
}

void test_a_slow_clock_still_gets_looked_at() {
  SyncOptions opt = defaults();
  SyncState state;
  state.drift.has = true;
  state.drift.ppm = 0.05;   // suspiciously good
  state.drift.span = 7200.0;

  // Believing 0.05 ppm would put the threshold a week away. Two hours of
  // watching at 24 fps cannot resolve better than 5.8 ppm, and that alone is
  // enough to keep the schedule honest.
  const double two_hours = octo::drift_bound_ppm(opt, state.drift, 24);
  CHECK_NEAR(two_hours, 0.05 + (1.0 / 24.0) / 7200.0 * 1e6, 1e-9);

  // Once the arm is long enough that quantisation stops mattering, the floor
  // is what remains: a clock measured at 0.05 ppm this afternoon is not a
  // clock that will hold 0.05 ppm overnight.
  state.drift.span = 200000.0;
  CHECK_NEAR(octo::drift_bound_ppm(opt, state.drift, 24), opt.min_assumed_ppm,
             1e-9);

  const octo::PollPlan plan = octo::next_poll(opt, state, 0.0, 24, 0.0);
  CHECK(plan.seconds <= opt.max_poll);
  CHECK(plan.seconds >= opt.poll);
}

void test_rate_limit_sets_the_horizon_when_it_is_further_off() {
  SyncOptions opt = defaults();
  SyncState state;
  state.has_last_write = true;
  state.last_write_mono = 0.0;

  // Ten minutes after a write, with fifty still to run: nothing found in the
  // next fifty minutes can be acted on, so there is no point looking every
  // minute to find it.
  const octo::PollPlan plan = octo::next_poll(opt, state, 0.400, 24, 600.0);
  CHECK_STR(plan.reason, "rate-limit");
  CHECK_NEAR(plan.until_actionable, 3000.0, 1e-6);
  CHECK_NEAR(plan.seconds, 750.0, 1e-6);
}

void test_the_horizon_is_the_later_of_the_two() {
  SyncOptions opt = defaults();
  SyncState state;
  state.drift.has = true;
  state.drift.ppm = -24.8;
  state.drift.span = 3600.0;
  state.has_last_write = true;
  state.last_write_mono = 0.0;

  // Drift alone would say ~570s; the rate limit says 3540s. The later one wins.
  const octo::PollPlan plan = octo::next_poll(opt, state, 0.0, 24, 60.0);
  CHECK_STR(plan.reason, "rate-limit");
  CHECK_NEAR(plan.until_actionable, 3540.0, 1e-6);
}

void test_the_ceiling_and_the_floor_both_hold() {
  SyncOptions opt = defaults();
  SyncState state;
  state.has_last_write = true;
  state.last_write_mono = 0.0;

  // Straight after a write the horizon is a full hour; a quarter of that is
  // still over the ceiling.
  const octo::PollPlan capped = octo::next_poll(opt, state, 0.4, 24, 0.0);
  CHECK_NEAR(capped.seconds, opt.max_poll, 1e-9);

  // ...and as it lapses, the floor catches it.
  const octo::PollPlan lapsed = octo::next_poll(opt, state, 0.4, 24, 3599.0);
  CHECK_NEAR(lapsed.seconds, opt.poll, 1e-9);
}

// Sleeping a quarter of the remaining wait each time converges on the moment
// something can be done without ever overshooting it, and gathers a handful of
// observations on the way -- which is what the drift fit is made of.
void test_the_schedule_converges_and_does_not_overshoot() {
  SyncOptions opt = defaults();
  SyncState state;
  state.has_last_write = true;
  state.last_write_mono = 0.0;

  double now = 0.0;
  int looks = 0;
  while (now < opt.min_write_interval && looks < 500) {
    const octo::PollPlan plan = octo::next_poll(opt, state, 0.400, 24, now);
    // Never past the moment the gate lifts by more than one floor interval.
    CHECK(now + plan.seconds <= opt.min_write_interval + opt.poll);
    now += plan.seconds;
    looks++;
  }
  // Far fewer than the sixty a fixed minute would have cost, but not so few
  // that a free-running hour has nothing to fit a line to.
  CHECK(looks > 5);
  CHECK(looks < 25);
}

}  // namespace


// --- the age of a reading -------------------------------------------------

// The bug this fixes: a reading that arrived 80ms ago was compared against a
// host clock read now, so the camera was charged 80ms it never owed.
void test_a_stale_reading_is_aged() {
  CHECK_NEAR(octo::reading_age_s(1000.080, 1000.000), 0.080, 1e-9);  // a reading 80ms old is 80ms old
}

// A default-constructed view carries 0.0, and mono_now() is uptime -- so
// treating that as a stamp would age the reading by however long the process
// had been running, which is the one error worse than the one being fixed.
void test_an_unstamped_reading_is_treated_as_fresh() {
  CHECK_NEAR(octo::reading_age_s(9999.0, 0.0), 0.0, 1e-9);  // no stamp means no correction
}

void test_a_stamp_in_the_future_is_treated_as_fresh() {
  CHECK_NEAR(octo::reading_age_s(1000.0, 1000.5), 0.0, 1e-9);  // a negative age is not a correction
}

// Past a few seconds the reading is not evidence about a clock at all, and a
// correction that large would move the answer further than the error does.
void test_a_very_stale_reading_is_capped() {
  CHECK_NEAR(octo::reading_age_s(1100.0, 1000.0, 5.0), 5.0, 1e-9);  // the cap holds
}

// The whole point, stated as the arithmetic the caller performs: a camera that
// is actually correct must measure as correct, however long its reading sat.
void test_correcting_for_age_removes_the_apparent_lateness() {
  const double host_at_arrival = 43200.000;
  const double camera_said = 43200.000;   // exactly right, at that instant
  const double sat_for = 0.080;
  const double host_now = host_at_arrival + sat_for;

  const double uncorrected = camera_said - host_now;
  CHECK_NEAR(uncorrected, -0.080, 1e-9);  // without the fix the camera looks 80ms late

  const double age = octo::reading_age_s(500.080, 500.000);
  const double corrected = camera_said - (host_now - age);
  CHECK_NEAR(corrected, 0.0, 1e-9);  // with the fix it looks correct
}

// --- reading the centre of a frame, not its edge --------------------------

void test_half_a_frame_is_half_a_frame() {
  CHECK_NEAR(octo::frame_centre_s(24), 0.5 / 24.0, 1e-12);
  CHECK_NEAR(octo::frame_centre_s(60), 0.5 / 60.0, 1e-12);
}

// An unknown frame rate must not invent a correction out of a division by zero.
void test_no_frame_rate_means_no_correction() {
  CHECK_NEAR(octo::frame_centre_s(0), 0.0, 1e-12);
  CHECK_NEAR(octo::frame_centre_s(-1), 0.0, 1e-12);
}

// The bias being removed: a camera whose clock is exactly right, sampled at a
// uniformly random moment inside a frame, reads low by half a frame on average
// when its frame number is taken at face value.
void test_face_value_is_biased_low_by_half_a_frame() {
  const int fps = 24;
  const double frame = 1.0 / fps;
  const int steps = 1000;

  double raw = 0.0;
  double centred = 0.0;
  for (int i = 0; i < steps; ++i) {
    // The camera's true clock, somewhere inside the frame it is naming.
    const double truth = (i + 0.5) / steps * frame;
    // The frame it names starts at zero.
    raw += 0.0 - truth;
    centred += octo::frame_centre_s(fps) - truth;
  }
  CHECK_NEAR(raw / steps, -frame / 2.0, 1e-6);
  CHECK_NEAR(centred / steps, 0.0, 1e-6);
}

int main() {
  test_half_a_frame_is_half_a_frame();
  test_no_frame_rate_means_no_correction();
  test_face_value_is_biased_low_by_half_a_frame();
  test_a_stale_reading_is_aged();
  test_an_unstamped_reading_is_treated_as_fresh();
  test_a_stamp_in_the_future_is_treated_as_fresh();
  test_a_very_stale_reading_is_capped();
  test_correcting_for_age_removes_the_apparent_lateness();
  test_tolerance_scales_with_frame_rate();
  test_recording_beats_everything();
  test_external_source_backs_off();
  test_half_frame_threshold();
  test_rate_limit_holds_between_writes();
  test_timecode_source_gate();
  test_timecode_follows_rtc();
  test_dry_run_decides_but_does_not_write();
  test_good_write_is_not_a_failure();
  test_three_bad_writes_stop_the_daemon();
  test_bias_is_learned_then_settles();
  test_improvement_counts_even_outside_tolerance();
  test_drift_uses_the_write_anchor();
  test_aligned_write_lands_on_a_whole_second();
  test_format_span();
  test_power_cycle_is_not_drift();
  test_ordinary_movement_is_still_drift();
  test_forgetting_reopens_the_external_gate();
  test_adapting_write_is_not_held_by_the_rate_limit();
  test_no_drift_figure_polls_at_the_floor();
  test_fixed_poll_ignores_everything();
  test_measured_drift_stretches_the_interval();
  test_interval_tightens_as_the_threshold_nears();
  test_a_slow_clock_still_gets_looked_at();
  test_rate_limit_sets_the_horizon_when_it_is_further_off();
  test_the_horizon_is_the_later_of_the_two();
  test_the_ceiling_and_the_floor_both_hold();
  test_the_schedule_converges_and_does_not_overshoot();
  return octotest::report("test_camsync");
}
