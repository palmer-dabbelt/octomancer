// The sync daemon, driven to completion with no radio and no wall-clock time.
//
// Everything here runs on src/loopfake.h: the clock is a variable, the camera
// is a scripted object that answers on that clock, and the Tentacle boxes are
// bytes handed to the registry. So a whole cycle -- connect, subscribe, wait
// for a reading, decide, wait for the second boundary, write, wait, verify --
// happens between two statements, and the awkward cases that are hard to
// produce on a bench are the cheap ones here: a camera that answers a connect
// and then goes silent, one that vanishes mid-cycle, a write that is taken and
// changes nothing.
//
// The fake camera holds the one rule that matters to the daemon's correctness
// and is easy to violate by accident: **a completion never runs inside the
// call that registered it.** Every answer is posted to the loop. A fake that
// called back immediately would let the daemon pass tests that the real
// asynchronous backends would fail.
#include "../src/syncd.h"

#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "../src/bmd.h"
#include "../src/loopfake.h"
#include "../src/tentacle.h"
#include "../src/timeutil.h"
#include "harness.h"

using namespace octo;

namespace {

// ---------------------------------------------------------------- the fakes

class FakeCamera : public AsyncCamera {
 public:
  explicit FakeCamera(FakeLoop* loop) : loop_(loop) {}

  // --- the script ---------------------------------------------------------
  bool connect_ok = true;
  bool subscribe_ok = true;
  bool write_ok = true;
  // "Never answers", which is a real failure mode: a controller that takes a
  // command and then goes quiet.
  bool answer_connect = true;
  bool answer_subscribe = true;
  bool answer_write = true;
  bool answer_scan = true;
  double delay = 0.05;
  ScanResult scan_result;

  // --- what happened ------------------------------------------------------
  int connects = 0;
  int subscribes = 0;
  int scans = 0;
  int disconnects = 0;
  std::string last_connect_id;
  std::vector<std::vector<uint8_t>> writes;
  std::vector<double> write_monos;

  // --- AsyncCamera --------------------------------------------------------
  void set_view_handler(ViewHandler on_change) override {
    on_view_ = std::move(on_change);
  }
  void set_disconnect_handler(std::function<void()> on_gone) override {
    on_gone_ = std::move(on_gone);
  }

  void scan(double seconds, const std::string& hint, bool want_all,
            ScanHandler done) override {
    (void)hint;
    (void)want_all;
    ++scans;
    if (!answer_scan) return;
    const ScanResult result = scan_result;
    post(seconds, [done, result]() { done(result); });
  }

  void connect(const std::string& id, double timeout,
               DoneHandler done) override {
    (void)timeout;
    ++connects;
    last_connect_id = id;
    if (!answer_connect) return;
    const bool ok = connect_ok;
    post(delay, [this, done, ok]() {
      if (ok) connected_ = true;
      done(ok, ok ? std::string() : std::string("no such camera"));
    });
  }

  void disconnect() override {
    ++disconnects;
    connected_ = false;
    subscribed_ = false;
    view_ = CameraView();
  }

  bool connected() const override { return connected_; }

  void subscribe(double timeout, DoneHandler done) override {
    (void)timeout;
    ++subscribes;
    if (!answer_subscribe) return;
    const bool ok = subscribe_ok;
    post(delay, [this, done, ok]() {
      if (ok) subscribed_ = true;
      done(ok, ok ? std::string()
                  : std::string("no control characteristic"));
    });
  }

  bool subscribed() const override { return subscribed_; }

  void write_control(const std::vector<uint8_t>& packet, double timeout,
                     DoneHandler done) override {
    (void)timeout;
    writes.push_back(packet);
    write_monos.push_back(loop_->now());
    if (!answer_write) return;
    const bool ok = write_ok;
    post(delay, [done, ok]() {
      done(ok, ok ? std::string() : std::string("write rejected"));
    });
  }

  const CameraView& view() const override { return view_; }
  void forget_timecode() override { view_.has_timecode = false; }

  // --- what the camera says -----------------------------------------------
  void report_timecode(double sod, int fps) {
    view_.has_timecode = true;
    const int total = static_cast<int>(sod);
    view_.timecode.hours = (total / 3600) % 24;
    view_.timecode.minutes = (total / 60) % 60;
    view_.timecode.seconds = total % 60;
    view_.timecode.frames =
        static_cast<int>((sod - static_cast<double>(total)) * fps);
    view_.timecode_mono = loop_->now();
    fire();
  }
  void report_transport(int64_t mode) {
    view_.has_transport = true;
    view_.transport = mode;
    fire();
  }
  void report_fps(int fps) {
    view_.has_fps = true;
    view_.fps = fps;
    fire();
  }
  void report_source(int64_t value) {
    view_.has_timecode_source = true;
    view_.timecode_source = value;
    fire();
  }
  // The camera walks out of the room.
  void vanish() {
    connected_ = false;
    subscribed_ = false;
    view_ = CameraView();
    if (on_gone_) on_gone_();
  }

 private:
  // Everything the camera answers is posted rather than returned, because that
  // is the contract src/camasync.h states and the daemon is entitled to.
  template <class F>
  void post(double in, F fn) {
    loop_->after(in < 0.0 ? 0.0 : in, fn);
  }
  void fire() {
    if (on_view_) on_view_(view_);
  }

  FakeLoop* loop_ = nullptr;
  ViewHandler on_view_;
  std::function<void()> on_gone_;
  CameraView view_;
  bool connected_ = false;
  bool subscribed_ = false;
};

class FakePeer : public MsgPeer {
 public:
  void send(const std::string& line) override { lines.push_back(line); }

  // The last message with this verb, decoded. Verbs rather than positions
  // because announcements interleave with replies by design.
  bool last(const std::string& verb, Message* out) const {
    for (size_t i = lines.size(); i > 0; --i) {
      Message msg;
      std::string err;
      if (!decode(lines[i - 1], &msg, &err)) continue;
      if (msg.verb != verb) continue;
      *out = msg;
      return true;
    }
    return false;
  }
  int count(const std::string& verb) const {
    int n = 0;
    for (const std::string& line : lines) {
      Message msg;
      std::string err;
      if (decode(line, &msg, &err) && msg.verb == verb) ++n;
    }
    return n;
  }

  std::vector<std::string> lines;
};

// A 0x32 Tentacle payload used to be built here, byte by byte. It is
// octo::encode_micros now -- a second copy of a wire format is a second thing
// to keep in step with the decoder, and this one was written before there was
// an encoder to use.
std::string temp_path(const char* tag) {
  return "/tmp/octo-syncd-" + std::to_string(getpid()) + "-" + tag + ".conf";
}

// ------------------------------------------------------------- the fixture

struct Rig {
  static constexpr double kMono0 = 1000.0;

  FakeLoop loop{kMono0};
  Registry registry;
  FakeCamera camera{&loop};
  SyncdOptions opt;
  std::unique_ptr<SyncDaemon> daemon;
  std::vector<CycleReport> cycles;
  double wall0 = 0.0;
  // What the camera's clock is wrong by, in seconds. The rig reports readings
  // from it, so a test can set a camera four seconds fast and then set it
  // right the way a successful write would.
  double camera_error = 0.0;
  int fps = 24;

  Rig() : registry(default_policy(), kMono0) {
    // A wall instant whose local time of day is known to this process and far
    // from midnight is not something a test can arrange, so it takes the one
    // it has -- but truncated to a whole second, which matters more than it
    // looks.
    //
    // This used to be `wall_now()` outright, with a comment saying only
    // differences mattered below. They do not: a write is *aligned* to land on
    // a whole second of wall-clock time, so the fraction of a second the rig
    // starts on decides where in each cycle the write falls. The suite was
    // therefore sampling one random phase per run and failing on about six per
    // cent of them -- 17 failures in 300 runs, all of them a verification that
    // arrived before the daemon was listening for it.
    //
    // Truncating fixes the phase at zero. The phases that are no longer
    // sampled by accident are swept on purpose in
    // test_a_write_lands_on_a_boundary_at_any_phase.
    wall0 = std::floor(wall_now());
    opt.sync.poll = 60.0;
    opt.sync.scan_timeout = 2.0;
    opt.sync.connect_timeout = 2.0;
    opt.sync.camera_wait = 2.0;
    opt.sync.verify_wait = 0.5;
    // Seconds rather than frames, so that a test's idea of "close enough" is
    // not a frame of quantisation away from the daemon's.
    opt.sync.has_tolerance = true;
    opt.sync.tolerance = 0.5;
    opt.default_writes = true;
    opt.announce_period = 0.0;  // no heartbeat unless a test asks for one
  }

  static Policy default_policy() {
    Policy policy;
    policy.stale_after = 30.0;
    return policy;
  }

  // False builds a daemon with no camera backend at all, which is what a Mac
  // with no dongle really has and is not a degraded state: it still hears
  // boxes, still serves the roster, and still says why it is not syncing.
  bool with_camera = true;

  void build() {
    daemon.reset(new SyncDaemon(&loop, &registry, opt));
    if (with_camera) daemon->set_camera(&camera);
    daemon->set_wall_clock([this]() { return wall(); });
    daemon->on_cycle([this](const CycleReport& report, SyncState*) {
      cycles.push_back(report);
    });
  }

  double wall() const { return wall0 + (loop.now() - kMono0); }

  // `n` boxes agreeing on an offset, heard often enough to be live and to have
  // a median worth the name.
  void feed_boxes(int n, double offset) {
    for (int i = 0; i < 12; ++i) {
      const double mono = loop.now() - 6.0 + i * 0.5;
      const double w = wall0 + (mono - kMono0);
      for (int b = 0; b < n; ++b) {
        const std::vector<uint8_t> pkt =
            octo::encode_micros(local_seconds_of_day(w) + offset);
        Advert advert;
        advert.id = "box" + std::to_string(b);
        advert.name = "Tentacle_" + std::to_string(b);
        advert.rssi = -40;
        advert.data = pkt;
        advert.mono = mono;
        advert.wall = w;
        daemon->observe_advert(advert);
      }
    }
  }

  // What the camera would report right now, given how wrong its clock is.
  void report_now(double bench_offset) {
    report_timecode_for(bench_offset + camera_error);
  }
  void report_timecode_for(double error_from_host) {
    camera.report_timecode(local_seconds_of_day(wall()) + error_from_host, fps);
  }

  // A camera on the air. The daemon prefers this to a scan, which is the
  // whole point of the radio noticing cameras it will never touch: it is the
  // cheap half of a question a twenty-second scan answers expensively.
  void see_camera(const std::string& id = "CAM-1") {
    Sighting seen;
    seen.id = id;
    seen.name = "Pocket 6K";
    seen.rssi = -50;
    seen.mono = loop.now();
    seen.wall = wall();
    daemon->observe_camera(seen);
  }

  // Drive one whole cycle to the point where the camera is connected,
  // subscribed and has said everything a camera says.
  void reach_camera(double bench_offset, int64_t transport = 0) {
    see_camera();
    loop.advance(0.5);
    camera.report_fps(fps);
    camera.report_source(bmd::kTimecodeSourceTimeOfDay);
    camera.report_transport(transport);
    report_now(bench_offset);
  }

  // A reference rather than a pointer, and an empty report when there has
  // been no cycle: a test that expected one and did not get it should say so
  // on the line that expected it, not take the whole suite down with a null
  // dereference three lines later.
  const CycleReport& last() const {
    static const CycleReport none;
    return cycles.empty() ? none : cycles.back();
  }
  // Null-safe, because a test that has not produced a cycle should report that
  // rather than dereference nothing and take the suite down with it.
  std::string last_action() const {
    return cycles.empty() ? std::string("(no cycle)") : cycles.back().action;
  }
};

// ------------------------------------------------------------------- tests

void test_bench_is_the_median_of_enabled_boxes() {
  Rig rig;
  rig.build();
  rig.feed_boxes(3, -6.25);

  const Snapshot snap = rig.registry.snapshot(rig.loop.now(), rig.wall());
  const BenchView bench = measure_bench(snap, nullptr);
  CHECK(bench.ok);
  CHECK_EQ(bench.boxes, 3);
  CHECK_NEAR(bench.offset, -6.25, 1e-3);
  CHECK(bench.spread < 1e-3);
  CHECK_EQ(bench.skipped, 0);
}

void test_a_disabled_box_does_not_vote() {
  const std::string path = temp_path("boxes");
  {
    std::ofstream out(path, std::ios::trunc);
    out << "box box0 enabled=off\n";
  }
  CamConf conf;
  std::string err;
  CHECK(conf.load(path, &err));

  Rig rig;
  rig.build();
  rig.feed_boxes(2, -3.0);

  const Snapshot snap = rig.registry.snapshot(rig.loop.now(), rig.wall());
  const BenchView bench = measure_bench(snap, &conf);
  CHECK(bench.ok);
  // One vote left, and the one that was dismissed is counted rather than
  // silently absent: "no boxes" and "every box is switched off" are different
  // situations.
  CHECK_EQ(bench.boxes, 1);
  CHECK_EQ(bench.skipped, 1);
  ::unlink(path.c_str());
}

void test_no_boxes_means_the_camera_is_not_touched() {
  Rig rig;
  rig.build();
  rig.daemon->start();
  rig.loop.advance(1.0);

  CHECK_EQ(rig.camera.connects, 0);
  CHECK_EQ(rig.camera.scans, 0);
  CHECK_STR(rig.last_action(), "skip:no-tentacle");
  // ...and it still schedules the next look, which is the failure that would
  // otherwise be a clock nobody corrected all night.
  CHECK_NEAR(rig.last().next_poll, 60.0, 1e-6);
}

void test_a_cycle_writes_on_a_second_boundary() {
  Rig rig;
  rig.camera_error = 4.0;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.reach_camera(-6.0);
  rig.loop.advance(3.0);

  CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 1);
  if (rig.camera.writes.empty()) return;

  // The RTC field is whole seconds, so the value has to be sent early enough
  // that it arrives on a boundary in bench time. That is the whole reason
  // there is an aligning state, and it is the property worth pinning: the
  // instant of the write, plus the lead it was sent with, plus the bench
  // offset, is a whole second.
  const double send_wall = rig.wall0 + (rig.camera.write_monos[0] - Rig::kMono0);
  const double lead = effective_lead(rig.opt.sync, rig.daemon->state());
  const double target = send_wall + lead + (-6.0);
  CHECK_NEAR(target - std::floor(target + 0.5), 0.0, 1e-6);

  // And the packet is an RTC write rather than anything else.
  CHECK_EQ(static_cast<int>(rig.camera.writes[0].size() > 8), 1);
}

// `aligned_wait` exists so that a write lands on a whole second of wall-clock
// time whatever fraction of a second the daemon started on. That property was
// only ever sampled -- one random phase per run, from the real clock -- and
// this sweeps it.
//
// The verification here is driven the way a camera really behaves, by reporting
// timecode repeatedly rather than once at a moment the test guessed. That is
// what makes it phase-independent: a test that speaks once has to know when the
// daemon is listening, and a test that keeps speaking does not.
void test_a_write_lands_on_a_boundary_at_any_phase() {
  for (int i = 0; i < 10; ++i) {
    const double frac = i / 10.0;
    Rig rig;
    rig.wall0 = std::floor(rig.wall0) + frac;
    rig.camera_error = 4.0;
    rig.build();
    rig.feed_boxes(3, -6.0);
    rig.daemon->start();
    rig.reach_camera(-6.0);
    rig.loop.advance(2.0);
    CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 1);
    if (rig.camera.writes.empty()) continue;

    const double send_wall =
        rig.wall0 + (rig.camera.write_monos[0] - Rig::kMono0);
    const double lead = effective_lead(rig.opt.sync, rig.daemon->state());
    const double target = send_wall + lead + (-6.0);
    CHECK_NEAR(target - std::floor(target + 0.5), 0.0, 1e-6);

    // The write landed, and the camera says so for as long as it takes.
    rig.camera_error = 0.0;
    for (int k = 0; k < 60 && rig.cycles.empty(); ++k) {
      rig.report_now(-6.0);
      rig.loop.advance(0.1);
    }
    CHECK_STR(rig.last_action(), "write:ok");
    CHECK(rig.last().verified);
  }
}

void test_a_good_write_is_verified() {
  Rig rig;
  rig.camera_error = 4.0;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.reach_camera(-6.0);
  rig.loop.advance(2.0);
  CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 1);

  // The write landed: the camera's clock is right now.
  rig.camera_error = 0.0;
  rig.loop.advance(0.6);
  rig.report_now(-6.0);
  rig.loop.advance(0.1);

  CHECK_STR(rig.last_action(), "write:ok");
  CHECK(rig.last().verified);
  CHECK_EQ(rig.daemon->state().failures, 0);
  CHECK(rig.last().has_error_after);
  CHECK_NEAR(rig.last().error_after, 0.0, 0.05);
}

void test_a_write_that_changes_nothing_is_not_called_verified() {
  Rig rig;
  rig.camera_error = 4.0;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.reach_camera(-6.0);
  rig.loop.advance(2.0);
  CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 1);

  // The camera ignored it and is still four seconds out.
  rig.loop.advance(0.6);
  rig.report_now(-6.0);
  rig.loop.advance(0.1);

  CHECK(!rig.cycles.empty());
  CHECK(!rig.last().verified);
  // Four seconds is a whole-second miss, so the bias is what gets blamed
  // first and the daemon adapts rather than counting a failure. That is
  // judge_write's rule and this only checks the daemon honours the verdict.
  CHECK(rig.last().action == "write:adapting" ||
        rig.last().action == "write:no-effect");
}

void test_recording_is_never_written_over() {
  Rig rig;
  rig.camera_error = 4.0;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.reach_camera(-6.0, bmd::kTransportRecord);
  rig.loop.advance(2.0);

  CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 0);
  CHECK_STR(rig.last_action(), "skip:recording");
}

void test_writes_disabled_stops_everything() {
  Rig rig;
  rig.camera_error = 4.0;
  rig.opt.default_writes = false;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.reach_camera(-6.0);
  rig.loop.advance(2.0);

  CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 0);
  CHECK_STR(rig.last_action(), "skip:writes-disabled");
}

void test_a_clock_that_is_close_enough_is_left_alone() {
  Rig rig;
  rig.camera_error = 0.0;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.reach_camera(-6.0);
  rig.loop.advance(2.0);

  CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 0);
  CHECK_STR(rig.last_action(), "skip:in-tolerance");
}

void test_forcing_overrules_an_advisory_gate_only() {
  {
    // In tolerance is advisory: somebody standing in front of the camera
    // knows better than the threshold.
    Rig rig;
    rig.camera_error = 0.0;
    rig.build();
    rig.feed_boxes(3, -6.0);
    rig.daemon->request_sync(std::string(), true);
    rig.daemon->start();
    rig.reach_camera(-6.0);
    rig.loop.advance(2.0);
    CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 1);
  }
  {
    // Recording is not. No amount of asking makes corrupting a take right.
    Rig rig;
    rig.camera_error = 4.0;
    rig.build();
    rig.feed_boxes(3, -6.0);
    rig.daemon->request_sync(std::string(), true);
    rig.daemon->start();
    rig.reach_camera(-6.0, bmd::kTransportRecord);
    rig.loop.advance(2.0);
    CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 0);
    CHECK_STR(rig.last_action(), "skip:recording");
  }
}

void test_a_camera_that_sends_no_timecode_ends_the_cycle() {
  Rig rig;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.see_camera();
  rig.loop.advance(0.5);
  rig.camera.report_transport(0);   // connected, but never a timecode
  rig.loop.advance(3.0);

  CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 0);
  CHECK_STR(rig.last_action(), "skip:no-timecode");
  CHECK(rig.last().reached_camera);
}

void test_a_backend_that_never_answers_does_not_wedge_the_daemon() {
  Rig rig;
  rig.camera.answer_connect = false;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.see_camera();
  rig.loop.advance(20.0);

  // The connect was made and never answered. The daemon gave up on its own
  // and scheduled another look rather than sitting in kConnecting forever.
  CHECK_EQ(rig.camera.connects, 1);
  CHECK_STR(rig.last_action(), "skip:no-camera");
  CHECK(rig.last().next_poll > 0.0);
}

void test_a_scan_finds_a_camera_when_nothing_is_known() {
  Rig rig;
  CameraDevice dev;
  dev.id = "CAM-1";
  dev.name = "Pocket 6K";
  dev.by_service_uuid = true;
  rig.camera.scan_result.cameras.push_back(dev);
  rig.camera.scan_result.total = 4;
  rig.camera_error = 4.0;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.loop.advance(3.0);

  CHECK_EQ(rig.camera.scans, 1);
  CHECK_EQ(rig.camera.connects, 1);
  CHECK_STR(rig.camera.last_connect_id, "CAM-1");
  CHECK_STR(rig.daemon->state().camera_id, "CAM-1");
}

void test_a_camera_that_is_not_there_backs_off() {
  Rig rig;
  rig.camera.scan_result.total = 9;  // other LE devices, but no camera
  // A short cadence, so that the second look happens while the boxes this
  // rig fed are still live. The property is the doubling, not the size.
  rig.opt.sync.poll = 2.0;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.loop.advance(3.0);

  CHECK_EQ(static_cast<int>(rig.cycles.size()), 1);
  CHECK_STR(rig.cycles[0].action, "skip:no-camera");
  const double first = rig.cycles[0].next_poll;
  CHECK_STR(rig.cycles[0].next_poll_reason, "reacquire");

  // Miss again and the interval grows: a camera missing for an hour is
  // switched off, and looking for it every minute until morning costs radio
  // and finds nothing.
  rig.loop.advance(first + 6.0);
  CHECK(static_cast<int>(rig.cycles.size()) >= 2);
  CHECK(rig.cycles[1].next_poll > first);
}

void test_a_camera_that_vanishes_mid_cycle_ends_the_cycle() {
  Rig rig;
  rig.camera_error = 4.0;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.see_camera();
  rig.loop.advance(0.5);
  CHECK(rig.camera.connected());

  rig.camera.vanish();
  rig.loop.advance(0.1);

  CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 0);
  CHECK_STR(rig.last_action(), "skip:camera-gone");
  CHECK(rig.last().next_poll > 0.0);
}

void test_a_completion_from_an_abandoned_cycle_is_ignored() {
  Rig rig;
  rig.camera.delay = 1.0;  // slow enough to still be in flight
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.see_camera();
  rig.loop.advance(0.1);
  CHECK_EQ(rig.camera.connects, 1);

  // The daemon is stopped with a connect outstanding. When the answer finally
  // arrives it belongs to a cycle that no longer exists, and acting on it
  // would drive a state machine that has been told to stand still.
  rig.daemon->stop();
  rig.loop.advance(3.0);

  CHECK_EQ(rig.camera.subscribes, 0);
  CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 0);
}

void test_destroying_the_daemon_with_work_in_flight_is_safe() {
  Rig rig;
  rig.camera.delay = 1.0;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.see_camera();
  rig.loop.advance(0.1);
  CHECK_EQ(rig.camera.connects, 1);

  // The completion and two timers are still out there and all of them capture
  // the daemon. Every one of them checks the liveness flag first.
  rig.daemon.reset();
  rig.loop.advance(5.0);
  CHECK_EQ(rig.camera.subscribes, 0);
}

void test_holding_the_camera_avoids_a_second_connect() {
  Rig rig;
  rig.camera_error = 0.0;
  rig.opt.hold = true;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.reach_camera(-6.0);
  rig.loop.advance(1.0);
  CHECK_EQ(rig.camera.connects, 1);
  CHECK_EQ(rig.camera.disconnects, 0);
  CHECK(rig.camera.connected());

  // The next cycle finds the camera already connected and subscribed, which
  // is what holding is for: no scan, no connect, no pairing.
  rig.daemon->request_sync(std::string(), false);
  rig.loop.advance(0.5);
  rig.report_now(-6.0);
  rig.loop.advance(0.5);
  CHECK_EQ(rig.camera.connects, 1);
  CHECK_EQ(rig.camera.scans, 0);
  CHECK(static_cast<int>(rig.cycles.size()) >= 2);
}

void test_not_holding_releases_the_camera() {
  Rig rig;
  rig.camera_error = 0.0;
  rig.opt.hold = false;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.reach_camera(-6.0);
  rig.loop.advance(1.0);
  CHECK_EQ(rig.camera.disconnects, 1);
  CHECK(!rig.camera.connected());
}

// ----------------------------------------------------------- the protocol

void test_a_peer_is_greeted() {
  Rig rig;
  rig.build();
  FakePeer peer;
  rig.daemon->peer_opened(&peer);

  Message hello;
  CHECK(peer.last("hello", &hello));
  CHECK_STR(hello.get("role"), "sync");
  int64_t proto = 0;
  CHECK(hello.get_int("proto", &proto));
  CHECK_EQ(proto, static_cast<int64_t>(kBoxProtocolVersion));
}

void test_ping_and_unknown_verbs_are_both_answered() {
  Rig rig;
  rig.build();
  FakePeer peer;
  rig.daemon->peer_opened(&peer);

  rig.daemon->peer_line(&peer, "ping id=7");
  Message pong;
  CHECK(peer.last("pong", &pong));
  // The tag comes back, so a client with several questions on one cable can
  // tell the answers apart.
  CHECK_STR(pong.get("id"), "7");

  // An unknown verb is answered rather than dropped: a Mac and a box run
  // different versions almost all the time, and a request that vanishes looks
  // exactly like a daemon that has stopped.
  rig.daemon->peer_line(&peer, "flurb x=1");
  Message err;
  CHECK(peer.last("err", &err));
  CHECK_STR(err.get("reason"), "unknown-verb");
  CHECK_STR(err.get("verb"), "flurb");

  rig.daemon->peer_line(&peer, "   ");
  CHECK(peer.last("err", &err));
  CHECK_STR(err.get("reason"), "bad-line");
}

// Being told what time it is. Only a host with no clock of its own accepts
// this -- see the note on on_settime in src/syncd.h.
void test_time_is_refused_by_a_daemon_that_has_a_clock() {
  Rig rig;
  rig.build();
  FakePeer peer;
  rig.daemon->peer_opened(&peer);

  // No handler installed, which is every daemon running on a Mac. Pushing the
  // time at that end is a mistake, and one that would leave no trace if it
  // were quietly obeyed.
  rig.daemon->peer_line(&peer, "time wall=1700000000.5 id=4");
  Message err;
  CHECK(peer.last("err", &err));
  CHECK_STR(err.get("reason"), "have-clock");
  CHECK_STR(err.get("id"), "4");
}


// The zone the box cannot work out for itself.
//
// A Tentacle broadcasts a local time of day, so an offset is only meaningful
// against a host that knows which local. A dongle has no timezone database --
// picolibc answers UTC and means it -- so the host has to say, and this is
// where it says it. Getting it wrong is not a subtle failure: every box on the
// bench reads as a whole UTC offset out, which looks like broken hardware.
void test_time_carries_the_zone_as_well_as_the_instant() {
  Rig rig;
  rig.build();
  SyncDaemon::WallTime got;
  int calls = 0;
  rig.daemon->on_settime([&](const SyncDaemon::WallTime& t) {
    got = t;
    ++calls;
    rig.wall0 = t.wall - (rig.loop.now() - Rig::kMono0);
  });
  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->peer_line(&peer, "time wall=1700000000.5 zone=-25200");

  CHECK_EQ(calls, 1);
  CHECK(got.has_zone);
  CHECK_EQ(got.zone, -25200);
  CHECK_NEAR(got.wall, 1700000000.5, 1e-6);

  Message ok;
  CHECK(peer.last("ok", &ok));
  CHECK_STR(ok.get("what"), "time");
  // Echoed, so a host can tell a box that took the zone from one that ignored
  // it. Without the echo those two look identical until the offsets come back
  // seven hours out.
  int64_t zone = 0;
  CHECK(ok.get_int("zone", &zone));
  CHECK_EQ(zone, -25200LL);
}

// A host that says nothing about the zone is not the same as one that says
// zero, and the box needs to be able to tell: zero is a real answer (a bench
// in London in winter) and "unset" is not.
void test_time_without_a_zone_leaves_the_zone_unsaid() {
  Rig rig;
  rig.build();
  SyncDaemon::WallTime got;
  got.has_zone = true;  // so a handler that never writes it cannot pass
  rig.daemon->on_settime([&](const SyncDaemon::WallTime& t) { got = t; });
  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->peer_line(&peer, "time wall=1700000000.5");

  CHECK(!got.has_zone);
  CHECK_EQ(got.zone, 0);
  Message ok;
  CHECK(peer.last("ok", &ok));
  CHECK_STR(ok.get("zone"), "");
}

// A zone outside any zone the Earth has is a host bug. Taking it would shift
// every reading by a plausible-looking amount, so it is refused outright and
// the clock is left alone rather than half-set.
void test_an_impossible_zone_is_refused() {
  Rig rig;
  rig.build();
  int calls = 0;
  rig.daemon->on_settime([&](const SyncDaemon::WallTime&) { ++calls; });
  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->peer_line(&peer, "time wall=1700000000.5 zone=90000");

  CHECK_EQ(calls, 0);
  Message err;
  CHECK(peer.last("err", &err));
  CHECK_STR(err.get("reason"), "bad-zone");
  CHECK(!peer.last("ok", &err));
}

void test_time_sets_the_clock_of_a_daemon_that_has_none() {
  Rig rig;
  rig.build();
  // What firmware/src/boxclock.h does: hold the difference between the
  // monotonic clock, which is trustworthy from the first instant, and the wall
  // clock, which is unknown until somebody says.
  rig.daemon->on_settime([&rig](const SyncDaemon::WallTime& t) {
    rig.wall0 = t.wall - (rig.loop.now() - Rig::kMono0);
  });
  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->peer_line(&peer, "time wall=1700000000.5");

  Message ok;
  CHECK(peer.last("ok", &ok));
  CHECK_STR(ok.get("what"), "time");
  // The acknowledgement carries what the clock reads now rather than echoing
  // the request, so a client can see the value that was actually adopted.
  double reported = 0.0;
  CHECK(ok.get_double("wall", &reported));
  CHECK_NEAR(reported, 1700000000.5, 1e-2);
  CHECK_NEAR(rig.wall(), 1700000000.5, 1e-2);

  // And it still moves with the monotonic clock afterwards, which is the whole
  // point of holding an offset rather than a timestamp.
  rig.loop.advance(30.0);
  CHECK_NEAR(rig.wall(), 1700000030.5, 1e-2);
}

// A date is the one fact a radio genuinely cannot work out for itself.
//
// The mesh broadcasts a time of day, so a dongle knows that exactly -- an
// offset is a difference between two seconds-of-day figures and its own
// free-running clock cancels out of it. No amount of listening will ever
// supply the date, and a camera's real-time clock wants one.
void test_a_date_can_be_pushed_to_a_daemon_with_no_clock() {
  Rig rig;
  rig.build();
  // A dongle: monotonic from the first instant, and with no idea what
  // year it is. See firmware/src/boxclock.h.
  rig.wall0 = 0.0;
  CHECK(!rig.daemon->date_known());

  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->peer_line(&peer, "date y=2026 mo=8 d=31 id=7");

  Message ok;
  CHECK(peer.last("ok", &ok));
  CHECK_STR(ok.get("what"), "date");
  CHECK_STR(ok.get("id"), "7");
  // Echoed, so a client can see what was adopted rather than what it sent.
  CHECK_STR(ok.get("y"), "2026");
  CHECK_STR(ok.get("mo"), "8");
  CHECK_STR(ok.get("d"), "31");

  CHECK(rig.daemon->date_known());
  const octo::bmd::Civil today = rig.daemon->today();
  CHECK_EQ(today.year, 2026);
  CHECK_EQ(today.month, 8);
  CHECK_EQ(today.day, 31);
}

// A plausible wrong date is harder to notice than an obviously wrong one,
// and it ends up stamped on a camera's clock.
void test_an_impossible_date_is_refused() {
  Rig rig;
  rig.build();
  // A dongle: monotonic from the first instant, and with no idea what
  // year it is. See firmware/src/boxclock.h.
  rig.wall0 = 0.0;
  FakePeer peer;
  rig.daemon->peer_opened(&peer);

  const char* bad_lines[] = {
      "date y=1970 mo=1 d=1",    // before this protocol existed
      "date y=2026 mo=13 d=1",   // no thirteenth month
      "date y=2026 mo=0 d=1",    // nor a zeroth
      "date y=2026 mo=8 d=0",    // nor a zeroth day
      "date y=2026 mo=8 d=32",   // nor a thirty-second
  };
  for (const char* line : bad_lines) {
    rig.daemon->peer_line(&peer, line);
    Message err;
    CHECK(peer.last("err", &err));
    CHECK_STR(err.get("reason"), "bad-date");
  }
  // ...and none of them was adopted.
  CHECK(!rig.daemon->date_known());
}

void test_a_date_says_which_field_is_missing() {
  Rig rig;
  rig.build();
  // A dongle: monotonic from the first instant, and with no idea what
  // year it is. See firmware/src/boxclock.h.
  rig.wall0 = 0.0;
  FakePeer peer;
  rig.daemon->peer_opened(&peer);

  rig.daemon->peer_line(&peer, "date mo=8 d=31");
  Message err;
  CHECK(peer.last("err", &err));
  CHECK_STR(err.get("reason"), "missing-field");
  CHECK_STR(err.get("field"), "y");
  CHECK(!rig.daemon->date_known());
}

// A host with a real clock needs telling nothing: it knows the date because
// it knows the time. And its own clock outranks anything pushed at it, so a
// stale message from yesterday cannot overrule a clock that is right.
void test_a_real_clock_is_its_own_date_and_outranks_a_pushed_one() {
  Rig rig;
  rig.build();
  rig.wall0 = 1788190848.0;  // 2026-08-31, mid-morning UTC
  CHECK(rig.daemon->date_known());
  CHECK_EQ(rig.daemon->today().year, 2026);
  CHECK_EQ(rig.daemon->today().month, 8);
  CHECK_EQ(rig.daemon->today().day, 31);

  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->peer_line(&peer, "date y=2019 mo=1 d=2");
  // Accepted as a message -- it is well formed -- and ignored as an answer.
  CHECK_EQ(rig.daemon->today().year, 2026);
  CHECK_EQ(rig.daemon->today().day, 31);
}

// The status line says whether a write could be dated, separately from
// whether the clock is real. They are different facts: a dongle told today's
// date still free-runs, and can still set a camera.
void test_the_status_says_whether_a_write_could_be_dated() {
  Rig rig;
  rig.build();
  // A dongle: monotonic from the first instant, and with no idea what
  // year it is. See firmware/src/boxclock.h.
  rig.wall0 = 0.0;
  // ...and it has heard the mesh, which is what makes the clock demonstrably
  // free-running rather than merely unmeasured.
  rig.feed_boxes(3, 0.020);
  FakePeer peer;
  rig.daemon->peer_opened(&peer);

  rig.daemon->peer_line(&peer, "status");
  Message st;
  CHECK(peer.last("status", &st));
  CHECK_STR(st.get("clock"), "free");
  CHECK_STR(st.get("date"), "0");

  rig.daemon->peer_line(&peer, "date y=2026 mo=8 d=31");
  rig.daemon->peer_line(&peer, "status");
  CHECK(peer.last("status", &st));
  // Still free-running, and now able to date a write anyway. That combination
  // is the whole point.
  CHECK_STR(st.get("clock"), "free");
  CHECK_STR(st.get("date"), "1");
}

void test_time_without_a_value_is_an_error() {
  Rig rig;
  rig.build();
  rig.daemon->on_settime([](const SyncDaemon::WallTime&) {});
  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->peer_line(&peer, "time");

  Message err;
  CHECK(peer.last("err", &err));
  CHECK_STR(err.get("reason"), "missing-field");
  CHECK_STR(err.get("field"), "wall");
}

void test_status_says_what_the_bench_is() {
  Rig rig;
  rig.build();
  rig.feed_boxes(3, -6.25);
  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->peer_line(&peer, "status");

  Message status;
  CHECK(peer.last("status", &status));
  bool bench = false;
  CHECK(status.get_bool("bench", &bench));
  CHECK(bench);
  double offset = 0.0;
  CHECK(status.get_double("offset", &offset));
  CHECK_NEAR(offset, -6.25, 1e-3);
  int64_t boxes = 0;
  CHECK(status.get_int("boxes", &boxes));
  CHECK_EQ(boxes, 3LL);
  CHECK_STR(status.get("phase"), "stopped");
}

void test_devices_streams_and_says_when_it_is_done() {
  Rig rig;
  rig.build();
  rig.feed_boxes(2, -1.0);
  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->peer_line(&peer, "devices id=3");

  CHECK_EQ(peer.count("dev"), 2);
  Message end;
  CHECK(peer.last("end", &end));
  CHECK_STR(end.get("what"), "devices");
  int64_t n = 0;
  CHECK(end.get_int("n", &n));
  CHECK_EQ(n, 2LL);
  CHECK_STR(end.get("id"), "3");
}

void test_a_peer_can_ask_not_to_be_announced_to() {
  Rig rig;
  rig.build();
  FakePeer loud;
  FakePeer quiet;
  rig.daemon->peer_opened(&loud);
  rig.daemon->peer_opened(&quiet);
  rig.daemon->peer_line(&quiet, "announce on=0");

  Sighting seen;
  seen.id = "CAM-1";
  seen.name = "Pocket 6K";
  seen.rssi = -55;
  seen.mono = rig.loop.now();
  seen.wall = rig.wall();
  rig.daemon->observe_camera(seen);

  CHECK_EQ(loud.count("cam"), 1);
  CHECK_EQ(quiet.count("cam"), 0);
  // ...but a reply to something it asked is not an announcement.
  rig.daemon->peer_line(&quiet, "ping");
  CHECK_EQ(quiet.count("pong"), 1);
}

// Saying yes to a peer that is never going to hear anything is a peer waiting
// all night for lines that were not coming. The daemon has a switch that turns
// announcements off for everybody, and a per-peer `announce on=1` cannot
// overrule it -- so the reply says what the peer asked for and, when the two
// differ, what it is actually going to get.
void test_announce_says_so_when_announcements_are_off_entirely() {
  Rig rig;
  rig.opt.announce = false;
  rig.build();
  FakePeer peer;
  rig.daemon->peer_opened(&peer);

  rig.daemon->peer_line(&peer, "announce on=1");
  Message ok;
  CHECK(peer.last("ok", &ok));
  CHECK_STR(ok.get("what"), "announce");
  CHECK_STR(ok.get("on"), "1");
  CHECK_STR(ok.get("effective"), "0");

  // And it is telling the truth: nothing is announced.
  Sighting seen;
  seen.id = "CAM-1";
  seen.name = "Pocket 6K";
  seen.rssi = -55;
  seen.mono = rig.loop.now();
  seen.wall = rig.wall();
  rig.daemon->observe_camera(seen);
  CHECK_EQ(peer.count("cam"), 0);
}

// With announcements on, the reply carries no `effective` at all: there is
// nothing to warn about, and a field that is always present stops being read.
void test_announce_is_quiet_when_there_is_nothing_to_warn_about() {
  Rig rig;
  rig.build();
  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->peer_line(&peer, "announce on=1");
  Message ok;
  CHECK(peer.last("ok", &ok));
  CHECK_STR(ok.get("on"), "1");
  CHECK(!ok.has("effective"));
}

void test_a_camera_that_goes_off_the_air_is_announced_once() {
  Rig rig;
  rig.opt.announce_period = 5.0;  // a heartbeat, so silence gets timed
  // No camera backend, so nothing here holds a connection: a held connection
  // counts as presence, and would rightly stop the camera ever being reported
  // as gone.
  rig.with_camera = false;
  rig.build();
  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->start();
  rig.see_camera();
  CHECK_EQ(peer.count("cam"), 1);

  // Still advertising: nothing new to say, however many times it is heard.
  rig.loop.advance(6.0);
  rig.see_camera();
  rig.loop.advance(6.0);
  CHECK_EQ(peer.count("cam"), 1);

  // Now switched off. There is no event for that -- a camera that is off
  // sends nothing -- so it is a silence that has to be timed out.
  rig.loop.advance(rig.registry.policy().camera_gone_after + 10.0);
  CHECK_EQ(peer.count("cam"), 2);
  Message cam;
  CHECK(peer.last("cam", &cam));
  bool up = true;
  CHECK(cam.get_bool("up", &up));
  CHECK(!up);
}

void test_a_closed_peer_is_not_written_to() {
  Rig rig;
  rig.build();
  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->peer_closed(&peer);
  const size_t before = peer.lines.size();

  Sighting seen;
  seen.id = "CAM-1";
  seen.mono = rig.loop.now();
  seen.wall = rig.wall();
  rig.daemon->observe_camera(seen);
  CHECK_EQ(static_cast<int>(peer.lines.size()), static_cast<int>(before));
}

void test_the_cycle_outcome_is_announced() {
  Rig rig;
  rig.build();
  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->start();
  rig.loop.advance(1.0);

  Message cycle;
  CHECK(peer.last("cycle", &cycle));
  CHECK_STR(cycle.get("action"), "skip:no-tentacle");
  double next = 0.0;
  CHECK(cycle.get_double("next", &next));
  CHECK_NEAR(next, 60.0, 0.5);
}

void test_a_sync_request_runs_a_cycle_now() {
  Rig rig;
  rig.camera_error = 0.0;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.reach_camera(-6.0);
  rig.loop.advance(1.0);
  const size_t after_first = rig.cycles.size();

  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->peer_line(&peer, "sync");
  Message ok;
  CHECK(peer.last("ok", &ok));
  CHECK_STR(ok.get("what"), "sync");
  bool queued = true;
  CHECK(ok.get_bool("queued", &queued));
  CHECK(!queued);  // the daemon was idle, so this one runs rather than waits

  rig.loop.advance(0.5);
  rig.report_now(-6.0);
  rig.loop.advance(0.5);
  CHECK(rig.cycles.size() > after_first);
}

void test_a_request_that_arrives_mid_cycle_waits_its_turn() {
  Rig rig;
  rig.camera.delay = 0.5;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.see_camera();
  rig.loop.advance(0.2);   // mid-connect

  FakePeer peer;
  rig.daemon->peer_opened(&peer);
  rig.daemon->peer_line(&peer, "sync");
  Message ok;
  CHECK(peer.last("ok", &ok));
  bool queued = false;
  CHECK(ok.get_bool("queued", &queued));
  CHECK(queued);
}

void test_the_timecode_source_errand_needs_an_echo() {
  Rig rig;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->request_source(std::string(), bmd::kTimecodeSourceTimeOfDay);
  rig.daemon->start();
  rig.see_camera();
  rig.loop.advance(0.5);
  rig.camera.report_source(bmd::kTimecodeSourceClip);
  rig.loop.advance(0.5);

  CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 1);
  // The camera has not echoed the new value back, so the write is not
  // believed: a GATT ack only proves the bytes were taken.
  rig.loop.advance(2.0);
  CHECK_STR(rig.last_action(), "source:unverified");
}

void test_the_timecode_source_errand_believes_an_echo() {
  Rig rig;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->request_source(std::string(), bmd::kTimecodeSourceTimeOfDay);
  rig.daemon->start();
  rig.see_camera();
  rig.loop.advance(0.5);
  rig.camera.report_source(bmd::kTimecodeSourceClip);
  rig.loop.advance(0.2);
  CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 1);
  rig.camera.report_source(bmd::kTimecodeSourceTimeOfDay);
  rig.loop.advance(2.0);

  CHECK_STR(rig.last_action(), "source:ok");
}

void test_nothing_here_used_the_wall_clock() {
  // The whole file runs on a clock that is a variable. This is the assertion
  // that says so: a rig that reached a camera, wrote to it and verified the
  // write has moved its own clock by seconds, while the process has been
  // running for whatever tiny fraction of a second all of the above took.
  Rig rig;
  rig.camera_error = 4.0;
  rig.build();
  rig.feed_boxes(3, -6.0);
  rig.daemon->start();
  rig.reach_camera(-6.0);
  rig.loop.advance(3.0);
  CHECK(rig.loop.now() - Rig::kMono0 >= 3.0);
  CHECK_EQ(static_cast<int>(rig.camera.writes.size()), 1);
}

}  // namespace

int main() {
  test_bench_is_the_median_of_enabled_boxes();
  test_a_disabled_box_does_not_vote();
  test_no_boxes_means_the_camera_is_not_touched();
  test_a_cycle_writes_on_a_second_boundary();
  test_a_write_lands_on_a_boundary_at_any_phase();
  test_a_good_write_is_verified();
  test_a_write_that_changes_nothing_is_not_called_verified();
  test_recording_is_never_written_over();
  test_writes_disabled_stops_everything();
  test_a_clock_that_is_close_enough_is_left_alone();
  test_forcing_overrules_an_advisory_gate_only();
  test_a_camera_that_sends_no_timecode_ends_the_cycle();
  test_a_backend_that_never_answers_does_not_wedge_the_daemon();
  test_a_scan_finds_a_camera_when_nothing_is_known();
  test_a_camera_that_is_not_there_backs_off();
  test_a_camera_that_vanishes_mid_cycle_ends_the_cycle();
  test_a_completion_from_an_abandoned_cycle_is_ignored();
  test_destroying_the_daemon_with_work_in_flight_is_safe();
  test_holding_the_camera_avoids_a_second_connect();
  test_not_holding_releases_the_camera();
  test_a_peer_is_greeted();
  test_ping_and_unknown_verbs_are_both_answered();
  test_time_is_refused_by_a_daemon_that_has_a_clock();
  test_time_sets_the_clock_of_a_daemon_that_has_none();
  test_a_date_can_be_pushed_to_a_daemon_with_no_clock();
  test_an_impossible_date_is_refused();
  test_a_date_says_which_field_is_missing();
  test_a_real_clock_is_its_own_date_and_outranks_a_pushed_one();
  test_the_status_says_whether_a_write_could_be_dated();
  test_time_without_a_value_is_an_error();
  test_time_carries_the_zone_as_well_as_the_instant();
  test_time_without_a_zone_leaves_the_zone_unsaid();
  test_an_impossible_zone_is_refused();
  test_status_says_what_the_bench_is();
  test_devices_streams_and_says_when_it_is_done();
  test_a_peer_can_ask_not_to_be_announced_to();
  test_announce_says_so_when_announcements_are_off_entirely();
  test_announce_is_quiet_when_there_is_nothing_to_warn_about();
  test_a_camera_that_goes_off_the_air_is_announced_once();
  test_a_closed_peer_is_not_written_to();
  test_the_cycle_outcome_is_announced();
  test_a_sync_request_runs_a_cycle_now();
  test_a_request_that_arrives_mid_cycle_waits_its_turn();
  test_the_timecode_source_errand_needs_an_echo();
  test_the_timecode_source_errand_believes_an_echo();
  test_nothing_here_used_the_wall_clock();
  return octotest::report("test_syncd");
}
