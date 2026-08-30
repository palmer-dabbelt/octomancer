// Daemons that are not there.
//
// The radio has a fake (src/fakebench.h); this is the other set of seams. Four
// programs talk to each other over three sockets, and until now every one of
// those conversations could only be exercised by starting real daemons on a
// real machine -- so the client halves have been tested against parsers and
// never against a server, and the server halves never against a client.
//
// The important decision here is what a mock is made of. Every one of these is
// built out of the *real* server-side implementation with fake data poured in:
// octomancerd's is a real Registry behind the real registry_handler, and the
// sync daemon's is a real octo::Control behind the real octo::Server. Nothing
// below reimplements a protocol.
//
// That is not tidiness. A hand-written mock of a wire format is a second
// implementation that agrees with the first exactly until somebody changes one
// of them, and the failure it produces then is a test that passes while the
// program is broken -- which is worse than having no test, because it is
// believed. The only mock worth having here is one that cannot drift, and the
// way to get one is to fake the data rather than the protocol.
#ifndef OCTO_TEST_FAKEDAEMON_H
#define OCTO_TEST_FAKEDAEMON_H

#include <unistd.h>

#include <atomic>
#include <string>
#include <memory>
#include <thread>
#include <vector>

#include "../src/bmd.h"
#include "../src/control.h"
#include "../src/fakebench.h"
#include "../src/registry.h"
#include "../src/server.h"
#include "../src/tentacle.h"
#include "../src/timeutil.h"

namespace octotest {

// A socket path short enough to be one. sun_path is 104 bytes and a build
// directory can easily be half of that, so these go under /tmp with the pid in
// them rather than beside the test binary.
inline std::string socket_path(const char* tag) {
  return "/tmp/octo-t-" + std::to_string(getpid()) + "-" + tag + ".sock";
}

// Runs an octo::Server on a thread of its own, because that is how a client
// sees one: a thing that is already listening when you connect. A test that
// served the socket from its own thread between requests would pass while a
// client that connected at the wrong moment failed.
class FakeSocketDaemon {
 public:
  ~FakeSocketDaemon() { stop(); }

  bool start(octo::Handler handler, const std::string& path, std::string* err) {
    ::unlink(path.c_str());
    server_.reset(new octo::Server(std::move(handler), path));
    if (!server_->start(err)) {
      server_.reset();
      return false;
    }
    running_ = true;
    thread_ = std::thread([this] {
      while (running_) server_->serve(20);
    });
    return true;
  }

  void stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (server_) server_->shutdown();
    server_.reset();
  }

 private:
  std::unique_ptr<octo::Server> server_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

// --------------------------------------------------------------- octomancerd
//
// A Registry with a bench in it, served by the handler octomancerd serves.
// What makes this a mock of octomancerd rather than a copy of it is only that
// the adverts came from FakeBench instead of from a radio -- which is exactly
// the difference `--radio fake` makes to the real one.
class FakeBenchDaemon {
 public:
  explicit FakeBenchDaemon(const octo::FakeBench& bench = octo::FakeBench::standard())
      : bench_(bench) {}

  // Pour `seconds` of bench history in, as though it had been listening that
  // long. Real time is not spent: the adverts carry their own timestamps, and
  // a test that slept for a minute to build a minute of history would be a
  // test nobody runs.
  //
  // The stamps end at now, so ages come out as a client expects rather than as
  // a bench heard from an hour ago.
  void listen(double seconds) {
    const double mono0 = octo::mono_now() - seconds;
    const double wall0 = octo::wall_now() - seconds;
    for (const octo::Advert& a :
         octo::adverts_between(bench_, -1.0, seconds, mono0, wall0)) {
      registry_.observe(a.id, a.name, a.rssi, a.data.data(), a.data.size(),
                        a.mono, a.wall);
    }
    for (const octo::Sighting& s :
         octo::sightings_between(bench_, -1.0, seconds, mono0, wall0)) {
      registry_.observe_camera(s.id, s.name, s.rssi, s.mono, s.wall);
    }
    // Said explicitly, because a client is entitled to ask why a bench is
    // empty and "the radio never reported" is a different answer from "nothing
    // was on the air" -- see the empty-list path in src/devices.cc.
    registry_.set_radio("poweredOn");
  }

  octo::Registry& registry() { return registry_; }

  bool start(std::string* err) {
    path_ = socket_path("bench");
    return daemon_.start(octo::registry_handler(registry_), path_, err);
  }
  void stop() { daemon_.stop(); }
  const std::string& path() const { return path_; }

 private:
  octo::FakeBench bench_;
  octo::Registry registry_;
  FakeSocketDaemon daemon_;
  std::string path_;
};

// ------------------------------------------------------------ octomancer-sync
//
// A real octo::Control -- the whole vocabulary, the request queue, the event
// log -- with a fake camera published into it, served on a socket. This is what
// a UI or the CLI talks to, and what a control daemon will eventually talk to.
class FakeControlDaemon {
 public:
  explicit FakeControlDaemon(const octo::FakeBench& bench = octo::FakeBench::standard()) {
    octo::DaemonStatus d;
    d.version = "0.1.0";
    d.started_wall = octo::wall_now() - 300.0;
    d.now_wall = octo::wall_now();
    control_.set_daemon(d);

    if (bench.has_camera) {
      octo::CameraStatus c;
      c.id = bench.camera.id;
      c.name = bench.camera.name;
      c.present = true;
      c.has_rssi = true;
      c.rssi = bench.camera.rssi;
      c.has_fps = true;
      c.fps = bench.camera.fps;
      c.has_error = true;
      c.error_s = bench.camera.error_s;
      c.recording = bench.camera.recording;
      c.writes_enabled = true;
      c.has_source = true;
      c.source = bench.camera.timecode_follows_clock
                     ? octo::bmd::kTimecodeSourceTimeOfDay
                     : octo::bmd::kTimecodeSourceClip;
      c.has_last_seen = true;
      c.last_seen_wall = octo::wall_now();
      control_.publish_camera(c);
    }
  }

  octo::Control& control() { return control_; }

  bool start(std::string* err) {
    path_ = socket_path("control");
    return daemon_.start(
        [this](const std::string& line) { return control_.handle(line); },
        path_, err);
  }
  void stop() { daemon_.stop(); }
  const std::string& path() const { return path_; }

 private:
  octo::Control control_;
  FakeSocketDaemon daemon_;
  std::string path_;
};

}  // namespace octotest

#endif  // OCTO_TEST_FAKEDAEMON_H
