// The POSIX backend: poll(2), and a pipe to interrupt it.
//
// There is nothing clever here on purpose. This program waits on a handful of
// file descriptors -- a serial port, a listening socket, a few clients -- and
// poll(2) is the portable call that does exactly that. epoll and kqueue matter
// when the count runs to thousands; here they would be two more backends to
// keep correct in exchange for nothing measurable.
//
// The self-pipe is the only subtle part. CoreBluetooth delivers on a private
// dispatch queue and a USB CDC receive interrupt delivers in interrupt
// context, and neither can touch the loop's data structures. Both can write
// one byte to a pipe, and that is all wake() is: the poll returns, the loop
// runs, and whatever queued the work sees it on the right thread.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <memory>
#include <vector>

#include "loop.h"
#include "timeutil.h"

namespace octo {
namespace {

class PosixLoop : public Loop {
 public:
  PosixLoop() {
    int fds[2];
    if (::pipe(fds) == 0) {
      wake_r_ = fds[0];
      wake_w_ = fds[1];
      set_nonblocking(wake_r_);
      set_nonblocking(wake_w_);
    }
  }

  ~PosixLoop() override {
    if (wake_r_ >= 0) ::close(wake_r_);
    if (wake_w_ >= 0) ::close(wake_w_);
  }

 protected:
  double clock() const override { return mono_now(); }

  void wait(double max_wait, std::vector<std::pair<SourceId, int>>* ready,
            std::vector<SourceId>* failed) override {
    pfds_.clear();
    ids_.clear();

    for (const auto& s : sources()) {
      if (s.dead || s.handle.fd < 0 || s.interest == 0) continue;
      struct pollfd p;
      p.fd = s.handle.fd;
      p.events = 0;
      if (s.interest & kRead) p.events |= POLLIN;
      if (s.interest & kWrite) p.events |= POLLOUT;
      p.revents = 0;
      pfds_.push_back(p);
      ids_.push_back(s.id);
    }

    const size_t wake_index = pfds_.size();
    if (wake_r_ >= 0) {
      struct pollfd p;
      p.fd = wake_r_;
      p.events = POLLIN;
      p.revents = 0;
      pfds_.push_back(p);
    }

    int timeout_ms = -1;
    if (max_wait >= 0.0) {
      double ms = max_wait * 1000.0;
      // Round up. Rounding down means a timer due in 0.4 ms is polled with a
      // zero timeout, the loop spins, and the CPU is busy for the whole
      // fraction of a millisecond before it is due -- repeatedly.
      if (ms > 2147483000.0) ms = 2147483000.0;
      timeout_ms = static_cast<int>(ms + 0.999);
    }

    int n;
    do {
      n = ::poll(pfds_.empty() ? nullptr : pfds_.data(),
                 static_cast<nfds_t>(pfds_.size()), timeout_ms);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) return;

    if (wake_r_ >= 0 && wake_index < pfds_.size() &&
        (pfds_[wake_index].revents & POLLIN)) {
      char drain[64];
      while (::read(wake_r_, drain, sizeof drain) > 0) {
      }
    }

    for (size_t i = 0; i < ids_.size(); ++i) {
      const short re = pfds_[i].revents;
      if (re == 0) continue;
      // POLLERR and POLLNVAL are failures. POLLHUP is not: the peer has gone
      // away, but anything it already sent is still in the buffer and is still
      // worth reading. Reporting it as a failure is how a well-formed request
      // from a client that closed its write side gets thrown away.
      if (re & (POLLERR | POLLNVAL)) {
        failed->push_back(ids_[i]);
        continue;
      }
      int interest = 0;
      if (re & POLLIN) interest |= kRead;
      if (re & POLLOUT) interest |= kWrite;
      if (re & POLLHUP) interest |= kRead | kHangup;
      if (interest != 0) ready->emplace_back(ids_[i], interest);
    }
  }

  void wake_backend() override {
    if (wake_w_ < 0) return;
    const char one = 'w';
    ssize_t rc;
    do {
      rc = ::write(wake_w_, &one, 1);
    } while (rc < 0 && errno == EINTR);
    // A full pipe means a wake is already pending, which is the same outcome.
    (void)rc;
  }

 private:
  static void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  int wake_r_ = -1;
  int wake_w_ = -1;
  std::vector<struct pollfd> pfds_;
  std::vector<SourceId> ids_;
};

}  // namespace

std::unique_ptr<Loop> make_loop() { return std::make_unique<PosixLoop>(); }

}  // namespace octo
