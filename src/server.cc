#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>

#include "proto.h"
#include "timeutil.h"

namespace octo {

namespace {

// A client that connects and then says nothing must not hold a slot forever.
constexpr double kClientTimeout = 10.0;
constexpr size_t kMaxRequest = 4096;

bool set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool make_parents(const std::string& path, std::string* err) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) return true;
  const std::string dir = path.substr(0, slash);
  // Private: the socket exposes what is in the room and who is in it.
  if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
    if (err) *err = "cannot create " + dir + ": " + strerror(errno);
    return false;
  }
  return true;
}

std::string trim(const std::string& s) {
  size_t begin = 0, end = s.size();
  while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) ++begin;
  while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                         s[end - 1] == '\r' || s[end - 1] == '\n')) {
    --end;
  }
  return s.substr(begin, end - begin);
}

}  // namespace

std::string default_socket_path() {
  const char* home = std::getenv("HOME");
  const std::string base = home && *home ? home : "/tmp";
  return base + "/Library/Application Support/octomancer/octomancerd.sock";
}

Handler registry_handler(Registry& registry) {
  return registry_handler(registry, SnapshotSource());
}

Handler registry_handler(Registry& registry, SnapshotSource snapshot) {
  return [&registry, snapshot](const std::string& command) -> std::string {
    auto shot = [&registry, &snapshot]() {
      return snapshot ? snapshot() : registry.snapshot();
    };
    if (command == "status" || command.empty()) {
      return render_text(shot());
    }
    if (command == "json") {
      return render_json(shot()) + "\n";
    }
    if (command == "ping") {
      return "octomancer " + std::to_string(kProtocolVersion) + "\npong\n";
    }
    // `forget <id>` -- the id verbatim, because that is what the snapshot
    // hands a client and asking it to quote or escape its own way back would
    // invent a second spelling of the same thing.
    if (command.compare(0, 7, "forget ") == 0) {
      const std::string id = command.substr(7);
      if (id.empty()) {
        return "octomancer " + std::to_string(kProtocolVersion) +
               "\nerror forget needs a device id\n";
      }
      // "Not here" is reported as success. The caller asked for the device to
      // be gone, and it is; making them tell the difference between "removed"
      // and "was never here" would only invite them to treat one of the two
      // as a failure worth showing somebody.
      const bool had = registry.forget(id);
      return "octomancer " + std::to_string(kProtocolVersion) + "\nforgot " +
             escape(id) + (had ? "" : " (was not listed)") + "\n";
    }
    return "octomancer " + std::to_string(kProtocolVersion) +
           "\nerror unknown command: " + escape(command) + "\n";
  };
}

Server::Server(Handler handler, std::string path)
    : handler_(std::move(handler)), path_(std::move(path)) {}

Server::Server(Registry& registry, std::string path)
    : handler_(registry_handler(registry)), path_(std::move(path)) {}

Server::~Server() { shutdown(); }

bool Server::start(std::string* err) {
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
  // accepts is a daemon already doing this job.
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
    shutdown();
    return false;
  }
  return true;
}

void Server::shutdown() {
  for (Client& c : clients_) {
    if (c.fd >= 0) ::close(c.fd);
  }
  clients_.clear();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (bound_) {
    ::unlink(path_.c_str());
    bound_ = false;
  }
}

std::string Server::handle(const std::string& request) const {
  if (!handler_) {
    return "octomancer " + std::to_string(kProtocolVersion) +
           "\nerror no handler\n";
  }
  return handler_(trim(request));
}

void Server::drop(size_t index) {
  if (clients_[index].fd >= 0) ::close(clients_[index].fd);
  clients_.erase(clients_.begin() + static_cast<long>(index));
}

void Server::serve(int timeout_ms) {
  if (listen_fd_ < 0) return;

  std::vector<struct pollfd> fds;
  fds.reserve(clients_.size() + 1);
  fds.push_back({listen_fd_, POLLIN, 0});
  for (const Client& c : clients_) {
    short events = 0;
    if (!c.replied) events |= POLLIN;
    if (!c.out.empty()) events |= POLLOUT;
    fds.push_back({c.fd, events, 0});
  }

  const int ready = ::poll(fds.data(), fds.size(), timeout_ms);
  const double now = mono_now();

  if (ready > 0 && (fds[0].revents & POLLIN)) {
    for (;;) {
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) break;
      set_nonblocking(fd);
      Client c;
      c.fd = fd;
      c.deadline = now + kClientTimeout;
      clients_.push_back(std::move(c));
    }
  }

  // Walk backwards so erasing does not disturb the indices still to come.
  for (size_t i = clients_.size(); i-- > 0;) {
    Client& c = clients_[i];
    const size_t slot = i + 1;
    const short revents = slot < fds.size() && fds[slot].fd == c.fd
                              ? fds[slot].revents
                              : 0;

    if (revents & (POLLERR | POLLNVAL)) {
      drop(i);
      continue;
    }

    if (revents & POLLIN) {
      char buf[1024];
      const ssize_t n = ::read(c.fd, buf, sizeof buf);
      if (n == 0) {
        // Peer closed without asking anything.
        if (c.out.empty()) {
          drop(i);
          continue;
        }
      } else if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          drop(i);
          continue;
        }
      } else {
        c.in.append(buf, static_cast<size_t>(n));
        if (c.in.size() > kMaxRequest) {
          c.out = "octomancer " + std::to_string(kProtocolVersion) +
                  "\nerror request too long\n";
          c.replied = true;
        }
      }
    }

    if (!c.replied) {
      const size_t nl = c.in.find('\n');
      if (nl != std::string::npos) {
        c.out = handle(c.in.substr(0, nl));
        c.replied = true;
      } else if (revents & POLLHUP) {
        // Closed the write side without a newline: treat what arrived as the
        // whole request, so `printf status | nc -U` behaves.
        c.out = handle(c.in);
        c.replied = true;
      }
    }

    if (!c.out.empty() && (revents & POLLOUT || c.replied)) {
      const ssize_t n = ::write(c.fd, c.out.data(), c.out.size());
      if (n > 0) {
        c.out.erase(0, static_cast<size_t>(n));
      } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                 errno != EINTR) {
        drop(i);
        continue;
      }
    }

    if (c.replied && c.out.empty()) {
      drop(i);
      continue;
    }
    if (now > c.deadline) {
      drop(i);
      continue;
    }
  }
}

}  // namespace octo
