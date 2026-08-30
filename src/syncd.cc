#include "syncd.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <utility>

#include "timeutil.h"

namespace octo {

namespace {

// How old a camera reading may be and still be treated as a statement about
// now. The same figure src/camsync.h caps reading_age_s at, and for the same
// reason: past it the correction stops being a correction and the sample
// should be waited out rather than believed.
constexpr double kMaxReadingAge = 5.0;

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return s;
}

std::string text(const char* f, ...) __attribute__((format(printf, 1, 2)));

std::string text(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return std::string(buf);
}

}  // namespace

BenchView measure_bench(const Snapshot& snap, const CamConf* conf) {
  BenchView bench;
  std::vector<double> votes;
  for (const DeviceSnapshot& d : snap.device) {
    if (!d.live || !d.has_time) continue;
    if (conf != nullptr && !conf->box_enabled(d.id)) {
      ++bench.skipped;
      continue;
    }
    votes.push_back(d.median_offset);
  }
  if (votes.empty()) return bench;

  bench.ok = true;
  bench.offset = median_offset(votes);
  const auto range = std::minmax_element(votes.begin(), votes.end());
  bench.spread = *range.second - *range.first;
  bench.boxes = static_cast<int>(votes.size());
  return bench;
}

namespace {

const char* phase_name(int phase) {
  switch (phase) {
    case 0: return "stopped";
    case 1: return "idle";
    case 2: return "scanning";
    case 3: return "connecting";
    case 4: return "subscribing";
    case 5: return "observing";
    case 6: return "source";
    case 7: return "aligning";
    case 8: return "writing";
    case 9: return "verifying";
  }
  return "unknown";
}

}  // namespace

SyncDaemon::SyncDaemon(Loop* loop, Registry* registry, SyncdOptions opt)
    : loop_(loop),
      registry_(registry),
      opt_(std::move(opt)),
      wall_([] { return wall_now(); }),
      alive_(new bool(true)) {}

SyncDaemon::~SyncDaemon() {
  // Before anything else: every completion and every timer still out there
  // checks this, and from here on they all decline.
  *alive_ = false;
  cancel_step();
  if (cycle_timer_ != kNoTimer) loop_->cancel(cycle_timer_);
  if (announce_timer_ != kNoTimer) loop_->cancel(announce_timer_);
  // The camera is owned by whoever made it and may outlive this, so the
  // handlers it is holding must stop pointing here.
  if (camera_ != nullptr) {
    camera_->set_view_handler(nullptr);
    camera_->set_disconnect_handler(nullptr);
  }
}

void SyncDaemon::set_camera(AsyncCamera* camera) {
  if (camera_ == camera) return;
  if (camera_ != nullptr) {
    camera_->set_view_handler(nullptr);
    camera_->set_disconnect_handler(nullptr);
  }
  camera_ = camera;
  if (camera_ == nullptr) return;

  std::shared_ptr<bool> alive = alive_;
  camera_->set_view_handler([this, alive](const CameraView& view) {
    if (*alive) on_view(view);
  });
  camera_->set_disconnect_handler([this, alive]() {
    if (*alive) on_camera_gone();
  });
}

void SyncDaemon::set_config(const CamConf* conf) { conf_ = conf; }

void SyncDaemon::set_wall_clock(std::function<double()> wall) {
  if (wall) wall_ = std::move(wall);
}

void SyncDaemon::set_state(const SyncState& state) { state_ = state; }
void SyncDaemon::on_cycle(CycleHandler handler) { on_cycle_ = std::move(handler); }
void SyncDaemon::on_bind(BindHandler handler) { on_bind_ = std::move(handler); }
void SyncDaemon::on_say(SayHandler handler) { on_say_ = std::move(handler); }
void SyncDaemon::on_settime(TimeHandler handler) {
  on_time_ = std::move(handler);
}

double SyncDaemon::wall() const { return wall_(); }
double SyncDaemon::mono() const { return loop_->now(); }

bool SyncDaemon::busy() const {
  return phase_ != Phase::kIdle && phase_ != Phase::kStopped;
}

void SyncDaemon::start() {
  if (started_) return;
  started_ = true;
  phase_ = Phase::kIdle;

  // Posted rather than run. start() is called from somebody's frame and a
  // cycle is entitled to call straight back into this object; the rule that a
  // handler never runs inside the call that registered it is worth keeping
  // even for the one call that is not a completion.
  schedule_next(0.0, "start");

  if (opt_.announce && opt_.announce_period > 0.0) {
    std::shared_ptr<bool> alive = alive_;
    announce_timer_ = loop_->every(opt_.announce_period, [this, alive]() {
      if (*alive) tick_announce();
    });
  }
}

void SyncDaemon::stop() {
  if (!started_) return;
  started_ = false;
  phase_ = Phase::kStopped;
  // Nothing in flight belongs to a cycle any more, so every completion still
  // out there fails current() and declines.
  cur_.seq = 0;
  cancel_step();
  if (cycle_timer_ != kNoTimer) {
    loop_->cancel(cycle_timer_);
    cycle_timer_ = kNoTimer;
  }
  if (announce_timer_ != kNoTimer) {
    loop_->cancel(announce_timer_);
    announce_timer_ = kNoTimer;
  }
  if (camera_ != nullptr && camera_->connected()) camera_->disconnect();
}

// --------------------------------------------------------------- scheduling

void SyncDaemon::schedule_next(double seconds, const char* reason) {
  (void)reason;
  if (!started_) return;
  if (seconds < 0.0) seconds = 0.0;
  if (cycle_timer_ != kNoTimer) loop_->cancel(cycle_timer_);
  next_cycle_at_ = mono() + seconds;
  std::shared_ptr<bool> alive = alive_;
  cycle_timer_ = loop_->after(seconds, [this, alive]() {
    if (!*alive) return;
    cycle_timer_ = kNoTimer;
    begin_cycle();
  });
}

void SyncDaemon::arm_step(double seconds, std::function<void()> fn) {
  cancel_step();
  if (seconds < 0.0) seconds = 0.0;
  std::shared_ptr<bool> alive = alive_;
  const uint64_t seq = cur_.seq;
  step_timer_ = loop_->after(seconds, [this, alive, seq, fn]() {
    if (!*alive || !current(seq)) return;
    step_timer_ = kNoTimer;
    fn();
  });
}

void SyncDaemon::cancel_step() {
  if (step_timer_ == kNoTimer) return;
  loop_->cancel(step_timer_);
  step_timer_ = kNoTimer;
}

bool SyncDaemon::current(uint64_t seq) const {
  return phase_ != Phase::kStopped && seq != 0 && seq == cur_.seq;
}

AsyncCamera::DoneHandler SyncDaemon::guard_done(
    void (SyncDaemon::*fn)(bool, const std::string&)) {
  std::shared_ptr<bool> alive = alive_;
  const uint64_t seq = cur_.seq;
  return [this, alive, seq, fn](bool ok, const std::string& err) {
    if (!*alive || !current(seq)) return;
    // The step's own deadline was a backstop against a backend that never
    // answers. It has answered, so the backstop goes before anything it could
    // interrupt runs.
    cancel_step();
    (this->*fn)(ok, err);
  };
}

// -------------------------------------------------------------------- cycle

void SyncDaemon::begin_cycle() {
  if (!started_) return;

  cur_ = Cycle();
  cur_.seq = next_seq_++;
  cur_.forced = pending_force_;
  cur_.set_source = pending_source_;
  cur_.source_value = pending_source_value_;
  cur_.want_camera = pending_camera_.empty() ? opt_.camera : pending_camera_;
  pending_sync_ = false;
  pending_force_ = false;
  pending_source_ = false;
  pending_camera_.clear();

  const Snapshot snap = registry_->snapshot(mono(), wall());
  cur_.bench = measure_bench(snap, conf_);
  cur_.report.bench = cur_.bench;
  cur_.offset = opt_.tentacle_bench ? cur_.bench.offset : 0.0;

  if (opt_.tentacle_bench && !cur_.bench.ok) {
    // Not the same complaint as silence, and worth separating: somebody
    // switched these off, and the fix is in the configuration rather than in
    // the room.
    const std::string why =
        cur_.bench.skipped > 0
            ? text("nothing to sync to -- every timecode box heard (%d) is"
                   " switched off",
                   cur_.bench.skipped)
            : std::string("no timecode boxes heard -- nothing to sync to");
    say(why);
    finish_cycle("skip:no-tentacle", why);
    return;
  }

  if (camera_ == nullptr) {
    // Not a failure and not worth saying every minute: this is the ordinary
    // state of a daemon on a host with no asynchronous camera backend, which
    // today is every Mac. It still hears boxes and still serves the roster.
    finish_cycle("skip:no-camera-backend",
                 "no camera backend on this host -- listening only");
    return;
  }

  if (cur_.bench.spread > opt_.sync.bench_spread) {
    say(text("WARNING: timecode boxes disagree by %.3fs -- not all jammed to"
             " the same source",
             cur_.bench.spread));
  }

  cur_.looked = true;
  step_connect();
}

void SyncDaemon::step_connect() {
  phase_ = Phase::kConnecting;

  // `want` names the camera this cycle is for: the configured one, or whatever
  // a client asked for. An id that is already what we are bound to is the fast
  // path; anything else has to be looked for, because the only way to tell a
  // name from a body is to see it advertise.
  const bool bound_is_wanted =
      cur_.want_camera.empty() ||
      (!state_.camera_id.empty() && cur_.want_camera == state_.camera_id);

  // Still connected from a previous cycle, and still subscribed. This is what
  // holding buys: no scan, no connect, and -- with no bond storage -- no
  // pairing.
  if (bound_is_wanted && camera_->connected() && camera_->subscribed()) {
    misses_ = 0;
    step_observe();
    return;
  }

  if (bound_is_wanted && !state_.camera_id.empty()) {
    connect_to(state_.camera_id, std::string());
    return;
  }

  // Something the radio has already heard advertising. This is the cheap half
  // of the question a twenty-second scan answers expensively, and the radio is
  // listening anyway.
  const Snapshot snap = registry_->snapshot(mono(), wall());
  if (cur_.want_camera.empty() && snap.camera.present && !snap.camera.id.empty()) {
    connect_to(snap.camera.id, snap.camera.name);
    return;
  }

  step_scan();
}

void SyncDaemon::connect_to(const std::string& id, const std::string& name) {
  phase_ = Phase::kConnecting;
  cur_.try_id = id;
  cur_.try_name = name;
  camera_->connect(id, opt_.sync.connect_timeout,
                   guard_done(&SyncDaemon::done_connect));
  // A backend that never answers must not wedge the daemon. The completion
  // cancels this; see guard_done.
  arm_step(opt_.sync.connect_timeout * 2.0 + 1.0, [this]() {
    ++misses_;
    finish_cycle("skip:no-camera", "the camera backend never answered");
  });
}

void SyncDaemon::done_connect(bool ok, const std::string& err) {
  if (!ok) {
    if (!cur_.tried_scan) {
      say(text("direct connect to %s failed (%s) -- rescanning",
               cur_.try_id.substr(0, 8).c_str(), err.c_str()));
      step_scan();
      return;
    }
    ++misses_;
    finish_cycle("skip:no-camera", "connect failed: " + err);
    return;
  }

  const std::string id = cur_.try_id;
  const bool new_body = state_.camera_id != id;
  if (!state_.camera_id.empty() && new_body) {
    // A different body. Nothing measured about the last one's clock says
    // anything about this one's.
    forget_drift(&state_);
  }
  state_.camera_id = id;
  misses_ = 0;
  cur_.report.camera_id = id;
  if (new_body && on_bind_) on_bind_(id, cur_.try_name, &state_);
  step_subscribe();
}

void SyncDaemon::step_scan() {
  phase_ = Phase::kScanning;
  cur_.tried_scan = true;
  say(text("scanning %.0fs for Blackmagic cameras...", opt_.sync.scan_timeout));

  std::shared_ptr<bool> alive = alive_;
  const uint64_t seq = cur_.seq;
  camera_->scan(opt_.sync.scan_timeout, cur_.want_camera, false,
                [this, alive, seq](const ScanResult& result) {
                  if (!*alive || !current(seq)) return;
                  cancel_step();
                  on_scan(result);
                });
  arm_step(opt_.sync.scan_timeout * 2.0 + 1.0, [this]() {
    ++misses_;
    finish_cycle("skip:no-camera", "the scan never finished");
  });
}

void SyncDaemon::on_scan(const ScanResult& result) {
  const CameraDevice* pick = nullptr;
  const std::string want = lower(cur_.want_camera);
  for (const CameraDevice& dev : result.cameras) {
    if (want.empty()) {
      pick = &dev;
      break;
    }
    if (lower(dev.name).find(want) != std::string::npos || lower(dev.id) == want) {
      pick = &dev;
      break;
    }
  }

  if (pick == nullptr) {
    ++misses_;
    // What the scan did see is the difference between a radio that is not
    // working and a camera that is not on: no LE devices at all usually means
    // this program has not been granted the radio.
    const std::string why =
        result.total == 0
            ? std::string("no LE devices at all -- the radio may be blocked"
                          " for this program")
            : text("no Blackmagic cameras among %d LE devices", result.total);
    say(why);
    finish_cycle("skip:no-camera", why);
    return;
  }

  connect_to(pick->id, pick->name);
}

void SyncDaemon::step_subscribe() {
  phase_ = Phase::kSubscribing;
  if (camera_->subscribed()) {
    step_observe();
    return;
  }
  camera_->subscribe(opt_.sync.camera_wait,
                     guard_done(&SyncDaemon::done_subscribe));
  arm_step(opt_.sync.camera_wait * 2.0 + 1.0, [this]() {
    finish_cycle("skip:no-characteristics",
                 "the camera never finished subscribing");
  });
}

void SyncDaemon::done_subscribe(bool ok, const std::string& err) {
  if (!ok) {
    say("connected, but " + err);
    finish_cycle("skip:no-characteristics", err);
    return;
  }
  step_observe();
}

void SyncDaemon::step_observe() {
  phase_ = Phase::kObserving;
  cur_.report.reached_camera = true;

  // The timecode-source errand is the one that does not need a reading, and
  // waiting for one would be worse than pointless: in the mode this exists to
  // escape, the camera parks its timecode and stops, so the daemon would wait
  // out the whole deadline for a notification that is never coming and then do
  // the write anyway.
  if (cur_.set_source) {
    evaluate();
    return;
  }

  const CameraView& view = camera_->view();
  const bool fresh = view.has_timecode &&
                     mono() - view.timecode_mono <= kMaxReadingAge;
  if (fresh && view.has_transport) {
    evaluate();
    return;
  }
  // Wait to be told. A camera that is connected sends a timecode about once a
  // second; one that sends nothing at all is a real outcome and is what the
  // deadline reports.
  arm_step(opt_.sync.camera_wait, [this]() { evaluate(); });
}

void SyncDaemon::evaluate() {
  cancel_step();
  const CameraView view = camera_->view();
  // The camera's own frame rate when it has said one. Everything downstream is
  // scaled to it -- half a frame is a different tolerance at 24 and at 60 --
  // so a guess here is a wrong tolerance rather than a rounding error.
  const int fps = view.has_fps ? view.fps : opt_.sync.fps;
  cur_.report.fps = fps;

  // Permission, read once and applied to everything this cycle could do.
  // "Writes are disabled" has to cover the timecode source as well as the
  // clock, or the setting would be a promise the program does not keep.
  cur_.may_write = conf_ != nullptr ? conf_->writes_enabled(state_.camera_id)
                                    : opt_.default_writes;

  cur_.report.recording =
      view.has_transport && view.transport == bmd::kTransportRecord;
  cur_.report.has_timecode_source = view.has_timecode_source;
  cur_.report.timecode_source = view.timecode_source;

  // Writing 4.7 is the one errand that does not care what the clock says, so
  // it is answered before a timecode is required -- in the mode this exists to
  // escape, the camera's timecode is exactly what has stopped being
  // informative.
  if (cur_.set_source) {
    step_source_write();
    return;
  }

  if (!view.has_timecode) {
    const std::string why = "camera connected but sent no timecode";
    say(why);
    finish_cycle("skip:no-timecode", why);
    return;
  }

  const double now = mono();
  const double error = error_from(view, fps);
  cur_.report.has_error = true;
  cur_.report.error_before = error;
  cur_.report.timecode = bmd::format_timecode(view.timecode);

  const Drift drift = observe(opt_.sync, &state_, error, now);
  if (drift.restarted) {
    say(text("camera's clock jumped %+.3fs -- that is a power cycle, not"
             " drift; forgetting what was measured about the old one",
             drift.restart_step));
  }

  Conditions cond;
  cond.recording = cur_.report.recording;
  cond.has_timecode_source = view.has_timecode_source;
  cond.timecode_source = view.timecode_source;
  cond.writes_enabled = cur_.may_write;

  std::string drift_note;
  if (drift.anchor_shown) {
    drift_note = text("  drift %+.1f ppm over %s", drift.anchor_ppm,
                      format_span(drift.anchor_span).c_str());
  } else if (drift.step_shown) {
    drift_note = text("  drift %+.1f ppm", drift.step_ppm);
  }
  if (opt_.tentacle_bench) {
    say(text("tentacles %+.3fs (%d boxes, spread %.3fs) | camera %s err"
             " %+.3fs%s",
             cur_.offset, cur_.bench.boxes, cur_.bench.spread,
             cur_.report.timecode.c_str(), error, drift_note.c_str()));
  } else {
    say(text("this host | camera %s err %+.3fs%s", cur_.report.timecode.c_str(),
             error, drift_note.c_str()));
  }

  Decision decision = decide(opt_.sync, state_, error, fps, cond, now);

  // Someone asked for this by hand. That overrules the gates that mean "there
  // is no need" and none of the ones that mean "must not" -- see
  // gate_is_advisory. The original verdict is still what gets logged if it
  // stands, so a forced write does not quietly erase the fact that the daemon
  // would not have made it on its own.
  if (cur_.forced && decision.action != Action::kWrite &&
      gate_is_advisory(decision.action)) {
    say(text("  forced: overruling %s", action_name(decision.action)));
    decision.action = Action::kWrite;
  }

  if (decision.action != Action::kWrite) {
    say(decision.message);
    finish_cycle(action_name(decision.action), decision.message);
    return;
  }

  cur_.bias = state_.rtc_bias;
  cur_.lead = effective_lead(opt_.sync, state_);
  step_align();
}

void SyncDaemon::step_source_write() {
  phase_ = Phase::kSourceWrite;
  if (!cur_.may_write) {
    const std::string why =
        "writes are disabled for this camera -- enable it with"
        " `octomancer writes on`";
    say("  gate: " + why);
    finish_cycle("skip:writes-disabled", why);
    return;
  }
  const std::vector<uint8_t> packet = bmd::build_packet(
      bmd::kGroupOutput, bmd::kParamTimecodeSource, bmd::kTypeInt8,
      bmd::kOpAssign, {static_cast<uint8_t>(cur_.source_value)});
  camera_->write_control(packet, 10.0, guard_done(&SyncDaemon::done_source));
  arm_step(11.0, [this]() {
    finish_cycle("source:unanswered", "the camera never answered the write");
  });
}

void SyncDaemon::done_source(bool ok, const std::string& err) {
  if (!ok) {
    say("  timecode source write rejected: " + err);
    finish_cycle("source:rejected", err);
    return;
  }
  // The camera echoes a change back on the Incoming Control characteristic, so
  // the write is not believed until the echo says so: a GATT ack only proves
  // the bytes were taken.
  arm_step(1.5, [this]() {
    const CameraView after = camera_->view();
    const bool echoed = after.has_timecode_source &&
                        after.timecode_source == cur_.source_value;
    cur_.report.has_timecode_source = after.has_timecode_source;
    cur_.report.timecode_source = after.timecode_source;
    say(text("  timecode source -> %lld%s",
             static_cast<long long>(cur_.source_value),
             echoed ? " (echoed back)" : " (no echo; unverified)"));
    finish_cycle(echoed ? "source:ok" : "source:unverified",
                 echoed ? text("timecode source is now %lld",
                               static_cast<long long>(cur_.source_value))
                        : std::string("the camera did not echo the change"
                                      " back"));
  });
}

void SyncDaemon::step_align() {
  phase_ = Phase::kAligning;
  // The RTC field is whole seconds, so writing "now" at an arbitrary moment
  // throws away the fraction and lands up to a second slow. Wait for the
  // instant that corresponds to the next whole second in bench time -- and
  // wait for it on the loop, because a daemon that sleeps here is a daemon
  // that stops answering for a second every hour.
  const double wait = aligned_wait(wall(), cur_.offset, cur_.bias, cur_.lead);
  arm_step(wait, [this]() { step_write(); });
}

void SyncDaemon::step_write() {
  phase_ = Phase::kWriting;
  const double send_at = wall();
  const bmd::Civil when = aligned_value(send_at, cur_.offset, cur_.bias);
  cur_.report.wrote_value = when;
  cur_.report.has_write = true;
  cur_.report.bias = cur_.bias;
  cur_.report.lead = cur_.lead;
  cur_.write_started = mono();

  camera_->write_control(bmd::rtc_packet(when, 0), 10.0,
                         guard_done(&SyncDaemon::done_write));
  arm_step(11.0, [this]() {
    state_.failures += 1;
    finish_cycle("write:unanswered", "the camera never answered the write");
  });
}

void SyncDaemon::done_write(bool ok, const std::string& err) {
  if (!ok) {
    say("  write rejected: " + err);
    state_.failures += 1;
    finish_cycle("write:rejected", err);
    return;
  }

  cur_.report.write_latency = mono() - cur_.write_started;
  cur_.report.wrote = true;
  state_.has_last_write = true;
  state_.last_write_mono = mono();
  say(text("  wrote RTC %02d:%02d:%02d UTC (bias %+ds, %.0fms lead, %.0fms"
           " latency)",
           cur_.report.wrote_value.hour, cur_.report.wrote_value.minute,
           cur_.report.wrote_value.second, cur_.bias, cur_.lead * 1000.0,
           cur_.report.write_latency * 1000.0));

  // A GATT ack proves the characteristic took the bytes and nothing more, so
  // verify against the camera's own clock. The notifications never stopped, so
  // drop the stale reading and let a fresh one arrive.
  camera_->forget_timecode();
  arm_step(opt_.sync.verify_wait, [this]() { step_verify(); });
}

void SyncDaemon::step_verify() {
  phase_ = Phase::kVerifying;
  if (camera_->view().has_timecode) {
    judge();
    return;
  }
  arm_step(opt_.sync.camera_wait, [this]() { judge(); });
}

void SyncDaemon::judge() {
  cancel_step();
  const CameraView view = camera_->view();
  if (!view.has_timecode) {
    const std::string why = "could not verify: no timecode after the write";
    say("  " + why);
    state_.failures += 1;
    finish_cycle("write:unverified", why);
    return;
  }

  const double after = error_from(view, cur_.report.fps);
  cur_.report.has_error_after = true;
  cur_.report.error_after = after;
  cur_.report.timecode = bmd::format_timecode(view.timecode);

  const WriteOutcome outcome =
      judge_write(opt_.sync, &state_, cur_.report.error_before, after, mono());
  cur_.report.verified = outcome.verified;
  cur_.report.timing_usable = outcome.timing_usable;
  say(outcome.message);

  const char* action = "write:ok";
  switch (outcome.verdict) {
    case Verdict::kOk:
      if (outcome.bias_changed) {
        say(text("  learned: RTC bias %+ds -> %+ds", outcome.bias_before,
                 outcome.bias_after));
      }
      break;
    case Verdict::kAdapting:
      action = "write:adapting";
      break;
    case Verdict::kNoEffect:
      action = "write:no-effect";
      say("  something else may be driving this camera's timecode");
      break;
  }
  finish_cycle(action, outcome.message);
}

void SyncDaemon::finish_cycle(const std::string& action,
                              const std::string& message) {
  cancel_step();
  phase_ = Phase::kIdle;

  CycleReport& rep = cur_.report;
  rep.action = action;
  rep.message = message;
  rep.bench = cur_.bench;
  last_action_ = action;

  // Let go of the camera -- unless we are holding it, which is the default.
  // Timecode only arrives over a live connection, so a daemon that holds one
  // for twenty seconds an hour is blind for the other fifty-nine minutes and
  // every cycle then opens by waiting for a reading it could have had.
  if (camera_ != nullptr && !opt_.hold && camera_->connected()) {
    camera_->disconnect();
  }

  // When to look again. A cycle that never reached the camera learns nothing
  // about when to come back, so the floor stands -- except when the reason it
  // did not reach the camera is that there was no camera, which is the one
  // case that has its own answer.
  double seconds = opt_.sync.poll;
  const char* reason = "floor";
  if (rep.reached_camera && rep.has_error) {
    const double latest =
        rep.has_error_after ? rep.error_after : rep.error_before;
    const PollPlan plan =
        next_poll(opt_.sync, state_, latest, rep.fps, mono());
    seconds = plan.seconds;
    reason = plan.reason;
    if (!plan.message.empty()) say(plan.message);
  } else if (cur_.looked && !rep.reached_camera && misses_ > 0) {
    seconds = reacquire_interval(opt_.sync, misses_);
    reason = "reacquire";
  }
  rep.next_poll = seconds;
  rep.next_poll_reason = reason;

  if (on_cycle_) on_cycle_(rep, &state_);

  Message msg;
  msg.verb = "cycle";
  msg.set("action", rep.action);
  if (!rep.message.empty()) msg.set("why", rep.message);
  if (rep.bench.ok) {
    msg.set_double("bench", rep.bench.offset);
    msg.set_int("boxes", rep.bench.boxes);
  }
  if (rep.has_error) msg.set_double("err", rep.error_before);
  if (rep.has_error_after) msg.set_double("err_after", rep.error_after);
  if (rep.wrote) msg.set_bool("wrote", true);
  if (rep.wrote) msg.set_bool("verified", rep.verified);
  msg.set_double("next", rep.next_poll, 1);
  announce(msg);

  schedule_next(seconds, reason);

  // A request that arrived while this cycle was running gets its own cycle
  // rather than waiting out the poll interval.
  if (pending_sync_ || pending_source_) schedule_next(0.0, "requested");
}

// ------------------------------------------------------------------- camera

void SyncDaemon::on_view(const CameraView& view) {
  if (phase_ == Phase::kObserving) {
    const bool fresh = view.has_timecode &&
                       mono() - view.timecode_mono <= kMaxReadingAge;
    if (fresh && view.has_transport) evaluate();
    return;
  }
  if (phase_ == Phase::kVerifying && view.has_timecode) {
    judge();
    return;
  }
}

void SyncDaemon::on_camera_gone() {
  if (camera_present_) {
    camera_present_ = false;
    Message msg;
    msg.verb = "cam";
    msg.set_bool("up", false);
    msg.set("id", state_.camera_id);
    announce(msg);
  }
  if (!busy()) return;
  // Whatever this cycle was waiting for is not coming. Ending it here is the
  // difference between a daemon that notices and one that sits in kObserving
  // until a deadline it armed for a camera that has left the room.
  ++misses_;
  finish_cycle("skip:camera-gone", "the camera disconnected");
}

double SyncDaemon::error_from(const CameraView& view, int fps) const {
  const double now = mono();
  const double age = reading_age_s(now, view.timecode_mono, kMaxReadingAge);
  const double centre = opt_.sync.centre_frames ? frame_centre_s(fps) : 0.0;
  const double cam = bmd::timecode_sod(view.timecode, fps) + centre;
  const double want = local_seconds_of_day(wall() - age) + cur_.offset;
  return wrap_delta(cam - want);
}

// -------------------------------------------------------------------- radio

void SyncDaemon::observe_advert(const Advert& advert) {
  registry_->observe(advert.id, advert.name, advert.rssi, advert.data.data(),
                     advert.data.size(), advert.mono, advert.wall);
  drain_events();
}

void SyncDaemon::observe_camera(const Sighting& sighting) {
  registry_->observe_camera(sighting.id, sighting.name, sighting.rssi,
                            sighting.mono, sighting.wall);
  if (!camera_present_) {
    camera_present_ = true;
    misses_ = 0;
    Message msg;
    msg.verb = "cam";
    msg.set_bool("up", true);
    msg.set("id", sighting.id);
    if (!sighting.name.empty()) msg.set("name", sighting.name);
    msg.set_int("rssi", sighting.rssi);
    announce(msg);
  }
}

void SyncDaemon::set_radio_state(const std::string& state) {
  registry_->set_radio(state);
  Message msg;
  msg.verb = "radio";
  msg.set("state", state);
  announce(msg);
}

// ------------------------------------------------------------------ talking

void SyncDaemon::say(const std::string& line) {
  if (on_say_) on_say_(line);
}

void SyncDaemon::send(MsgPeer* peer, const Message& msg) {
  if (peer == nullptr) return;
  peer->send(encode(msg));
}

void SyncDaemon::announce(const Message& msg) {
  if (!opt_.announce) return;
  const std::string line = encode(msg);
  for (MsgPeer* peer : peers_) {
    if (std::find(quiet_.begin(), quiet_.end(), peer) != quiet_.end()) continue;
    peer->send(line);
  }
}

void SyncDaemon::drain_events() {
  for (const AlertEvent& event : registry_->take_events()) {
    Message msg;
    msg.verb = "alert";
    msg.set("id", event.id);
    msg.set("name", event.name);
    msg.set_double("offset", event.offset);
    msg.set_bool("drifted", event.entering);
    if (event.repeat) msg.set_bool("repeat", true);
    announce(msg);
  }
}

void SyncDaemon::update_camera_presence(const Snapshot& snap) {
  const bool connected = camera_ != nullptr && camera_->connected();
  const bool present = connected || snap.camera.present;
  if (present == camera_present_) return;
  camera_present_ = present;

  Message msg;
  msg.verb = "cam";
  msg.set_bool("up", present);
  const std::string id =
      snap.camera.id.empty() ? state_.camera_id : snap.camera.id;
  if (!id.empty()) msg.set("id", id);
  if (present) {
    if (!snap.camera.name.empty()) msg.set("name", snap.camera.name);
    msg.set_int("rssi", snap.camera.rssi);
  }
  announce(msg);
}

void SyncDaemon::tick_announce() {
  drain_events();
  const Snapshot snap = registry_->snapshot(mono(), wall());
  // A camera that has been switched off sends nothing at all: there is no
  // event to notice, only a silence to time, and this is where it is timed.
  // Doing it per advertisement instead would mean building a whole snapshot
  // once a second to learn something that changes twice a day.
  update_camera_presence(snap);
  const BenchView bench = measure_bench(snap, conf_);
  Message msg;
  msg.verb = "bench";
  msg.set_bool("ok", bench.ok);
  if (bench.ok) {
    msg.set_double("offset", bench.offset);
    msg.set_double("spread", bench.spread);
    msg.set_int("boxes", bench.boxes);
  }
  if (bench.skipped > 0) msg.set_int("disabled", bench.skipped);
  msg.set_int("live", snap.live);
  announce(msg);
}

Message SyncDaemon::device_message(const DeviceSnapshot& dev) const {
  Message msg;
  msg.verb = "dev";
  msg.set("id", dev.id);
  if (!dev.name.empty()) msg.set("name", dev.name);
  msg.set_int("rssi", dev.rssi);
  msg.set_bool("live", dev.live);
  msg.set_double("age", dev.age, 1);
  if (dev.has_time) {
    msg.set_double("offset", dev.offset);
    msg.set_double("median", dev.median_offset);
    msg.set_int("samples", dev.samples);
  }
  if (dev.has_drift) msg.set_double("ppm", dev.drift_ppm, 2);
  if (conf_ != nullptr && !conf_->box_enabled(dev.id)) {
    msg.set_bool("enabled", false);
  }
  if (dev.alerting) msg.set_bool("alerting", true);
  return msg;
}

Message SyncDaemon::status_message() const {
  const Snapshot snap = registry_->snapshot(mono(), wall());
  const BenchView bench = measure_bench(snap, conf_);

  Message msg;
  msg.verb = "status";
  msg.set("phase", phase_name(static_cast<int>(phase_)));
  msg.set("radio", snap.radio);
  msg.set_double("uptime", snap.uptime, 1);
  msg.set_int("devices", snap.devices);
  msg.set_int("live", snap.live);
  msg.set_bool("bench", bench.ok);
  if (bench.ok) {
    msg.set_double("offset", bench.offset);
    msg.set_double("spread", bench.spread);
    msg.set_int("boxes", bench.boxes);
  }
  if (bench.skipped > 0) msg.set_int("disabled", bench.skipped);

  // A camera stops advertising while something holds a connection to it, so a
  // connection of our own counts as presence. Without that the daemon would
  // report the camera missing for exactly as long as it was talking to it.
  const bool connected = camera_ != nullptr && camera_->connected();
  msg.set_bool("camera", connected || snap.camera.present);
  msg.set_bool("connected", connected);
  if (!state_.camera_id.empty()) msg.set("camera_id", state_.camera_id);
  msg.set_bool("backend", camera_ != nullptr);
  if (!last_action_.empty()) msg.set("action", last_action_);
  if (state_.has_last_write) {
    msg.set_double("since_write", mono() - state_.last_write_mono, 1);
  }
  if (state_.drift.has) msg.set_double("ppm", state_.drift.ppm, 2);
  if (state_.lead.has) msg.set_double("lead", state_.lead.lead_s);
  msg.set_int("bias", state_.rtc_bias);
  msg.set_int("failures", state_.failures);
  if (started_) msg.set_double("next", next_cycle_at_ - mono(), 1);
  return msg;
}

// --------------------------------------------------------------- the peers

void SyncDaemon::peer_opened(MsgPeer* peer) {
  if (peer == nullptr) return;
  peers_.push_back(peer);
  Message hello;
  hello.verb = "hello";
  hello.set_int("proto", kBoxProtocolVersion);
  hello.set("role", "sync");
#ifdef OCTO_VERSION
  hello.set("version", OCTO_VERSION);
#endif
  send(peer, hello);
}

void SyncDaemon::peer_closed(MsgPeer* peer) {
  peers_.erase(std::remove(peers_.begin(), peers_.end(), peer), peers_.end());
  quiet_.erase(std::remove(quiet_.begin(), quiet_.end(), peer), quiet_.end());
}

void SyncDaemon::peer_line(MsgPeer* peer, const std::string& line) {
  Message msg;
  std::string err;
  if (!decode(line, &msg, &err)) {
    Message bad;
    bad.verb = "err";
    bad.set("reason", "bad-line");
    bad.set("detail", err);
    send(peer, bad);
    return;
  }
  handle(peer, msg);
}

void SyncDaemon::handle(MsgPeer* peer, const Message& msg) {
  // A request may carry an id, and every reply to it carries the same one
  // back. Nothing here needs it -- one line in, one or more lines out, in
  // order -- but a client multiplexing several questions over one serial cable
  // does, and adding it later would be a change to every verb.
  const std::string tag = msg.get("id");
  auto reply = [&](Message out) {
    if (!tag.empty()) out.set("id", tag);
    send(peer, out);
  };

  if (msg.verb == "ping") {
    Message out;
    out.verb = "pong";
    reply(out);
    return;
  }

  if (msg.verb == "hello") {
    Message out;
    out.verb = "hello";
    out.set_int("proto", kBoxProtocolVersion);
    out.set("role", "sync");
#ifdef OCTO_VERSION
    out.set("version", OCTO_VERSION);
#endif
    reply(out);
    return;
  }

  if (msg.verb == "time") {
    double when = 0.0;
    if (!msg.get_double("wall", &when)) {
      Message bad;
      bad.verb = "err";
      bad.set("reason", "missing-field");
      bad.set("field", "wall");
      reply(bad);
      return;
    }
    // A daemon that already knows the time says so rather than taking the
    // correction. Accepting it would make a Mac's roster silently depend on
    // whatever the last client felt like sending, and the mistake -- pushing
    // the time at the wrong end of the cable -- would leave no trace.
    if (!on_time_) {
      Message bad;
      bad.verb = "err";
      bad.set("reason", "have-clock");
      reply(bad);
      return;
    }
    WallTime set;
    set.wall = when;
    int64_t zone = 0;
    if (msg.get_int("zone", &zone)) {
      // A zone the box cannot represent is a host bug, and taking it would
      // move every offset by a plausible-looking amount. The real range is
      // -12:00 to +14:00; this is that, rounded outwards to a whole day.
      if (zone <= -86400 || zone >= 86400) {
        Message bad;
        bad.verb = "err";
        bad.set("reason", "bad-zone");
        bad.set_int("zone", zone);
        reply(bad);
        return;
      }
      set.has_zone = true;
      set.zone = static_cast<int>(zone);
    }
    on_time_(set);

    Message out;
    out.verb = "ok";
    out.set("what", "time");
    // What the clock reads now, which is the acknowledgement worth having: it
    // is the value actually adopted, and the round trip is visible in it.
    out.set_double("wall", wall(), 3);
    // Echoed so a host can see whether the zone landed. A box that took the
    // instant and dropped the zone reads as seven hours out, which looks like
    // a broken box rather than a missing field.
    if (set.has_zone) out.set_int("zone", set.zone);
    reply(out);
    return;
  }

  if (msg.verb == "status") {
    reply(status_message());
    return;
  }

  if (msg.verb == "devices") {
    const Snapshot snap = registry_->snapshot(mono(), wall());
    for (const DeviceSnapshot& dev : snap.device) reply(device_message(dev));
    Message end;
    end.verb = "end";
    end.set("what", "devices");
    end.set_int("n", static_cast<int64_t>(snap.device.size()));
    reply(end);
    return;
  }

  if (msg.verb == "sync") {
    bool force = false;
    msg.get_bool("force", &force);
    const bool was_busy = busy();
    request_sync(msg.get("camera"), force);
    Message out;
    out.verb = "ok";
    out.set("what", "sync");
    // "Accepted" and "will happen now" stop being the same statement the
    // moment there is a queue, so say which one this is.
    out.set_bool("queued", was_busy);
    reply(out);
    return;
  }

  if (msg.verb == "source") {
    int64_t value = 0;
    if (!msg.get_int("value", &value)) {
      Message bad;
      bad.verb = "err";
      bad.set("reason", "missing-field");
      bad.set("field", "value");
      reply(bad);
      return;
    }
    const bool was_busy = busy();
    request_source(msg.get("camera"), value);
    Message out;
    out.verb = "ok";
    out.set("what", "source");
    out.set_int("value", value);
    out.set_bool("queued", was_busy);
    reply(out);
    return;
  }

  if (msg.verb == "announce") {
    bool on = true;
    msg.get_bool("on", &on);
    quiet_.erase(std::remove(quiet_.begin(), quiet_.end(), peer), quiet_.end());
    if (!on) quiet_.push_back(peer);
    Message out;
    out.verb = "ok";
    out.set("what", "announce");
    // What the peer asked for, and what it is actually going to get. Those
    // differ when announcements are switched off for the whole daemon, and
    // answering "on=1" there would be a peer waiting all night for lines that
    // were never going to come.
    out.set_bool("on", on);
    const bool effective = on && opt_.announce;
    if (effective != on) out.set_bool("effective", effective);
    reply(out);
    return;
  }

  if (msg.verb == "forget") {
    const std::string dev = msg.get("dev");
    if (dev.empty()) {
      Message bad;
      bad.verb = "err";
      bad.set("reason", "missing-field");
      bad.set("field", "dev");
      reply(bad);
      return;
    }
    const bool had = registry_->forget(dev);
    Message out;
    out.verb = "ok";
    out.set("what", "forget");
    out.set("dev", dev);
    out.set_bool("known", had);
    reply(out);
    return;
  }

  // Unknown verbs are answered rather than dropped. A Mac and a box will be
  // running different versions almost all of the time, and a request that
  // vanishes is indistinguishable from a daemon that has stopped.
  Message bad;
  bad.verb = "err";
  bad.set("reason", "unknown-verb");
  bad.set("verb", msg.verb);
  reply(bad);
}

void SyncDaemon::request_sync(const std::string& camera, bool force) {
  pending_sync_ = true;
  pending_force_ = pending_force_ || force;
  if (!camera.empty()) pending_camera_ = camera;
  if (!started_ || busy()) return;
  schedule_next(0.0, "requested");
}

void SyncDaemon::request_source(const std::string& camera, int64_t value) {
  pending_source_ = true;
  pending_source_value_ = value;
  if (!camera.empty()) pending_camera_ = camera;
  if (!started_ || busy()) return;
  schedule_next(0.0, "requested");
}

}  // namespace octo
