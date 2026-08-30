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

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

#include <atomic>
#include <string>
#include <memory>
#include <thread>
#include <vector>

#include "../src/bmd.h"
#include "../src/control.h"
#include "../src/fakebench.h"
#include "../src/boxsock.h"
#include "../src/registry.h"
#include "../src/server.h"
#include "../src/syncd.h"
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

// A client that holds a connection open and reads whole lines off it.
//
// octo::query in src/client.h cannot be used here: it writes a command, reads
// until the peer closes, and returns. That is exactly right for the two
// block-reply sockets and exactly wrong for this one, where the connection
// stays open and the interesting messages are the ones that arrive without
// being asked for. Nothing in the tree does this yet, which is the point --
// this is roughly the read path a control daemon will need.
class LineClient {
 public:
  ~LineClient() { close(); }

  bool connect(const std::string& path, std::string* err) {
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
      if (err) *err = "socket: " + std::string(strerror(errno));
      return false;
    }
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
      if (err) *err = "socket path is too long";
      close();
      return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    // A server that is listening may still be a moment from accepting, and a
    // test that raced it would fail a few times in a hundred runs and look
    // like a real bug.
    for (int attempt = 0; attempt < 100; ++attempt) {
      if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                    sizeof addr) == 0) {
        return true;
      }
      struct timespec ts = {0, 10 * 1000 * 1000};
      nanosleep(&ts, nullptr);
    }
    if (err) *err = "connect: " + std::string(strerror(errno));
    close();
    return false;
  }

  void send(const std::string& line) {
    const std::string out = line + "\n";
    ssize_t ignored = ::write(fd_, out.data(), out.size());
    (void)ignored;
  }

  // One whole line, or false if none arrived before the deadline.
  bool next(std::string* out, double timeout) {
    const double until = octo::mono_now() + timeout;
    for (;;) {
      const size_t nl = buf_.find('\n');
      if (nl != std::string::npos) {
        *out = buf_.substr(0, nl);
        buf_.erase(0, nl + 1);
        return true;
      }
      const double left = until - octo::mono_now();
      if (left <= 0.0) return false;
      struct pollfd p;
      p.fd = fd_;
      p.events = POLLIN;
      const int ready = ::poll(&p, 1, static_cast<int>(left * 1000.0) + 1);
      if (ready <= 0) continue;
      char chunk[4096];
      const ssize_t n = ::read(fd_, chunk, sizeof chunk);
      if (n <= 0) return false;
      buf_.append(chunk, static_cast<size_t>(n));
    }
  }

  void close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

 private:
  int fd_ = -1;
  std::string buf_;
};

// -------------------------------------------------- the sync daemon, on a socket
//
// The third seam, and the one with no client yet: `octomancer-syncd.sock`
// speaks src/boxmsg.h and nothing outside tests has ever connected to it
// (doc/KNOWN_ISSUES.md entry 4). So this exists slightly ahead of its use --
// it is what whoever writes the control daemon connects to on their first
// afternoon, instead of having to start a real daemon on a machine with a
// radio before they can see a single message.
//
// It is a real SyncDaemon behind a real LineServer, fed by a real Registry,
// on a real Loop. The only fake part is where the adverts come from. That
// matters more here than anywhere else in this file: the box protocol is the
// one an eventual Nordic firmware speaks, and a mock of it that drifted would
// be discovered on hardware, months later, by somebody with a soldering iron.
class FakeBoxDaemon {
 public:
  explicit FakeBoxDaemon(const octo::FakeBench& bench = octo::FakeBench::standard())
      : bench_(bench) {}

  ~FakeBoxDaemon() { stop(); }

  bool start(std::string* err) {
    path_ = socket_path("box");
    ::unlink(path_.c_str());

    octo::SyncdOptions opt;
    opt.announce = true;
    // Fast enough that a test does not sit waiting for the bench line, and
    // still a real timer on a real loop rather than a poke from the test.
    opt.announce_period = 0.2;
    daemon_.reset(new octo::SyncDaemon(loop_.get(), &registry_, opt));

    server_.reset(new octo::LineServer(loop_.get(), path_));
    octo::SyncDaemon* d = daemon_.get();
    server_->on_open([d](octo::MsgPeer* peer) { d->peer_opened(peer); });
    server_->on_line([d](octo::MsgPeer* peer, const std::string& line) {
      d->peer_line(peer, line);
    });
    server_->on_close([d](octo::MsgPeer* peer) { d->peer_closed(peer); });
    if (!server_->start(err)) return false;

    // The radio, as a timer on the loop. Pouring adverts in from the test's
    // own thread would be a data race that happens to pass -- the daemon and
    // the registry both belong to the loop.
    // Started as though it had been listening for a while, rather than from
    // nothing. A client that connects and immediately asks `devices` should
    // get the bench, and a fixture that took ten seconds of real time to
    // become useful would be a fixture nobody puts in a test.
    //
    // Safe to pour before run(): there is no other thread yet, and everything
    // after this point goes through the loop.
    const double kPrime = 10.0;
    mono0_ = octo::mono_now() - kPrime;
    wall0_ = octo::wall_now() - kPrime;
    pour();
    loop_->every(0.1, [this] { pour(); });
    // Said explicitly, as a real backend does, so a client can tell "the radio
    // never came up" from "nothing was on the air".
    daemon_->set_radio_state("poweredOn");
    daemon_->start();

    running_ = true;
    thread_ = std::thread([this] { loop_->run(); });
    return true;
  }

  void stop() {
    if (!running_.exchange(false)) return;
    loop_->stop();
    if (thread_.joinable()) thread_.join();
    server_.reset();
    daemon_.reset();
    if (!path_.empty()) ::unlink(path_.c_str());
  }

  const std::string& path() const { return path_; }

 private:
  void pour() {
    const double now = octo::mono_now() - mono0_;
    for (const octo::Advert& a :
         octo::adverts_between(bench_, since_, now, mono0_, wall0_)) {
      daemon_->observe_advert(a);
    }
    since_ = now;
  }

  octo::FakeBench bench_;
  // A loop of its own rather than default_loop(): two of these in one test
  // binary must not share a run() and a stop().
  std::unique_ptr<octo::Loop> loop_ = octo::make_loop();
  octo::Registry registry_;
  std::unique_ptr<octo::SyncDaemon> daemon_;
  std::unique_ptr<octo::LineServer> server_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::string path_;
  double mono0_ = 0.0;
  double wall0_ = 0.0;
  double since_ = -1.0;
};

}  // namespace octotest

#endif  // OCTO_TEST_FAKEDAEMON_H
