// The box protocol over a real unix socket, on the real loop.
//
// Unlike tests/test_syncd.cc this one does not fake anything: it opens a
// socket, connects to it, and runs poll(2). That is deliberate. The daemon
// above it is tested against a fake because its interesting behaviour is
// arithmetic; this file is nothing but system calls, and a fake socket would
// test the fake.
//
// What it is looking for is the half of a byte stream that is easy to get
// wrong and easy to not notice: a message arriving in two reads, two messages
// arriving in one, a peer that hangs up mid-sentence, and a peer that stops
// reading and would otherwise make the daemon hold every announcement it ever
// produced.
#include "../src/boxsock.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include "../src/loop.h"
#include "harness.h"

using namespace octo;

namespace {

std::string temp_path(const char* tag) {
  return "/tmp/octo-boxsock-" + std::to_string(getpid()) + "-" + tag + ".sock";
}

// The other end of the socket, in the same process. Non-blocking, because the
// server it is talking to is being run by this same thread between calls.
class Peer {
 public:
  bool open(const std::string& path) {
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.c_str(), path.size());
    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof addr) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    return true;
  }

  // Writes all of it, however many turns that takes. A short write here would
  // silently weaken every test that sends more than a socket buffer.
  void write(const std::string& bytes) {
    size_t sent = 0;
    while (fd_ >= 0 && sent < bytes.size()) {
      const ssize_t n = ::write(fd_, bytes.data() + sent, bytes.size() - sent);
      if (n > 0) {
        sent += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        read_lines();  // make room by draining whatever came back
        continue;
      }
      break;
    }
  }

  // Whatever has arrived, split into whole lines.
  std::vector<std::string> read_lines() {
    char buf[4096];
    for (;;) {
      const ssize_t n = ::read(fd_, buf, sizeof buf);
      if (n > 0) {
        pending_.append(buf, static_cast<size_t>(n));
        continue;
      }
      break;
    }
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i < pending_.size(); ++i) {
      if (pending_[i] != '\n') continue;
      out.push_back(pending_.substr(start, i - start));
      start = i + 1;
    }
    pending_.erase(0, start);
    return out;
  }

  void close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }
  // Stop reading, but stay connected. What a suspended UI looks like.
  int fd() const { return fd_; }

  ~Peer() { close(); }

 private:
  int fd_ = -1;
  std::string pending_;
};

struct Rig {
  explicit Rig(const char* tag)
      : path(temp_path(tag)), loop(make_loop()), server(loop.get(), path) {
    ::unlink(path.c_str());
    server.on_open([this](MsgPeer* peer) {
      opened.push_back(peer);
      peer->send("hello proto=1");
    });
    server.on_line([this](MsgPeer* peer, const std::string& line) {
      lines.push_back(line);
      if (line == "ping") peer->send("pong");
    });
    server.on_close([this](MsgPeer* peer) { closed.push_back(peer); });
  }

  ~Rig() { ::unlink(path.c_str()); }

  // Run the loop for a moment. Everything here is local, so a few turns with
  // a short deadline is enough for anything that is going to happen.
  void pump(int turns = 8) {
    for (int i = 0; i < turns; ++i) loop->tick(0.01);
  }

  std::string path;
  std::unique_ptr<Loop> loop;
  LineServer server;
  std::vector<MsgPeer*> opened;
  std::vector<MsgPeer*> closed;
  std::vector<std::string> lines;
};

void test_a_client_connects_and_is_greeted() {
  Rig rig("greet");
  std::string err;
  CHECK(rig.server.start(&err));

  Peer peer;
  CHECK(peer.open(rig.path));
  rig.pump();

  CHECK_EQ(static_cast<int>(rig.opened.size()), 1);
  CHECK_EQ(static_cast<int>(rig.server.clients()), 1);
  const std::vector<std::string> got = peer.read_lines();
  CHECK_EQ(static_cast<int>(got.size()), 1);
  if (!got.empty()) CHECK_STR(got[0], "hello proto=1");
}

void test_two_messages_in_one_write_are_two_messages() {
  Rig rig("two");
  std::string err;
  CHECK(rig.server.start(&err));
  Peer peer;
  CHECK(peer.open(rig.path));
  rig.pump();

  peer.write("status\nping\n");
  rig.pump();

  CHECK_EQ(static_cast<int>(rig.lines.size()), 2);
  if (rig.lines.size() == 2) {
    CHECK_STR(rig.lines[0], "status");
    CHECK_STR(rig.lines[1], "ping");
  }
}

void test_one_message_in_two_writes_is_one_message() {
  Rig rig("split");
  std::string err;
  CHECK(rig.server.start(&err));
  Peer peer;
  CHECK(peer.open(rig.path));
  rig.pump();

  peer.write("pi");
  rig.pump();
  // Nothing yet: half a line is not a line, and a transport that guessed
  // otherwise would hand the daemon a verb that does not exist.
  CHECK_EQ(static_cast<int>(rig.lines.size()), 0);

  peer.write("ng\n");
  rig.pump();
  CHECK_EQ(static_cast<int>(rig.lines.size()), 1);
  if (!rig.lines.empty()) CHECK_STR(rig.lines[0], "ping");

  bool saw_pong = false;
  for (const std::string& line : peer.read_lines()) {
    if (line == "pong") saw_pong = true;
  }
  CHECK(saw_pong);
}

void test_an_announcement_reaches_every_peer() {
  Rig rig("announce");
  std::string err;
  CHECK(rig.server.start(&err));
  Peer a, b;
  CHECK(a.open(rig.path));
  CHECK(b.open(rig.path));
  rig.pump();
  CHECK_EQ(static_cast<int>(rig.opened.size()), 2);

  for (MsgPeer* peer : rig.opened) peer->send("bench offset=-6.2500");
  rig.pump();

  int seen = 0;
  for (const std::string& line : a.read_lines()) {
    if (line == "bench offset=-6.2500") ++seen;
  }
  for (const std::string& line : b.read_lines()) {
    if (line == "bench offset=-6.2500") ++seen;
  }
  CHECK_EQ(seen, 2);
}

void test_a_peer_that_hangs_up_is_forgotten() {
  Rig rig("hangup");
  std::string err;
  CHECK(rig.server.start(&err));
  {
    Peer peer;
    CHECK(peer.open(rig.path));
    rig.pump();
    CHECK_EQ(static_cast<int>(rig.server.clients()), 1);
  }
  rig.pump();

  CHECK_EQ(static_cast<int>(rig.server.clients()), 0);
  CHECK_EQ(static_cast<int>(rig.closed.size()), 1);
}

void test_a_peer_that_half_closes_is_still_answered() {
  Rig rig("halfclose");
  std::string err;
  CHECK(rig.server.start(&err));
  Peer peer;
  CHECK(peer.open(rig.path));
  rig.pump();

  // Write a command and shut down the write side without waiting. The socket
  // is then readable and hung up at the same instant, and the bytes are still
  // worth reading -- which is the property src/loop.h reports kHangup
  // alongside kRead for rather than instead of it.
  peer.write("ping\n");
  ::shutdown(peer.fd(), SHUT_WR);
  rig.pump();

  CHECK_EQ(static_cast<int>(rig.lines.size()), 1);
  bool saw_pong = false;
  for (const std::string& line : peer.read_lines()) {
    if (line == "pong") saw_pong = true;
  }
  CHECK(saw_pong);
}

void test_a_line_that_never_ends_is_refused_rather_than_buffered() {
  Rig rig("long");
  std::string err;
  CHECK(rig.server.start(&err));
  Peer peer;
  CHECK(peer.open(rig.path));
  rig.pump();

  // Past LineReader's cap, with no newline in sight. Without a cap this is an
  // unbounded allocation driven by whatever is on the other end of a cable.
  peer.write(std::string(LineReader::kMaxLine + 64, 'x'));
  rig.pump();
  CHECK_EQ(static_cast<int>(rig.lines.size()), 0);

  // The complaint comes at the newline that ends the over-long line, not at
  // the byte that crossed the cap: one lost command should be reported once,
  // rather than once per read the tail of it happened to arrive in.
  peer.write("\n");
  rig.pump();
  CHECK_EQ(static_cast<int>(rig.lines.size()), 0);
  bool complained = false;
  for (const std::string& line : peer.read_lines()) {
    if (line.find("line-too-long") != std::string::npos) complained = true;
  }
  CHECK(complained);
  // ...and the connection survives it, because a peer that sent one bad line
  // is usually a cable that dropped a newline rather than an attacker.
  CHECK_EQ(static_cast<int>(rig.server.clients()), 1);
}

void test_a_peer_that_stops_reading_is_dropped() {
  Rig rig("stalled");
  std::string err;
  CHECK(rig.server.start(&err));
  rig.server.set_max_pending(4096);
  Peer peer;
  CHECK(peer.open(rig.path));
  rig.pump();
  CHECK_EQ(static_cast<int>(rig.server.clients()), 1);

  // The peer never reads. Announcements pile up in the kernel's buffer, then
  // in ours, and past the cap the honest thing is to let the peer go: a
  // daemon that holds them all instead is a daemon that runs out of memory
  // because a window was scrolled.
  MsgPeer* stalled = rig.opened.front();
  for (int i = 0; i < 20000; ++i) {
    stalled->send("bench offset=-6.2500 spread=0.0010 boxes=3 live=3");
  }
  // The pointer above is still good here: a failed peer is marked, not
  // dropped, until the loop next runs. That is exactly what the deferral in
  // Client::send exists to guarantee.
  rig.pump();

  CHECK_EQ(static_cast<int>(rig.server.clients()), 0);
  CHECK_EQ(static_cast<int>(rig.closed.size()), 1);
}

void test_a_second_server_will_not_take_the_socket() {
  Rig rig("dup");
  std::string err;
  CHECK(rig.server.start(&err));

  std::unique_ptr<Loop> other_loop = make_loop();
  LineServer other(other_loop.get(), rig.path);
  std::string other_err;
  CHECK(!other.start(&other_err));
  CHECK(other_err.find("already listening") != std::string::npos);

  // ...and the first one still works, which is the point of failing rather
  // than unlinking whatever was there.
  Peer peer;
  CHECK(peer.open(rig.path));
  rig.pump();
  CHECK_EQ(static_cast<int>(rig.server.clients()), 1);
}

void test_a_stale_socket_file_is_cleared_away() {
  const std::string path = temp_path("stale");
  ::unlink(path.c_str());
  // A socket file with nothing behind it, which is what a crash leaves.
  {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.c_str(), path.size());
    CHECK(::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) ==
          0);
    ::close(fd);  // bound, never listened on, now gone
  }

  std::unique_ptr<Loop> loop = make_loop();
  LineServer server(loop.get(), path);
  std::string err;
  CHECK(server.start(&err));
  server.stop();
  ::unlink(path.c_str());
}

}  // namespace

int main() {
  test_a_client_connects_and_is_greeted();
  test_two_messages_in_one_write_are_two_messages();
  test_one_message_in_two_writes_is_one_message();
  test_an_announcement_reaches_every_peer();
  test_a_peer_that_hangs_up_is_forgotten();
  test_a_peer_that_half_closes_is_still_answered();
  test_a_line_that_never_ends_is_refused_rather_than_buffered();
  test_a_peer_that_stops_reading_is_dropped();
  test_a_second_server_will_not_take_the_socket();
  test_a_stale_socket_file_is_cleared_away();
  return octotest::report("test_boxsock");
}
