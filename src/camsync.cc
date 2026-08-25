#include "camsync.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace octo {

namespace {

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));

std::string fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

int round_to_int(double v) { return static_cast<int>(std::lround(v)); }

}  // namespace

const char* action_name(Action a) {
  switch (a) {
    case Action::kWrite: return "write";
    case Action::kSkipRecording: return "skip:recording";
    case Action::kSkipExternal: return "skip:external-suspected";
    case Action::kSkipInTolerance: return "skip:in-tolerance";
    case Action::kSkipRateLimited: return "skip:rate-limited";
    case Action::kSkipDryRun: return "skip:dry-run";
  }
  return "?";
}

std::string format_span(double seconds) {
  if (seconds < 90.0) return fmt("%.0fs", seconds);
  if (seconds < 5400.0) return fmt("%.0fm", seconds / 60.0);
  return fmt("%.1fh", seconds / 3600.0);
}

double trigger_tolerance(const SyncOptions& opt, int fps) {
  if (opt.has_tolerance) return opt.tolerance;
  int rate = fps > 0 ? fps : opt.fps;
  if (rate <= 0) rate = 24;
  return opt.tolerance_frames / static_cast<double>(rate);
}

Decision decide(const SyncOptions& opt, const SyncState& state, double error,
                int fps, bool recording, double now_mono) {
  Decision d;
  d.tolerance = trigger_tolerance(opt, fps);

  if (recording) {
    d.action = Action::kSkipRecording;
    d.message = "  gate: camera is RECORDING -- leaving the clock alone";
    return d;
  }

  if (state.failures >= opt.max_failures) {
    d.action = Action::kSkipExternal;
    d.message = fmt("  gate: %d writes in a row did not take -- assuming an"
                    " external timecode source owns this camera",
                    state.failures);
    return d;
  }

  if (std::fabs(error) <= d.tolerance) {
    d.action = Action::kSkipInTolerance;
    d.message = fmt("  within %.0fms tolerance -- no change", d.tolerance * 1000.0);
    return d;
  }

  // The threshold above decides whether the clock is wrong; this decides
  // whether it is worth doing anything about it yet. They are separate
  // questions once the threshold is tight enough that the answer to the first
  // one is nearly always yes.
  //
  // A write that is still converging is exempt. judge_write() promises to
  // retry on the next cycle after a bias adjustment, and the rate limit would
  // otherwise turn "next cycle" into "next hour" and make convergence take
  // half a day. max_adapts bounds how many writes this can let through.
  if (state.has_last_write && state.adapts == 0) {
    const double since = now_mono - state.last_write_mono;
    if (since < opt.min_write_interval) {
      d.action = Action::kSkipRateLimited;
      d.since_write = since;
      d.message = fmt("  off %+.3fs, but wrote %s ago -- holding for %s", error,
                      format_span(since).c_str(),
                      format_span(opt.min_write_interval - since).c_str());
      return d;
    }
  }

  if (opt.dry_run) {
    d.action = Action::kSkipDryRun;
    d.message = fmt("  --dry-run: would correct %+.3fs", -error);
    return d;
  }

  d.action = Action::kWrite;
  return d;
}

void forget_drift(SyncState* state) {
  // state->lead and state->rtc_bias are deliberately untouched; see the
  // header. Everything below is a statement about the old clock's *value*.
  state->drift = DriftEstimate();
  state->has_last_obs = false;
  state->has_anchor = false;
  state->wrote_since_obs = false;
  state->failures = 0;
  state->adapts = 0;
}

Drift observe(const SyncOptions& opt, SyncState* state, double error,
              double now_mono) {
  Drift drift;

  // A jump this large is not a clock walking, it is a clock being set. In
  // practice that means the camera was switched off and on again: this bench
  // logged -0.023s at 06:32 and -3.897s at 06:47, which fits as 4300 ppm and
  // is nothing of the sort. Fitting it would poison the poll schedule for the
  // rest of the night, so everything measured against the old clock goes.
  if (state->has_last_obs && !state->wrote_since_obs &&
      std::fabs(error - state->last_obs_error) > opt.restart_step) {
    drift.restarted = true;
    drift.restart_step = error - state->last_obs_error;
    forget_drift(state);
  }

  if (state->has_last_obs && !state->wrote_since_obs) {
    const double dt = now_mono - state->last_obs_mono;
    if (dt > 5.0) {
      drift.has_step = true;
      drift.step_dt = dt;
      drift.step_ppm = (error - state->last_obs_error) / dt * 1e6;
      // Kept in the log for later fitting, but only put on screen once the
      // interval is long enough to mean anything: the camera reports whole
      // frames, so 42 ms of quantisation over a one-minute gap invents
      // ~700 ppm of drift that reads as a real measurement.
      drift.step_shown = dt >= opt.min_drift_interval;
    }
  }
  state->has_last_obs = true;
  state->last_obs_mono = now_mono;
  state->last_obs_error = error;
  state->wrote_since_obs = false;

  if (!state->has_anchor) {
    state->has_anchor = true;
    state->anchor_mono = now_mono;
    state->anchor_error = error;
  }
  const double span = now_mono - state->anchor_mono;
  if (span > 5.0) {
    drift.has_anchor = true;
    drift.anchor_span = span;
    drift.anchor_ppm = (error - state->anchor_error) / span * 1e6;
    drift.anchor_shown = span >= opt.min_drift_interval;
    // Only a lever arm long enough to have resolved something is worth
    // scheduling against. The same threshold that decides whether to print the
    // figure decides whether to believe it.
    if (drift.anchor_shown) {
      const int seen = state->drift.samples;
      state->drift.has = true;
      state->drift.ppm = drift.anchor_ppm;
      state->drift.span = span;
      state->drift.samples = seen + 1;
    }
  }
  return drift;
}

double observed_apply_delay(double lead_used_s, double error_after_s) {
  return lead_used_s - error_after_s;
}

LeadEstimate estimate_lead(const std::vector<double>& delays,
                           const SyncOptions& opt) {
  LeadEstimate out;
  if (!opt.adapt_lead) return out;
  const int want = opt.lead_window > 0 ? opt.lead_window : 1;
  const int need = opt.min_lead_samples > 0 ? opt.min_lead_samples : 1;
  if (static_cast<int>(delays.size()) < need) return out;

  // The most recent `want`, which is what makes this follow a camera rather
  // than average over its whole recorded life.
  std::vector<double> recent;
  const size_t from =
      delays.size() > static_cast<size_t>(want) ? delays.size() - want : 0;
  recent.assign(delays.begin() + static_cast<long>(from), delays.end());

  std::sort(recent.begin(), recent.end());
  const size_t n = recent.size();
  double median = (n % 2 == 1) ? recent[n / 2]
                               : 0.5 * (recent[n / 2 - 1] + recent[n / 2]);

  // A negative delay would mean the camera acted before it was asked, which
  // means the measurement is wrong rather than the camera being clairvoyant.
  if (median < 0.0) median = 0.0;
  if (median > opt.max_lead) median = opt.max_lead;

  out.has = true;
  out.lead_s = median;
  out.samples = static_cast<int>(n);
  return out;
}

double effective_lead(const SyncOptions& opt, const SyncState& state) {
  if (opt.adapt_lead && state.lead.has) return state.lead.lead_s;
  return opt.lead;
}

double drift_bound_ppm(const SyncOptions& opt, const DriftEstimate& est,
                       int fps) {
  if (!est.has) return 0.0;
  int rate = fps > 0 ? fps : opt.fps;
  if (rate <= 0) rate = 24;
  const double quantisation =
      est.span > 0.0 ? (1.0 / rate) / est.span * 1e6 : 0.0;
  double bound = std::fabs(est.ppm) + quantisation;
  if (bound < opt.min_assumed_ppm) bound = opt.min_assumed_ppm;
  return bound;
}

PollPlan next_poll(const SyncOptions& opt, const SyncState& state, double error,
                   int fps, double now_mono) {
  PollPlan plan;
  plan.seconds = opt.poll;
  if (!opt.adaptive_poll) {
    plan.reason = "fixed";
    return plan;
  }

  // How long until the clock is wrong by more than the trigger threshold.
  // Without a drift figure this is zero: not knowing how fast a clock walks is
  // a reason to keep watching it, not a licence to look away.
  double until_wrong = 0.0;
  double bound = 0.0;
  const double tolerance = trigger_tolerance(opt, fps);
  if (state.drift.has) {
    bound = drift_bound_ppm(opt, state.drift, fps);
    const double headroom = tolerance - std::fabs(error);
    if (headroom > 0.0 && bound > 0.0) until_wrong = headroom / (bound * 1e-6);
  }

  // How long until a write would be allowed at all.
  double until_allowed = 0.0;
  if (state.has_last_write && state.adapts == 0) {
    until_allowed = opt.min_write_interval - (now_mono - state.last_write_mono);
    if (until_allowed < 0.0) until_allowed = 0.0;
  }

  const bool gated = until_allowed >= until_wrong;
  plan.until_actionable = gated ? until_allowed : until_wrong;

  const int slices = opt.poll_slices > 0 ? opt.poll_slices : 1;
  double want = plan.until_actionable / slices;
  if (want > opt.max_poll) want = opt.max_poll;
  if (want < opt.poll) want = opt.poll;
  plan.seconds = want;

  if (want <= opt.poll) {
    plan.reason = "floor";
    return plan;
  }
  if (gated) {
    plan.reason = "rate-limit";
    plan.message = fmt("  nothing to do for %s (rate limit) -- next look in %s",
                       format_span(plan.until_actionable).c_str(),
                       format_span(want).c_str());
  } else {
    plan.reason = "drift";
    plan.message = fmt("  %.0f ppm needs %s to reach %.0fms -- next look in %s",
                       bound, format_span(plan.until_actionable).c_str(),
                       tolerance * 1000.0, format_span(want).c_str());
  }
  return plan;
}

WriteOutcome judge_write(const SyncOptions& opt, SyncState* state,
                         double error_before, double error_after,
                         double now_mono) {
  WriteOutcome out;
  out.bias_before = state->rtc_bias;
  out.bias_after = state->rtc_bias;

  const bool ok = std::fabs(error_after) <= opt.write_tolerance;
  const bool improved = std::fabs(error_after) < std::fabs(error_before) - 0.25;
  const int adapts = state->adapts;

  out.verified = ok || improved;

  // A write ends the free-running stretch, so both drift baselines restart
  // here rather than at the next observation -- otherwise the correction
  // itself would be fitted as if it were drift.
  state->wrote_since_obs = true;
  state->has_last_obs = true;
  state->last_obs_mono = now_mono;
  state->last_obs_error = error_after;
  state->has_anchor = true;
  state->anchor_mono = now_mono;
  state->anchor_error = error_after;

  if (ok || improved) {
    state->failures = 0;
    state->adapts = 0;
    out.verdict = Verdict::kOk;
    // Half a second, because past that the whole-second bias is what is wrong
    // and the sub-second lead cannot fix it.
    out.timing_usable = std::fabs(error_after) < 0.5;
    out.message = fmt("  verified: error %+.3fs -> %+.3fs", error_before,
                      error_after);

    // A residual after a verified write means the bias is wrong. Fold it back
    // in, or the daemon writes every cycle forever and never converges. This
    // body's offset is not a constant of nature: it was -75 s before a power
    // cycle and 0 after one, so the bias has to be learned, not configured.
    //
    // The bias is a whole number of seconds, so a sub-second residual cannot
    // move it -- and with the tolerance at half a frame, testing against the
    // trigger threshold here would log a no-op adjustment after every write.
    const int residual = round_to_int(-error_after);
    if (residual != 0 && opt.adapt_bias) {
      state->rtc_bias = out.bias_before + residual;
      out.bias_after = state->rtc_bias;
      out.bias_changed = true;
    }
    return out;
  }

  if (opt.adapt_bias && adapts < opt.max_adapts) {
    int step = round_to_int(-error_after);
    if (std::abs(step) > opt.max_bias_step) {
      step = step > 0 ? opt.max_bias_step : -opt.max_bias_step;
    }
    state->rtc_bias = out.bias_before + step;
    state->adapts = adapts + 1;
    out.bias_after = state->rtc_bias;
    out.bias_changed = true;
    out.verdict = Verdict::kAdapting;
    out.message = fmt("  landed %+.3fs out; RTC bias %+ds -> %+ds, retrying"
                      " next cycle (attempt %d/%d)",
                      error_after, out.bias_before, out.bias_after,
                      adapts + 1, opt.max_adapts);
    return out;
  }

  state->failures += 1;
  state->adapts = 0;
  out.verdict = Verdict::kNoEffect;
  out.message = fmt("  WRITE DID NOT TAKE: error %+.3fs -> %+.3fs after %d bias"
                    " adjustments (%d in a row)",
                    error_before, error_after, adapts, state->failures);
  return out;
}

double aligned_wait(double now_unix, double offset, double bias, double lead) {
  const double target = now_unix + offset + bias;
  double whole = std::floor(target) + 1.0;
  while (whole - target < lead) whole += 1.0;
  const double wait = (whole - target) - lead;
  return wait > 0.0 ? wait : 0.0;
}

bmd::Civil aligned_value(double send_unix, double offset, double bias) {
  return bmd::utc_civil(std::floor(send_unix + offset + bias + 0.5));
}

}  // namespace octo
