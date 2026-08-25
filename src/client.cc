#include "client.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "proto.h"
#include "timeutil.h"

namespace octo {

bool query(const std::string& socket_path, const std::string& command,
           std::string* reply, std::string* err, double timeout) {
  struct sockaddr_un addr;
  if (socket_path.size() >= sizeof(addr.sun_path)) {
    if (err) *err = "socket path is too long: " + socket_path;
    return false;
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    if (err) *err = std::string("socket: ") + strerror(errno);
    return false;
  }

  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, socket_path.c_str(), socket_path.size());

  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0) {
    const int saved = errno;
    ::close(fd);
    if (err) {
      // The overwhelmingly common failure is that the agent is not running,
      // and saying so beats reporting ENOENT at the user.
      if (saved == ENOENT || saved == ECONNREFUSED) {
        *err = "no daemon is listening at " + socket_path;
      } else {
        *err = "connect " + socket_path + ": " + strerror(saved);
      }
    }
    return false;
  }

  const double deadline = mono_now() + timeout;
  std::string request = command;
  if (request.empty() || request.back() != '\n') request.push_back('\n');

  size_t sent = 0;
  while (sent < request.size()) {
    const double left = deadline - mono_now();
    if (left <= 0) {
      ::close(fd);
      if (err) *err = "timed out sending to " + socket_path;
      return false;
    }
    struct pollfd pfd = {fd, POLLOUT, 0};
    if (::poll(&pfd, 1, static_cast<int>(left * 1000)) <= 0) continue;
    const ssize_t n = ::write(fd, request.data() + sent, request.size() - sent);
    if (n > 0) {
      sent += static_cast<size_t>(n);
    } else if (n < 0 && errno != EINTR && errno != EAGAIN) {
      ::close(fd);
      if (err) *err = std::string("write: ") + strerror(errno);
      return false;
    }
  }
  ::shutdown(fd, SHUT_WR);

  std::string out;
  for (;;) {
    const double left = deadline - mono_now();
    if (left <= 0) {
      ::close(fd);
      if (err) *err = "timed out waiting for " + socket_path;
      return false;
    }
    struct pollfd pfd = {fd, POLLIN, 0};
    const int ready = ::poll(&pfd, 1, static_cast<int>(left * 1000));
    if (ready <= 0) continue;
    char buf[4096];
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      ::close(fd);
      if (err) *err = std::string("read: ") + strerror(errno);
      return false;
    }
    out.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  if (reply) *reply = std::move(out);
  return true;
}

bool fetch(const std::string& socket_path, Snapshot* out, std::string* err,
           double timeout) {
  std::string reply;
  if (!query(socket_path, "status", &reply, err, timeout)) return false;
  return parse_text(reply, out, err);
}

}  // namespace octo
