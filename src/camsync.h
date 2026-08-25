// Deciding whether to touch the camera's clock, and what to make of it after.
//
// None of this talks to a radio. That is deliberate: the gates below are the
// part of the program most likely to be wrong in a way that only shows up
// overnight, and the only way to test "does it refuse to write while
// recording" or "does a good write get mistaken for a failure" without a
// camera, a Tentacle bench and an hour to wait is to keep the arithmetic on
// this side of the seam. src/camera.h is where the radio starts.
//
// The gates, in the order they are applied, each logged with its reason:
//
//   * **Recording.** Never touch the clock while transport mode (10.1) says
//     Record. Jumping timecode mid-take corrupts the take, which is a worse
//     outcome than any clock error.
//   * **Externally jam-synced.** No protocol field reports this, so it is
//     inferred: if writes stop taking, something else is driving the camera.
//     After max_failures in a row, back off rather than fight it.
//   * **Already close enough.** Below the trigger threshold -- half a frame by
//     default, scaled to whatever rate the camera reports -- leave it alone.
//   * **Written recently.** At most one write per min_write_interval. A
//     half-frame threshold means nearly every cycle wants to write, and
//     re-jamming every minute would destroy the very thing the log exists to
//     measure: the free-running stretches drift is computed from.
#ifndef OCTO_CAMSYNC_H
#define OCTO_CAMSYNC_H

#include <string>

#include "bmd.h"

namespace octo {

struct SyncOptions {
  double poll = 60.0;
  double listen = 8.0;

  // The trigger threshold, in frames unless a seconds value overrides it.
  // Frames rather than seconds because "close enough" means different things
  // at 24 and 60 fps, and because a frame is the camera's own unit: it reports
  // whole frames, so below half a frame there is nothing left to resolve.
  bool has_tolerance = false;
  double tolerance = 0.0;
  double tolerance_frames = 0.5;

  // How close a write has to land to count as having taken. Deliberately
  // looser than the trigger threshold: this asks "did the write land where we
  // asked", not "is the clock perfect now". Judging a write against half a
  // frame would mark every good write a failure, and max_failures of those in
  // a row is exactly what makes the daemon stop writing altogether.
  double write_tolerance = 1.0;

  double min_write_interval = 3600.0;

  int rtc_bias = 0;
  bool adapt_bias = true;
  int max_bias_step = 120;
  int max_adapts = 4;

  // --- how often to look -------------------------------------------------
  //
  // `poll` is the floor, not the schedule. Once drift has been measured, the
  // interval stretches towards the moment something could actually need doing
  // and shortens again as that moment approaches; see next_poll().
  bool adaptive_poll = true;
  double max_poll = 900.0;
  // How many observations to take across the wait. Each poll sleeps 1/N of the
  // remaining wait, so the cadence is coarse when the next action is far off
  // and tightens as it nears, without ever overshooting it by more than one
  // interval.
  int poll_slices = 4;
  // A floor under the drift figure used for scheduling. A clock that measured
  // 2 ppm this afternoon is not a clock that will hold 2 ppm all night --
  // temperature alone moves these by more than that.
  double min_assumed_ppm = 5.0;
  // An error jump larger than this is not drift; it is the camera having been
  // switched off and on again. See observe().
  double restart_step = 1.0;

  double min_drift_interval = 1800.0;
  double lead = 0.05;
  double verify_wait = 3.0;
  double camera_wait = 6.0;
  double scan_timeout = 20.0;
  double connect_timeout = 15.0;
  int max_failures = 3;
  double bench_spread = 0.5;
  int fps = 24;
  bool dry_run = false;
};

// A drift figure good enough to schedule against, learned from a completed
// free-running stretch. Not the same thing as the per-cycle Drift below: this
// one survives writes, and only a camera restart clears it.
struct DriftEstimate {
  bool has = false;
  double ppm = 0.0;
  double span = 0.0;   // the lever arm it was measured over
  int samples = 0;     // how many stretches have contributed
};

// Everything carried between cycles. Kept in one struct so a test can put the
// daemon into any state -- five failures deep, mid-bias-adaptation, an hour
// since the last write -- without running a daemon.
struct SyncState {
  int failures = 0;
  int adapts = 0;
  int rtc_bias = 0;

  bool has_last_write = false;
  double last_write_mono = 0.0;

  // The previous observation, for drift across one poll interval.
  bool has_last_obs = false;
  double last_obs_mono = 0.0;
  double last_obs_error = 0.0;
  bool wrote_since_obs = false;

  // The last *write*, for drift across a whole free-running stretch.
  bool has_anchor = false;
  double anchor_mono = 0.0;
  double anchor_error = 0.0;

  DriftEstimate drift;

  std::string camera_id;
};

// Throw away everything measured against the camera's old clock.
//
// A power cycle resets the camera's RTC, so every drift figure taken across it
// is measuring the step, not the clock. It also clears the failure counters:
// if the daemon had decided an external source owned this camera, a camera
// that has just been switched on deserves to be asked again.
//
// The learned RTC bias deliberately survives. It is the only thing here that
// took hours to acquire -- a bias adjustment costs a write, and writes are
// rationed -- and if the power cycle did change it, judge_write() will find
// that out on the next write anyway.
void forget_drift(SyncState* state);

enum class Action {
  kWrite,
  kSkipRecording,
  kSkipExternal,
  kSkipInTolerance,
  kSkipRateLimited,
  kSkipDryRun,
};

const char* action_name(Action a);

struct Decision {
  Action action = Action::kWrite;
  double tolerance = 0.0;
  double since_write = 0.0;
  std::string message;  // the console line, ready to print
};

// How far off the camera has to be before a write is worth making, in seconds.
double trigger_tolerance(const SyncOptions& opt, int fps);

Decision decide(const SyncOptions& opt, const SyncState& state, double error,
                int fps, bool recording, double now_mono);

// What one observation says about how fast the clock is walking.
struct Drift {
  // Since the previous observation. Logged but rarely shown: consecutive
  // observations are a poll interval apart, so their difference is almost pure
  // frame quantisation -- 42 ms across 60 s invents 700 ppm of nothing.
  bool has_step = false;
  double step_ppm = 0.0;
  double step_dt = 0.0;
  bool step_shown = false;

  // Since the last write. This anchor only moves when the clock is actually
  // set, so the lever arm grows all the way to the next write, which is what
  // makes an hourly write rate worth having.
  bool has_anchor = false;
  double anchor_ppm = 0.0;
  double anchor_span = 0.0;
  bool anchor_shown = false;

  // The camera's clock moved further in one interval than drift can explain.
  // Everything learned about the old clock has been discarded.
  bool restarted = false;
  double restart_step = 0.0;
};

// Fold one observation into the state and report what it implies about drift.
Drift observe(const SyncOptions& opt, SyncState* state, double error,
              double now_mono);

enum class Verdict {
  kOk,        // the write landed, or at least improved matters
  kAdapting,  // it missed, but the bias still has attempts left
  kNoEffect,  // it missed with the bias out of attempts: count a failure
};

struct WriteOutcome {
  Verdict verdict = Verdict::kOk;
  bool verified = false;
  int bias_before = 0;
  int bias_after = 0;
  bool bias_changed = false;
  std::string message;
};

// Judge a completed write and update the state, including the learned bias.
//
// "The error did not change" is NOT evidence the write was ignored: if the
// camera was already sitting where the write puts it, a perfectly applied
// write moves nothing. That trap is what made the original probe conclude
// group 7.0 was unimplemented. So a residual is treated as the bias being
// wrong and fed back, and only a write that still misses after the bias has
// had a fair chance to converge counts as a failure.
WriteOutcome judge_write(const SyncOptions& opt, SyncState* state,
                         double error_before, double error_after,
                         double now_mono);

// --- timing a write so it lands on a second boundary -----------------------
//
// The RTC field is whole seconds, so writing "now" at an arbitrary moment
// throws away the fraction and lands up to a second slow. Instead pick the
// next whole second in Tentacle time, wait until this Mac's clock reaches the
// instant it corresponds to, and send then. What is left is BLE latency, which
// is tens of milliseconds rather than half a second -- which is why sub-frame
// accuracy is reachable at all despite the whole-second field.

// Seconds to wait before sending, given the current wall clock.
double aligned_wait(double now_unix, double offset, double bias, double lead);

// The UTC value to put in the packet, given the instant the write is sent.
//
// Rounds to the nearest second rather than truncating: the send is deliberately
// `lead` early, so the value is a hair below the whole second aimed at, and
// truncating throws away a whole second and lands the camera ~1 s slow every
// single time.
bmd::Civil aligned_value(double send_unix, double offset, double bias);

// --- how long to wait before looking again ---------------------------------

// The drift figure to schedule against, in ppm, widened for what the
// measurement cannot resolve.
//
// The camera reports whole frames, so a figure measured over `span` carries a
// frame of quantisation at each end -- half an hour of watching at 24 fps
// cannot distinguish 25 ppm from 48. Scheduling against the fast end of that
// range costs a few extra wakeups; scheduling against the middle means
// arriving late, which is the failure that matters.
double drift_bound_ppm(const SyncOptions& opt, const DriftEstimate& est,
                       int fps);

struct PollPlan {
  double seconds = 60.0;
  // When the clock could next be both wrong enough to matter and allowed to be
  // written. Zero means "now", which is the ordinary case.
  double until_actionable = 0.0;
  const char* reason = "floor";  // floor | drift | rate-limit | fixed
  std::string message;           // the console line, empty when unremarkable
};

// How long to sleep before the next cycle.
//
// Two things have to be true before a write can happen: the clock has to be
// wrong by more than the trigger threshold, and the rate limit has to have
// lapsed. Whichever is further away sets the horizon, and the interval is one
// slice of it -- so a camera that has just been corrected is left alone for
// most of the hour, and one about to cross the threshold is watched closely.
//
// Never longer than max_poll and never shorter than poll: the first bounds how
// long a camera can be unattended after something unexpected, and the second
// is what a caller asking for a fixed cadence gets.
PollPlan next_poll(const SyncOptions& opt, const SyncState& state, double error,
                   int fps, double now_mono);

// A duration a human reads at a glance: "38s", "2m", "1.0h".
std::string format_span(double seconds);

}  // namespace octo

#endif  // OCTO_CAMSYNC_H
