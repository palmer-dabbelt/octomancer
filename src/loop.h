// One place for the program to wait, so that there is only ever one thread.
//
// This exists because of a measurement rather than a preference. The Zephyr
// SDK's arm-zephyr-eabi libstdc++ is built with _GLIBCXX_HAS_GTHREADS
// undefined in every one of its multilib variants, so std::thread, std::mutex
// and std::condition_variable do not exist on the target at all. That is not a
// Kconfig option to turn on; it is how the toolchain ships. Six files in this
// tree fail to cross-compile on nothing but that, and they are the six that
// have to run on a standalone box.
//
// So the radio-free core stops being thread-safe and starts being
// single-threaded, which it always wanted to be. A controller is passive -- it
// answers when spoken to and reports what happened -- and both things this
// program waits on, a serial port and a socket, are pollable. There was never
// any concurrency here worth the locks; there was a reader thread because
// blocking reads are the easy way to write one, and everything above it grew
// mutexes to defend against that one decision.
//
// Nothing here blocks, and that is the rule the design turns on. Handlers are
// called with bytes that have already arrived, they mutate their own state,
// and anything that needs a round trip posts a continuation and returns. A
// wait that lives inside a call is what made src/camera_hci.cc park on a
// condition variable that only the reader thread could signal; with one thread
// and no reader, that same shape is a deadlock. Refusing to offer a blocking
// primitive is what keeps it from being written again.
//
// The backend is two virtuals -- what time is it, and wait until something
// happens -- because everything else is arithmetic that should be tested once
// rather than three times. loop_posix.cc is poll(2), the Zephyr backend is
// k_poll(), and loopfake.cc is a variable holding the time. That last one is
// the point: a whole sync cycle can be driven to completion in a test, on a
// machine with no radio, in no wall-clock time at all.
#ifndef OCTO_LOOP_H
#define OCTO_LOOP_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace octo {

using SourceId = uint32_t;
using TimerId = uint32_t;

inline constexpr SourceId kNoSource = 0;
inline constexpr TimerId kNoTimer = 0;

// What a backend needs in order to wait for something. POSIX fills in `fd`;
// Zephyr has no file descriptors and fills in `object` with a k_poll_event;
// the fake loop fills in `object` with itself. The core never looks inside
// either field, which is why this is a struct and not an int -- an int would
// have quietly become "the POSIX one" and made the other two backends liars.
struct Handle {
  int fd = -1;
  void* object = nullptr;

  bool valid() const { return fd >= 0 || object != nullptr; }
};

enum Interest : int {
  kRead = 1,
  kWrite = 2,
  // Reported alongside kRead, never instead of it. A half-closed socket still
  // has buffered bytes worth reading, and src/server.cc depends on reading
  // them: a client that writes a command and shuts down its write side without
  // a trailing newline is answered rather than dropped.
  kHangup = 4,
};

class Loop {
 public:
  // `interest` is the bitwise-or of the Interest values that fired.
  using ReadyHandler = std::function<void(int interest)>;
  using ErrorHandler = std::function<void(const std::string& why)>;
  using TimerHandler = std::function<void()>;

  virtual ~Loop();

  Loop(const Loop&) = delete;
  Loop& operator=(const Loop&) = delete;

  // Monotonic seconds. Never the wall clock: a clock step must not look like
  // a timer firing early, and on the box there is no wall clock at boot at
  // all.
  double now() const { return clock(); }

  // ------------------------------------------------------------- sources

  SourceId add_source(const Handle& handle, int interest, ReadyHandler on_ready,
                      ErrorHandler on_error);
  void set_interest(SourceId id, int interest);

  // Safe to call from inside the source's own handler, which is the usual
  // case: a read of zero bytes means the peer is gone and the handler that
  // noticed is the one that wants it removed. The entry is retired after the
  // dispatch finishes rather than during it, so the vector cannot move under
  // a handler that is still running.
  void remove_source(SourceId id);

  // -------------------------------------------------------------- timers

  TimerId at(double deadline, TimerHandler fn);
  TimerId after(double seconds, TimerHandler fn);

  // Repeating. A period that is missed -- because a handler ran long, or
  // because the fake clock jumped -- fires once and then realigns to the
  // future. It does not fire once per period skipped: the beacon catching up
  // on four minutes of missed broadcasts is a burst of radio nobody asked
  // for, and a sync cycle doing it is four connections to a camera.
  TimerId every(double period, TimerHandler fn);

  // Safe from inside any handler, including the timer's own.
  void cancel(TimerId id);

  // Monotonic deadline of the earliest live timer, or a negative number when
  // there are none. For a caller that wants to run the loop itself.
  double next_deadline() const;

  // ------------------------------------------------------------- running

  // Wait for at most `max_wait` seconds, then dispatch everything that came
  // due: I/O first, then timers. Returns false once the loop has been
  // stopped. A negative `max_wait` means "until something happens", which is
  // the only thing that ever actually idles the CPU.
  bool tick(double max_wait);

  // tick() until stop(). This is main().
  void run();

  // Safe from inside a handler; the current dispatch finishes first.
  void stop();
  bool running() const { return running_; }

  // True while a handler is on the stack. Exposed so that code which must not
  // be re-entered can assert rather than discover.
  bool dispatching() const { return depth_ > 0; }

  // Make the current or next wait return immediately. The only method here
  // that is safe to call from another thread or from an interrupt, which is
  // how CoreBluetooth's dispatch queue and a USB CDC receive interrupt get
  // their work onto this thread.
  void wake();

 protected:
  Loop();

  virtual double clock() const = 0;

  // Wait up to max_wait seconds. Append (id, interest) for each source that is
  // ready and each id that has failed. A backend that has nothing to wait on
  // still has to honour max_wait, because timers depend on it.
  virtual void wait(double max_wait,
                    std::vector<std::pair<SourceId, int>>* ready,
                    std::vector<SourceId>* failed) = 0;

  virtual void wake_backend() = 0;

  struct Source {
    SourceId id = kNoSource;
    Handle handle;
    int interest = 0;
    bool dead = false;
    ReadyHandler on_ready;
    ErrorHandler on_error;
  };

  // For the backends, which need the handle and interest of every live source
  // in order to build whatever their wait takes.
  const std::vector<Source>& sources() const { return sources_; }

 private:
  struct Timer {
    TimerId id = kNoTimer;
    double deadline = 0.0;
    double period = 0.0;  // zero for a one-shot
    uint64_t seq = 0;     // creation order, to break deadline ties
    bool dead = false;
    TimerHandler fn;
  };

  void sweep();
  Source* find(SourceId id);

  std::vector<Source> sources_;
  std::vector<Timer> timers_;
  SourceId next_source_ = 1;
  TimerId next_timer_ = 1;
  uint64_t next_seq_ = 1;
  int depth_ = 0;
  bool running_ = true;
  bool need_sweep_ = false;
};

// The loop for this host. poll(2) everywhere this program builds today.
std::unique_ptr<Loop> make_loop();

}  // namespace octo

#endif  // OCTO_LOOP_H
