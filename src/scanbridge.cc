#include "scanbridge.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <deque>
#include <mutex>
#include <utility>

namespace octo {

// One thing the radio said. A struct with a kind rather than three queues,
// because the order between them is information: "the radio powered off"
// arriving before the last advertisement it explains would read as a daemon
// that kept hearing a box after the antenna went away.
struct ScanBridge::Shared {
  enum Kind { kAdvert, kCamera, kState };

  struct Event {
    Kind kind = kAdvert;
    Advert advert;
    Sighting sighting;
    std::string state;
  };

  std::mutex mu;
  std::deque<Event> queue;
  size_t dropped = 0;
  bool signalled = false;  // a byte is already in the pipe, unread
  bool alive = false;      // the ScanBridge still exists
  int read_fd = -1;
  int write_fd = -1;

  ~Shared() {
    if (read_fd >= 0) ::close(read_fd);
    if (write_fd >= 0) ::close(write_fd);
  }

  // Called from whatever thread the radio delivers on.
  void push(Event event) {
    bool poke = false;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (!alive) return;
      // Oldest first: the newest advertisement is the one that says something
      // about now, and it is the one worth keeping.
      while (queue.size() >= ScanBridge::kMaxQueued) {
        queue.pop_front();
        ++dropped;
      }
      queue.push_back(std::move(event));
      if (!signalled) {
        signalled = true;
        poke = true;
      }
    }
    if (!poke) return;
    const char byte = 'x';
    // One byte at a time and only when the pipe is empty, so this cannot
    // block: a write that blocked here would stall the radio's own queue
    // behind the loop that is trying to drain it.
    while (::write(write_fd, &byte, 1) < 0 && errno == EINTR) {
    }
  }
};

namespace {

bool make_pipe(int* read_fd, int* write_fd, std::string* err) {
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    *err = std::string("pipe: ") + strerror(errno);
    return false;
  }
  for (int i = 0; i < 2; ++i) {
    const int flags = ::fcntl(fds[i], F_GETFL, 0);
    if (flags < 0 || ::fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) != 0 ||
        ::fcntl(fds[i], F_SETFD, FD_CLOEXEC) != 0) {
      *err = std::string("pipe: ") + strerror(errno);
      ::close(fds[0]);
      ::close(fds[1]);
      return false;
    }
  }
  *read_fd = fds[0];
  *write_fd = fds[1];
  return true;
}

}  // namespace

ScanBridge::ScanBridge(Loop* loop) : shared_(new Shared), loop_(loop) {
  if (!make_pipe(&shared_->read_fd, &shared_->write_fd, &error_)) return;
  shared_->alive = true;

  Handle handle;
  handle.fd = shared_->read_fd;
  // Capturing `this` is safe here and needs saying, because src/box-notes.md
  // lists a captured `this` with nothing to cancel it as one of the four ways
  // this model goes wrong. The thing that cancels it is the destructor's
  // remove_source, and Loop::tick re-finds every ready source before
  // dispatching it -- so a bridge torn down inside another source's handler,
  // in the same tick this one was found ready, is not called back into.
  source_ = loop_->add_source(
      handle, kRead, [this](int) { drain(); },
      [](const std::string&) {
        // The read end of a pipe this object owns cannot fail on its own. If
        // it somehow does there is nothing useful to do about it, and dying
        // here would take a daemon down over a diagnostic channel.
      });
}

ScanBridge::~ScanBridge() {
  {
    std::lock_guard<std::mutex> lock(shared_->mu);
    shared_->alive = false;
    shared_->queue.clear();
  }
  if (source_ != kNoSource) loop_->remove_source(source_);
}

bool ScanBridge::ok() const { return shared_->write_fd >= 0; }
const std::string& ScanBridge::error() const { return error_; }

void ScanBridge::on_advert(Scanner::AdvertHandler h) {
  on_advert_ = std::move(h);
}
void ScanBridge::on_camera(Scanner::SightingHandler h) {
  on_camera_ = std::move(h);
}
void ScanBridge::on_state(Scanner::StateHandler h) { on_state_ = std::move(h); }

Scanner::AdvertHandler ScanBridge::advert_sink() {
  std::shared_ptr<Shared> shared = shared_;
  return [shared](const Advert& advert) {
    Shared::Event event;
    event.kind = Shared::kAdvert;
    event.advert = advert;
    shared->push(std::move(event));
  };
}

Scanner::SightingHandler ScanBridge::camera_sink() {
  std::shared_ptr<Shared> shared = shared_;
  return [shared](const Sighting& seen) {
    Shared::Event event;
    event.kind = Shared::kCamera;
    event.sighting = seen;
    shared->push(std::move(event));
  };
}

Scanner::StateHandler ScanBridge::state_sink() {
  std::shared_ptr<Shared> shared = shared_;
  return [shared](const std::string& state) {
    Shared::Event event;
    event.kind = Shared::kState;
    event.state = state;
    shared->push(std::move(event));
  };
}

void ScanBridge::drain() {
  // Empty the pipe first, and only then clear the flag that says a byte is in
  // it. The other order loses events: a push between clearing the flag and
  // reading the pipe writes a byte this read then eats, so its event is left
  // in the queue with no wake-up owing and no later push willing to write one
  // -- the flag is already set. This way round the worst case is a byte left
  // behind for an event already taken, which costs one spurious wake-up.
  char buf[64];
  while (::read(shared_->read_fd, buf, sizeof buf) > 0) {
  }

  // Then take the whole queue in one turn and call the handlers with the lock
  // released. A handler is entitled to take as long as it likes; holding a
  // lock the radio's thread needs while it runs would stall the radio behind
  // the daemon.
  std::deque<Shared::Event> batch;
  {
    std::lock_guard<std::mutex> lock(shared_->mu);
    batch.swap(shared_->queue);
    shared_->signalled = false;
  }

  for (Shared::Event& event : batch) {
    switch (event.kind) {
      case Shared::kAdvert:
        if (on_advert_) on_advert_(event.advert);
        break;
      case Shared::kCamera:
        if (on_camera_) on_camera_(event.sighting);
        break;
      case Shared::kState:
        if (on_state_) on_state_(event.state);
        break;
    }
  }
}

size_t ScanBridge::dropped() const {
  std::lock_guard<std::mutex> lock(shared_->mu);
  return shared_->dropped;
}

size_t ScanBridge::queued() const {
  std::lock_guard<std::mutex> lock(shared_->mu);
  return shared_->queue.size();
}

}  // namespace octo
