// A Blackmagic camera that is not there.
//
// The other half of --radio fake. The scanner half only has to say what is on
// the air; this one has to hold a clock, drift, answer questions about itself,
// and -- the part that matters -- accept a write and afterwards report the
// time it was told rather than the time it had.
//
// That last property is the whole reason this exists. Everything octomancer
// does converges on one moment: a packet leaves at a computed instant so that
// a whole second lands on a boundary, and a few hundred milliseconds later the
// camera is asked whether it worked. Until now that moment could only be
// reached with a camera switched on in front of somebody, which is why
// doc/KNOWN_ISSUES.md has been waiting on hardware verification. A fake camera
// cannot prove the *timing* is right -- only a real one can, and this file
// does not pretend otherwise -- but it can prove that the program does the
// arithmetic it thinks it does, and that everything around the write behaves.
//
// It is deliberately obedient. There is no failed write, no dropped
// connection, no camera that reports a rate it is not running at. Those are
// worth having and are not here yet; when they arrive they belong in
// FakeCamera as flags, so that a spec can ask for them by name.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "bmd.h"
#include "camera.h"
#include "fakebench.h"
#include "radio.h"
#include "timeutil.h"

namespace octo {
namespace {

class FakeCameraLink : public CameraLink {
 public:
  explicit FakeCameraLink(FakeBench bench)
      : bench_(std::move(bench)),
        // The camera starts wrong by whatever the spec asked for, and its
        // drift is measured from now. These two travel together: `error_s_`
        // is always the error as of `error_at_`, and a write is simply a new
        // pair. Starting `error_at_` at zero instead would make the first
        // reading carry the machine's entire uptime worth of drift.
        error_s_(bench_.camera.error_s),
        error_at_(mono_now()) {}

  bool ready(double timeout, std::string* err) override {
    (void)timeout;
    (void)err;
    return true;
  }

  ScanResult scan(double seconds, const std::string& name_hint, bool want_all,
                  const CameraSeen& on_camera) override {
    (void)seconds;  // a fake scan is instant; waiting would only be theatre
    (void)want_all;
    ScanResult out;
    if (!bench_.has_camera) return out;
    if (!name_hint.empty() &&
        bench_.camera.name.find(name_hint) == std::string::npos &&
        bench_.camera.id.find(name_hint) == std::string::npos) {
      return out;
    }
    CameraDevice d;
    d.id = bench_.camera.id;
    d.name = bench_.camera.name;
    d.rssi = bench_.camera.rssi;
    // Proof, not a name guess: a fake camera that only matched by name would
    // let a caller pass here that would fail against a real one.
    d.by_service_uuid = true;
    out.cameras.push_back(d);
    out.all.push_back(d);
    out.total = 1 + static_cast<int>(bench_.boxes.size());
    out.tentacles = static_cast<int>(bench_.boxes.size());
    if (on_camera) on_camera(d);
    return out;
  }

  bool connect(const std::string& id, double timeout,
               std::string* err) override {
    (void)timeout;
    if (!bench_.has_camera || (!id.empty() && id != bench_.camera.id)) {
      if (err) *err = "no such camera: " + id;
      return false;
    }
    connected_ = true;
    subscribed_ = false;
    return true;
  }

  void disconnect() override {
    connected_ = false;
    subscribed_ = false;
  }

  bool connected() const override { return connected_; }

  bool subscribe(double timeout, std::string* err) override {
    (void)timeout;
    if (!connected_) {
      if (err) *err = "not connected";
      return false;
    }
    // Refused rather than ignored, because the real link refuses and a caller
    // that subscribes twice is a caller with a bug this fake exists to catch.
    if (subscribed_) {
      if (err) *err = "already subscribed";
      return false;
    }
    subscribed_ = true;
    return true;
  }

  bool subscribed() const override { return subscribed_; }

  bool read_status(std::vector<uint8_t>* out, double timeout,
                   std::string* err) override {
    (void)timeout;
    if (!connected_) {
      if (err) *err = "not connected";
      return false;
    }
    // Power on, connected, paired.
    if (out) *out = std::vector<uint8_t>{0x07};
    return true;
  }

  bool write_control(const std::vector<uint8_t>& packet, double timeout,
                     std::string* err) override {
    (void)timeout;
    if (!connected_) {
      if (err) *err = "not connected";
      return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (const bmd::Message& msg : bmd::parse_stream(packet)) {
      bmd::Value v;
      if (!bmd::decode_value(msg, &v)) continue;

      // Group 7 parameter 0: the RTC. This is the write the whole program
      // exists to perform, and the fake's response to it is the one behaviour
      // worth being exact about.
      if (v.group == 7 && v.param == 0 && v.ints.size() >= 2) {
        int h = 0, m = 0, s = 0, f = 0;
        if (!bmd::decode_bcd_timecode(static_cast<uint32_t>(v.ints[0]), &h, &m,
                                      &s, &f)) {
          if (err) *err = "the RTC field was not BCD";
          return false;
        }
        // The camera takes the value at the instant it arrives, so its error
        // afterwards is the difference between what it was told and what the
        // time actually was -- which is precisely what the verification pass
        // upstairs is about to measure. Note this is UTC: writing local time
        // here would hide the double-offset bug that bmd.h warns about, which
        // is the opposite of what a fake is for.
        // ...at the instant it *lands*, which is not the instant it was
        // sent. The daemon writes `lead` early expecting the packet to spend
        // that long in transit, so a fake that applied it immediately would
        // leave the camera a whole lead fast on every write and give the
        // lead-learning loop nothing to converge on. See
        // FakeCamera::write_latency_s.
        const double now = wall_now() + bench_.camera.write_latency_s;
        const bmd::Civil utc = bmd::utc_civil(now);
        const double told = h * 3600.0 + m * 60.0 + s;
        const double actual =
            utc.hour * 3600.0 + utc.minute * 60.0 + utc.second +
            (now - std::floor(now));
        double error = told - actual;
        if (error > 43200.0) error -= 86400.0;
        if (error < -43200.0) error += 86400.0;
        error_s_ = error;
        error_at_ = mono_now() + bench_.camera.write_latency_s;
        ++writes_;
        continue;
      }

      // Group 9 parameter 7: which clock the timecode follows.
      if (v.group == 9 && v.param == 7 && !v.ints.empty()) {
        source_ = v.ints[0];
      }
    }
    return true;
  }

  CameraView view() override {
    std::lock_guard<std::mutex> lock(mu_);
    CameraView out;
    if (!connected_) return out;
    const FakeCamera& c = bench_.camera;

    out.has_fps = true;
    out.fps = c.fps;
    out.has_transport = true;
    out.transport = c.recording ? 1 : 0;
    out.has_timecode_source = true;
    out.timecode_source = source_;

    if (forgot_) return out;  // no reading since the last write

    const double now = mono_now();
    // The camera's own clock: true time plus whatever error it currently
    // carries, plus the drift accumulated since. A write resets the error;
    // drift starts again from there.
    const double drift = c.drift_ppm * 1e-6 * (now - error_at_);
    const double wall = wall_now() + error_s_ + drift;
    const time_t whole = static_cast<time_t>(std::floor(wall));
    struct tm local;
    localtime_r(&whole, &local);

    out.has_timecode = true;
    out.timecode.hours = local.tm_hour;
    out.timecode.minutes = local.tm_min;
    out.timecode.seconds = local.tm_sec;
    // The camera reports whole frames and nothing finer, which is a real
    // limit the program above has to cope with -- it is why a write is aimed
    // at a second boundary rather than at an arbitrary instant.
    out.timecode.frames =
        static_cast<int>((wall - std::floor(wall)) * c.fps);
    out.timecode_mono = now;
    return out;
  }

  void forget_timecode() override {
    std::lock_guard<std::mutex> lock(mu_);
    forgot_ = true;
    forgot_at_ = mono_now();
  }

  CameraView await_state(double seconds) override {
    // A real camera notifies on its own schedule and the caller waits. Here
    // the reading is always available, except immediately after
    // forget_timecode() -- where pretending otherwise would let a caller pass
    // that never actually waits for a fresh sample.
    const double until = mono_now() + std::max(0.0, seconds);
    while (mono_now() < until) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        // One notification interval, so a forgotten reading comes back the way
        // a real one does: a moment later, not instantly.
        if (forgot_ && mono_now() - forgot_at_ >= kNotifyInterval) {
          forgot_ = false;
        }
        if (!forgot_) break;
      }
      struct timespec ts = {0, 10 * 1000 * 1000};
      nanosleep(&ts, nullptr);
    }
    return view();
  }

  int writes() const { return writes_; }

 private:
  // How long a real camera takes to send the next timecode notification. A
  // 24 fps camera notifies every frame; this is deliberately slower, so that
  // a caller which does not wait properly fails here rather than on hardware.
  static constexpr double kNotifyInterval = 0.05;

  const FakeBench bench_;

  mutable std::mutex mu_;
  bool connected_ = false;
  bool subscribed_ = false;
  bool forgot_ = false;
  double forgot_at_ = 0.0;
  int writes_ = 0;
  int64_t source_ = bmd::kTimecodeSourceTimeOfDay;
  // Declared last but initialised in the constructor's list, so the order
  // above is the order they are built in.
  double error_s_ = 0.0;
  double error_at_ = 0.0;
};

}  // namespace

std::unique_ptr<CameraLink> make_fake_camera_link() {
  FakeBench bench;
  std::string err;
  if (!FakeBench::parse(radio_options().fake, &bench, &err)) {
    std::fprintf(stderr, "octomancer: %s\n", err.c_str());
    return nullptr;
  }
  if (!bench.has_camera) {
    std::fprintf(stderr,
                 "octomancer: this fake bench has no camera in it; add one"
                 " with cam,<id>,<name>,<error_s>\n");
    return nullptr;
  }
  return std::unique_ptr<CameraLink>(new FakeCameraLink(bench));
}

}  // namespace octo
