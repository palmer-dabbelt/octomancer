// The glue between FakeBench and the Scanner interface.
//
// Deliberately almost empty. Everything that could be got wrong -- what a box
// is transmitting, when it transmits, when it stops -- is in src/fakebench.cc
// and is tested there; what is left here is a thread that wakes up, asks, and
// hands the answers to the callbacks. That is the same division the real
// backends have, and it is what keeps the fake honest: a bug that only shows
// up through this file is a bug in threading, not in the bench.
//
// A thread rather than the event loop in src/loop.h, because Scanner's
// contract is that callbacks arrive on the scanner's own queue and not the
// caller's -- octomancerd's ScanBridge exists precisely to move them back --
// and a fake that delivered them inline would let a caller pass here while
// deadlocking against CoreBluetooth.
#include <atomic>
#include <cstdio>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "fakebench.h"
#include "radio.h"
#include "scanner.h"
#include "timeutil.h"

namespace octo {
namespace {

class FakeScanner : public Scanner {
 public:
  FakeScanner(FakeBench bench, AdvertHandler on_advert,
              SightingHandler on_camera, StateHandler on_state)
      : bench_(std::move(bench)),
        on_advert_(std::move(on_advert)),
        on_camera_(std::move(on_camera)),
        on_state_(std::move(on_state)) {}

  ~FakeScanner() override { stop(); }

  bool start(std::string* err) override {
    (void)err;
    if (running_) return true;
    running_ = true;
    // The run's own zero. Everything the bench knows is expressed against
    // seconds since the scan began, so that a spec saying "goes quiet after
    // sixty seconds" means sixty seconds after somebody started looking.
    mono0_ = mono_now();
    wall0_ = wall_now();
    since_ = -1.0;
    // Said before any advert, as CoreBluetooth does: the state callback is how
    // a caller learns the radio is usable, and one that never fires is exactly
    // the failure a refused Bluetooth permission produces. A fake that stayed
    // silent here would make every caller look broken.
    if (on_state_) on_state_("poweredOn");
    thread_ = std::thread([this] { run(); });
    return true;
  }

  void stop() override {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
  }

 private:
  void run() {
    while (running_) {
      // Fine enough that a half-second box is not visibly quantised by it, and
      // coarse enough not to spin.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (!running_) break;
      const double now = mono_now() - mono0_;
      std::vector<Advert> adverts =
          adverts_between(bench_, since_, now, mono0_, wall0_);
      std::vector<Sighting> seen =
          sightings_between(bench_, since_, now, mono0_, wall0_);
      since_ = now;
      if (on_advert_) {
        for (const Advert& a : adverts) on_advert_(a);
      }
      if (on_camera_) {
        for (const Sighting& s : seen) on_camera_(s);
      }
    }
  }

  const FakeBench bench_;
  const AdvertHandler on_advert_;
  const SightingHandler on_camera_;
  const StateHandler on_state_;

  std::atomic<bool> running_{false};
  std::thread thread_;
  double mono0_ = 0.0;
  double wall0_ = 0.0;
  double since_ = -1.0;
};

}  // namespace

std::unique_ptr<Scanner> make_fake_scanner(Scanner::AdvertHandler on_advert,
                                           Scanner::SightingHandler on_camera,
                                           Scanner::StateHandler on_state) {
  FakeBench bench;
  std::string err;
  if (!FakeBench::parse(radio_options().fake, &bench, &err)) {
    // Refusing rather than falling back to the standard bench. A spec with a
    // typo in it is somebody asking for a particular experiment; running a
    // different one and saying nothing is how a whole afternoon's results turn
    // out to have been about the wrong thing.
    std::fprintf(stderr, "octomancer: %s\n", err.c_str());
    return nullptr;
  }
  return std::unique_ptr<Scanner>(new FakeScanner(
      std::move(bench), std::move(on_advert), std::move(on_camera),
      std::move(on_state)));
}

}  // namespace octo
