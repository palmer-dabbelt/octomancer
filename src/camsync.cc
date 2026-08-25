#include "camsync.h"

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
  if (state.has_last_write) {
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

Drift observe(const SyncOptions& opt, SyncState* state, double error,
              double now_mono) {
  Drift drift;

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
  }
  return drift;
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
