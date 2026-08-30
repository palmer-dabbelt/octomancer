// A whole bench, from the air to the page.
//
// The unit tests in this suite are good at the pieces and have never been good
// at the join. Every regression that has reached a person recently lived
// there: a camera that vanished from the list because both sources of camera
// rows are records of something being *heard*, an event cursor off by one
// between a daemon and its client, a table drawing itself on top of itself.
// Each piece was behaving; the composition was not, and nothing exercised the
// composition.
//
// So this drives the real chain, with only the radio faked:
//
//   src/fakebench.h  what would be on the air, as bytes
//        v
//   src/registry.h   the real roster, decoding the real Tentacle payloads
//        v
//   src/devices.h    the real merge of two daemons' views and a config file
//        v
//   src/render.h     the real table a person reads
//
// A test says what is in the room and asserts what somebody would see. Nothing
// in between is stubbed, so a change anywhere along that chain has to keep the
// page honest or say why.
//
// What it deliberately does not cover: ui/main.mm. AppKit is not reachable
// from here, and pretending otherwise would be worse than admitting it -- the
// blur that prompted this file was a view-hierarchy bug that no amount of this
// would have caught. What this does cover is everything the UI *reads*, which
// is where the rest of them were.
#ifndef OCTO_TEST_SCENARIO_H
#define OCTO_TEST_SCENARIO_H

#include <unistd.h>

#include <cmath>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "camconf.h"
#include "control.h"
#include "devices.h"
#include "fakebench.h"
#include "harness.h"
#include "registry.h"
#include "render.h"
#include "timeutil.h"

namespace octotest {

class Scenario {
 public:
  // An empty spec is FakeBench::standard(), which is the bench `--radio fake`
  // gives: five boxes that disagree slightly, one of them quiet for a while,
  // and a camera a quarter-second out. See src/fakebench.h for the grammar.
  explicit Scenario(const std::string& spec = std::string()) {
    std::string err;
    if (spec.empty()) {
      bench_ = octo::FakeBench::standard();
    } else if (!octo::FakeBench::parse(spec, &bench_, &err)) {
      octotest::fail(__FILE__, __LINE__, "bad bench spec: " + err);
    }
    // Truncated to a whole second, for the reason tests/test_syncd.cc gives at
    // length: a fraction of a second here decides where in each cycle a write
    // falls, and the suite was quietly sampling one random phase per run.
    wall0_ = std::floor(octo::wall_now());
    registry_.reset(new octo::Registry(policy_, kMono0));
  }

  ~Scenario() {
    if (!conf_path_.empty()) ::unlink(conf_path_.c_str());
  }

  Scenario(const Scenario&) = delete;
  Scenario& operator=(const Scenario&) = delete;

  // The contents of cameras.conf. Written to a file and read back by the real
  // CamConf, because CamConf is the thing that decides what "enabled" means
  // and a test that reimplements that decision is testing itself.
  Scenario& conf(const std::string& body) {
    conf_path_ = "/tmp/octo-scenario-" + std::to_string(::getpid()) + "-" +
                 std::to_string(++conf_seq_) + ".conf";
    {
      std::ofstream out(conf_path_, std::ios::trunc);
      out << body;
    }
    std::string err;
    CHECK(conf_.load(conf_path_, &err));
    has_conf_ = true;
    return *this;
  }

  // Which daemons are answering. Both are by default; a view with one missing
  // is a real state and the one somebody is looking at when they are trying to
  // work out which half of the program is broken.
  Scenario& without_sync() {
    sync_up_ = false;
    return *this;
  }
  Scenario& without_octomancerd() {
    bench_up_ = false;
    return *this;
  }

  // octomancer-sync holding the camera's link. A camera stops advertising
  // while something is connected to it, so this is the state where silence is
  // our own doing and must not read as absence.
  Scenario& camera_held(bool held = true) {
    camera_held_ = held;
    return *this;
  }

  // Pour `seconds` of radio into the roster. Call it more than once to build a
  // longer run; time does not restart.
  void run(double seconds) {
    const double until = mono_ + seconds;
    // In steps, so that a box which goes quiet and comes back is actually
    // quiet for a while rather than having its whole history delivered at
    // once. A tenth of a second is finer than any advertisement interval.
    const double step = 0.1;
    while (mono_ < until) {
      const double next = std::min(mono_ + step, until);
      for (const octo::Advert& a :
           octo::adverts_between(bench_, mono_, next, kMono0, wall0_)) {
        registry_->observe(a.id, a.name, a.rssi, a.data.data(), a.data.size(),
                           a.mono, a.wall);
      }
      for (const octo::Sighting& s :
           octo::sightings_between(bench_, mono_, next, kMono0, wall0_)) {
        registry_->observe_camera(s.id, s.name, s.rssi, s.mono, s.wall);
        last_camera_ = s;
        saw_camera_ = true;
      }
      mono_ = next;
    }
    registry_->set_radio("poweredOn");
    built_ = false;
  }

  // What a person would see. Both are built from the same view, so a test can
  // assert on the structure and on the text without them being able to
  // disagree.
  const octo::DeviceView& view() {
    build();
    return view_;
  }
  std::string table(bool verbose = false) {
    build();
    return octo::render_devices(view_, verbose, /*colour=*/false);
  }

  const octo::DeviceRow* row(const std::string& name) {
    build();
    for (const octo::DeviceRow& r : view_.rows) {
      if (r.name == name) return &r;
    }
    return nullptr;
  }

  // The identifier the fake radio will use for a named box. Ids are derived
  // from the name by a hash, so a test that wants to write a line about a box
  // in cameras.conf cannot spell one out; this is how it asks.
  std::string box_id(const std::string& name) const {
    for (const octo::FakeBox& b : bench_.boxes) {
      if (b.name == name) return b.id;
    }
    octotest::fail(__FILE__, __LINE__, "no box named " + name);
    return std::string();
  }

  double now_wall() const { return wall0_ + (mono_ - kMono0); }
  octo::Registry& registry() { return *registry_; }
  const octo::FakeBench& bench() const { return bench_; }

 private:
  static constexpr double kMono0 = 1000.0;

  void build() {
    if (built_) return;
    built_ = true;

    snapshot_ = registry_->snapshot(mono_, now_wall());
    status_ = octo::Status();

    // octomancer-sync's view of the camera, assembled from what the fake
    // radio actually delivered rather than from the spec. A test that asserted
    // against the spec would pass even if nothing ever reached the roster.
    if (bench_.has_camera) {
      octo::CameraStatus c;
      c.id = bench_.camera.id;
      c.name = bench_.camera.name;
      c.writes_enabled =
          !has_conf_ || conf_.writes_enabled(bench_.camera.id);
      c.connected = camera_held_;
      // Held counts as present: the camera stopped advertising because we are
      // talking to it, and reporting that as absence would be reporting our
      // own behaviour as the camera's.
      c.present = camera_held_ ||
                  (saw_camera_ && now_wall() - last_camera_.wall < 90.0);
      if (saw_camera_) {
        c.has_last_seen = true;
        c.last_seen_wall = last_camera_.wall;
        c.has_rssi = true;
        c.rssi = last_camera_.rssi;
      }
      // How wrong the camera is, as octomancer-sync would report it. Only
      // when it could actually have been measured: the figure comes from
      // connecting and reading a timecode back, so a camera that has never
      // been on the air has no error and the page has to say so rather than
      // quote a zero.
      //
      // The spec's error is used as-is. A camera's drift over a scenario is
      // microseconds and this is standing in for a measurement, not for the
      // camera's true state.
      if (c.present || c.connected) {
        c.has_error = true;
        c.error_s = bench_.camera.error_s;
      }
      status_.cameras.push_back(c);
    }

    octo::DeviceSources from;
    from.bench = bench_up_ ? &snapshot_ : nullptr;
    from.cameras = sync_up_ ? &status_ : nullptr;
    from.conf = has_conf_ ? &conf_ : nullptr;
    from.now_wall = now_wall();
    view_ = octo::build_device_view(from);
  }

  octo::Policy policy_;
  octo::FakeBench bench_;
  std::unique_ptr<octo::Registry> registry_;
  octo::CamConf conf_;
  std::string conf_path_;
  int conf_seq_ = 0;
  bool has_conf_ = false;

  double wall0_ = 0.0;
  double mono_ = kMono0;

  bool sync_up_ = true;
  bool bench_up_ = true;
  bool camera_held_ = false;

  octo::Sighting last_camera_;
  bool saw_camera_ = false;

  bool built_ = false;
  octo::Snapshot snapshot_;
  octo::Status status_;
  octo::DeviceView view_;
};

}  // namespace octotest

#endif  // OCTO_TEST_SCENARIO_H
