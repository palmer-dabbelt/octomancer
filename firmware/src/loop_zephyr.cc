// The Zephyr backend: k_poll(), and a signal to interrupt it.
//
// src/loop.h names this file before it exists and says what it has to be:
// "loop_posix.cc is poll(2), the Zephyr backend is k_poll()". The three
// backends differ in how they wait and in nothing else -- every timer, every
// deadline and every dispatch rule is arithmetic in src/loop.cc that is
// tested once on the host and then simply used here.
//
// A source's Handle carries `object` rather than `fd`, which is the case
// src/loop.h's Handle was shaped for: Zephyr has no file descriptors. The
// object is a `struct k_poll_signal*`, and everything on this device that has
// something to say -- the USB CDC receive interrupt, the Bluetooth receive
// thread -- says it by raising one. That is deliberate. Both of those run on
// a thread that is not this one, and neither may touch the loop's vectors;
// raising a signal is the whole of what they are allowed to do, and it is
// exactly what the self-pipe does on POSIX.
#include <zephyr/kernel.h>

#include <memory>
#include <utility>
#include <vector>

#include "loop.h"

namespace octo {
namespace {

// k_poll takes a flat array, so the events have to be counted before they are
// built. This is the ceiling on live sources plus the wake signal; a device
// with one serial port and one radio uses three of them.
constexpr size_t kMaxEvents = 8;

class ZephyrLoop : public Loop {
 public:
  ZephyrLoop() { k_poll_signal_init(&wake_); }

 protected:
  // Ticks rather than milliseconds. k_uptime_get() is millisecond-resolution,
  // and a millisecond is a large unit here -- the whole point of the program
  // is arithmetic on a clock to within a frame. Converting through
  // microseconds keeps this independent of whatever the tick rate is
  // configured to be.
  double clock() const override {
    return static_cast<double>(k_ticks_to_us_floor64(k_uptime_ticks())) * 1e-6;
  }

  void wait(double max_wait, std::vector<std::pair<SourceId, int>>* ready,
            std::vector<SourceId>* failed) override {
    (void)failed;  // A signal cannot fail. Only a peer can, and it says so.

    struct k_poll_event events[kMaxEvents];
    struct k_poll_signal* signals[kMaxEvents];
    SourceId ids[kMaxEvents];
    size_t n = 0;

    for (const auto& s : sources()) {
      if (s.dead || s.handle.object == nullptr || s.interest == 0) continue;
      // kWrite is not offered. Nothing on this device waits to write: the CDC
      // transmitter is a ring buffer drained by its own interrupt, so a source
      // asking for kWrite would be asking for an event that is never raised.
      if ((s.interest & kRead) == 0) continue;
      if (n + 1 >= kMaxEvents) break;  // keep one slot for the wake signal
      signals[n] = static_cast<struct k_poll_signal*>(s.handle.object);
      ids[n] = s.id;
      k_poll_event_init(&events[n], K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY,
                        signals[n]);
      ++n;
    }

    const size_t wake_index = n;
    k_poll_event_init(&events[n], K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY,
                      &wake_);
    ++n;

    k_timeout_t timeout = K_FOREVER;
    if (max_wait >= 0.0) {
      // Round up, for the reason loop_posix.cc rounds up: rounding down means
      // a timer due in a fraction of a tick is waited on with a zero timeout,
      // and the loop spins until it is due.
      double us = max_wait * 1e6;
      if (us > 4e12) us = 4e12;  // ~46 days; k_timeout_t is finite
      timeout = K_USEC(static_cast<int64_t>(us + 0.999));
    }

    k_poll(events, static_cast<int>(n), timeout);

    // Reset before dispatching. A signal raised while a handler is running is
    // a wake the next wait must observe, and clearing it afterwards would
    // discard exactly that.
    if (events[wake_index].state == K_POLL_STATE_SIGNALED) {
      k_poll_signal_reset(&wake_);
    }
    for (size_t i = 0; i < wake_index; ++i) {
      if (events[i].state != K_POLL_STATE_SIGNALED) continue;
      k_poll_signal_reset(signals[i]);
      ready->emplace_back(ids[i], kRead);
    }
  }

  // The only method here that is safe from an interrupt, which is the whole
  // reason it exists. k_poll_signal_raise is ISR-safe and idempotent: a
  // signal already raised stays raised, so a busy room cannot overflow
  // anything.
  void wake_backend() override { k_poll_signal_raise(&wake_, 1); }

 private:
  struct k_poll_signal wake_;
};

}  // namespace

std::unique_ptr<Loop> make_loop() { return std::make_unique<ZephyrLoop>(); }

}  // namespace octo
