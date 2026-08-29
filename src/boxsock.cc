#include "boxsock.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace octo {

namespace {

bool set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool make_parents(const std::string& path, std::string* err) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) return true;
  const std::string dir = path.substr(0, slash);
  // Private: this socket says what is in the room and lets a caller change a
  // camera's clock.
  if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
    if (err) *err = "cannot create " + dir + ": " + strerror(errno);
    return false;
  }
  return true;
}

}  // namespace

std::string default_box_socket_path() {
  const char* home = std::getenv("HOME");
  const std::string base = home && *home ? home : "/tmp";
  return base + "/Library/Application Support/octomancer/octomancer-syncd.sock";
}

// One connection. The peer identity the daemon sees is this object's address,
// which is why it is heap-allocated and stable for as long as the connection
// is: the daemon holds the pointer in its list of peers.
class LineServer::Client : public MsgPeer {
 public:
  Client(LineServer* server, int fd) : server_(server), fd_(fd) {}

  ~Client() override {
    if (fd_ >= 0) ::close(fd_);
  }

  void send(const std::string& line) override {
    if (fd_ < 0 || failed_) return;
    out_ += line;
    out_ += '\n';
    // A failure here is *not* acted on here. send() is called from inside the
    // daemon's loop over its list of peers, and dropping a peer calls back
    // into that daemon to remove it from the very list being walked. So the
    // connection is marked and swept on the next turn of the loop, which is
    // the same shape doc/box-notes.md warns about under "a reference held
    // across a call that can destroy what it refers to".
    if (!flush()) {
      failed_ = true;
      server_->schedule_sweep();
    }
  }

  int fd() const { return fd_; }
  SourceId source = kNoSource;

  // Returns false when the connection is finished with, for whatever reason:
  // the peer went away, the socket failed, or it stopped reading for long
  // enough to be a problem.
  bool on_ready(int interest) {
    if ((interest & kWrite) != 0 && !flush()) return false;
    if ((interest & kRead) == 0 && (interest & kHangup) == 0) return true;

    char buf[1024];
    for (;;) {
      const ssize_t n = ::read(fd_, buf, sizeof buf);
      if (n > 0) {
        std::vector<std::string> lines;
        if (!reader_.feed(buf, static_cast<size_t>(n), &lines)) {
          // Said once, and not fatal. A line over the cap is a peer that is
          // not speaking this protocol, or a cable that dropped a newline;
          // either way the rest of the connection may still be good.
          send("err reason=line-too-long");
        }
        for (const std::string& line : lines) {
          if (server_->on_line_) server_->on_line_(this, line);
          // A handler is entitled to have closed this connection, and
          // everything after that would be reading a freed object.
          if (fd_ < 0) return false;
        }
        continue;
      }
      if (n == 0) return false;  // the peer hung up
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      return false;
    }

    // A half-closed socket is reported as readable and hung up at the same
    // time, and the bytes it already sent are worth reading -- which is why
    // this is checked after the read rather than instead of it.
    if ((interest & kHangup) != 0 && out_.empty()) return false;
    return true;
  }

  // False when this connection should be dropped.
  bool flush() {
    if (fd_ < 0) return false;
    while (!out_.empty()) {
      // send() with MSG_NOSIGNAL rather than write(), because writing to a
      // peer that has hung up raises SIGPIPE, and the default disposition of
      // SIGPIPE is to kill the process. A daemon that dies because a UI was
      // closed at the wrong moment is not a daemon. On macOS the flag does
      // not exist and SO_NOSIGPIPE on the socket does the same job; both are
      // set, because whichever is available is the one that matters.
#ifdef MSG_NOSIGNAL
      const int flags = MSG_NOSIGNAL;
#else
      const int flags = 0;
#endif
      const ssize_t n = ::send(fd_, out_.data(), out_.size(), flags);
      if (n > 0) {
        out_.erase(0, static_cast<size_t>(n));
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      return false;
    }

    // Only ask about writability while there is something to write. A socket
    // that is idle and writable is every socket, and asking about that would
    // spin the loop for nothing. Set before the cap is judged, so that a peer
    // which is merely behind gets the wake-up it needs to catch up.
    if (server_->loop_ != nullptr && source != kNoSource) {
      server_->loop_->set_interest(source,
                                   out_.empty() ? kRead : (kRead | kWrite));
    }
    return out_.size() <= server_->max_pending_;
  }

  bool failed() const { return failed_; }

  void close_fd() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

 private:
  LineServer* server_ = nullptr;
  int fd_ = -1;
  bool failed_ = false;
  std::string out_;
  LineReader reader_;
};

LineServer::LineServer(Loop* loop, std::string path)
    : loop_(loop), path_(std::move(path)), alive_(new bool(true)) {}

LineServer::~LineServer() {
  *alive_ = false;
  // Forget the handlers before closing the connections, so that tearing this
  // down never calls back into the object that owns it. That object is very
  // likely being destroyed too -- it declared this one as a member -- and
  // whichever of its fields the handler touches may already be gone. A
  // caller that wants to hear about the connections closing calls stop()
  // while it is still alive, which is what the daemon does.
  //
  // Found by a test that crashed about one run in three: the fixture's
  // on_close handler appended to a vector its own destructor had already
  // run.
  on_open_ = nullptr;
  on_line_ = nullptr;
  on_close_ = nullptr;
  stop();
}

void LineServer::schedule_sweep() {
  if (sweep_pending_) return;
  sweep_pending_ = true;
  std::shared_ptr<bool> alive = alive_;
  loop_->after(0.0, [this, alive]() {
    if (!*alive) return;
    sweep_pending_ = false;
    sweep();
  });
}

void LineServer::sweep() {
  for (;;) {
    Client* doomed = nullptr;
    for (const std::unique_ptr<Client>& client : clients_) {
      if (client->failed()) {
        doomed = client.get();
        break;
      }
    }
    if (doomed == nullptr) return;
    drop(doomed);
  }
}

void LineServer::on_open(OnOpen handler) { on_open_ = std::move(handler); }
void LineServer::on_line(OnLine handler) { on_line_ = std::move(handler); }
void LineServer::on_close(OnClose handler) { on_close_ = std::move(handler); }
size_t LineServer::clients() const { return clients_.size(); }
void LineServer::set_max_pending(size_t bytes) { max_pending_ = bytes; }

bool LineServer::start(std::string* err) {
  struct sockaddr_un addr;
  if (path_.size() >= sizeof(addr.sun_path)) {
    if (err) {
      *err = "socket path is too long (" + std::to_string(path_.size()) +
             " bytes, limit " + std::to_string(sizeof(addr.sun_path) - 1) + ")";
    }
    return false;
  }
  if (!make_parents(path_, err)) return false;

  // A socket file left behind by a crash looks exactly like a running daemon.
  // Tell them apart by trying to talk to it: something that refuses the
  // connection is a corpse and can be cleared away, whereas something that
  // accepts is a daemon already doing this job. The same reasoning as
  // src/server.cc, and the same code, because two different answers to it
  // would be worse than one duplicated one.
  {
    const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
      memset(&addr, 0, sizeof addr);
      addr.sun_family = AF_UNIX;
      memcpy(addr.sun_path, path_.c_str(), path_.size());
      if (::connect(probe, reinterpret_cast<struct sockaddr*>(&addr),
                    sizeof addr) == 0) {
        ::close(probe);
        if (err) *err = "another daemon is already listening on " + path_;
        return false;
      }
      ::close(probe);
    }
    ::unlink(path_.c_str());
  }

  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    if (err) *err = std::string("socket: ") + strerror(errno);
    return false;
  }
  set_nonblocking(listen_fd_);

  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, path_.c_str(), path_.size());

  const mode_t saved = ::umask(0077);
  const int rc = ::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                        sizeof addr);
  ::umask(saved);
  if (rc != 0) {
    if (err) *err = "bind " + path_ + ": " + strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  bound_ = true;

  if (::listen(listen_fd_, 16) != 0) {
    if (err) *err = std::string("listen: ") + strerror(errno);
    stop();
    return false;
  }

  Handle handle;
  handle.fd = listen_fd_;
  listen_source_ = loop_->add_source(
      handle, kRead, [this](int) { accept_one(); },
      [this](const std::string&) {
        // The listening socket failing is not something to carry on through;
        // there is nothing left to accept on.
        stop();
      });
  return true;
}

void LineServer::stop() {
  while (!clients_.empty()) drop(clients_.back().get());
  if (listen_source_ != kNoSource) {
    loop_->remove_source(listen_source_);
    listen_source_ = kNoSource;
  }
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (bound_) {
    ::unlink(path_.c_str());
    bound_ = false;
  }
}

void LineServer::accept_one() {
  // Everything that is waiting, not one per turn of the loop: the listening
  // socket is level-triggered, so one at a time would still work, and it
  // would take one wake-up per connection to say so.
  for (;;) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) break;
    set_nonblocking(fd);
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    const int nosigpipe = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof nosigpipe);
#endif

    clients_.push_back(std::unique_ptr<Client>(new Client(this, fd)));
    Client* client = clients_.back().get();

    Handle handle;
    handle.fd = fd;
    client->source = loop_->add_source(
        handle, kRead,
        [this, client](int interest) {
          if (!client->on_ready(interest)) drop(client);
        },
        [this, client](const std::string&) { drop(client); });

    if (on_open_) on_open_(client);
  }
}

void LineServer::drop(Client* client) {
  const auto it = std::find_if(
      clients_.begin(), clients_.end(),
      [client](const std::unique_ptr<Client>& c) { return c.get() == client; });
  if (it == clients_.end()) return;

  // Tell the daemon before the object goes, because the pointer it holds is
  // the identity it will look for, and afterwards there is nothing to name.
  if (on_close_) on_close_(client);
  if (client->source != kNoSource) loop_->remove_source(client->source);
  client->close_fd();
  clients_.erase(it);
}

}  // namespace octo
