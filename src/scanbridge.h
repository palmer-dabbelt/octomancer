// Getting what the radio heard onto the loop's thread.
//
// This exists because of one fact about one backend. CoreBluetooth delivers
// advertisements on a private dispatch queue of its own, and there is no way
// to ask it not to; the dongle backend in src/scanner_hci.cc already delivers
// on the loop's thread, and the box will too. So there is exactly one place
// where another thread's work has to become this thread's work, and this is
// it -- rather than every structure downstream growing a lock to defend
// against it, which is what src/registry.h used to do and what the box cannot
// have (see src/loop.h for why a lock is not available on the target at all).
//
// The mechanism is a pipe. A sink handed to make_ble_scanner appends to a
// queue under a lock and writes one byte; the loop notices the byte, drains
// the queue on its own thread, and calls the handlers. Handlers therefore run
// with the same guarantees as everything else on the loop: one at a time, on
// one thread, with nothing to lock.
//
// A byte is written only when the pipe is not already carrying one. Otherwise
// a radio in a busy room fills the pipe buffer, the write blocks, and the
// dispatch queue stalls behind a loop that is trying to drain it.
//
// The queue is capped, and what it drops it counts. An advertisement is a
// perishable statement about the present -- the next one is a few hundred
// milliseconds behind it -- so a bounded queue that discards the oldest is
// closer to right than an unbounded one that eventually consumes a device with
// 256 KB of RAM. Silence about the discards would not be, which is what
// dropped() is for.
#ifndef OCTO_SCANBRIDGE_H
#define OCTO_SCANBRIDGE_H

#include <cstddef>
#include <memory>
#include <string>

#include "loop.h"
#include "scanner.h"

namespace octo {

class ScanBridge {
 public:
  // Registers a source on `loop`, which must outlive this object.
  explicit ScanBridge(Loop* loop);
  ~ScanBridge();

  ScanBridge(const ScanBridge&) = delete;
  ScanBridge& operator=(const ScanBridge&) = delete;

  // False when the pipe could not be created, which is the only way
  // constructing this can fail. A caller that ignores it gets a bridge that
  // accepts everything and delivers nothing.
  bool ok() const;
  const std::string& error() const;

  // Called on the loop's thread, in the order the radio produced them.
  void on_advert(Scanner::AdvertHandler h);
  void on_camera(Scanner::SightingHandler h);
  void on_state(Scanner::StateHandler h);

  // What to hand make_ble_scanner. Safe to call from any thread, and safe to
  // outlive this object: a sink invoked after the bridge is gone drops what it
  // was given rather than writing to freed memory. That is not a licence to
  // arrange it -- stop the scanner first -- but a scanner being torn down on
  // its own queue is not something a caller can order precisely.
  Scanner::AdvertHandler advert_sink();
  Scanner::SightingHandler camera_sink();
  Scanner::StateHandler state_sink();

  // Deliver everything queued, on the calling thread.
  //
  // The loop calls this when the pipe says there is something. It is public
  // for the program that has not been converted to the loop yet and drains at
  // the top of its own iteration instead.
  void drain();

  // How many events were discarded because the queue was full. Monotonic.
  size_t dropped() const;
  // How many are waiting right now.
  size_t queued() const;

  // The most a queue is allowed to hold before the oldest is discarded. About
  // a minute of a busy room.
  static constexpr size_t kMaxQueued = 4096;

 private:
  struct Shared;

  std::shared_ptr<Shared> shared_;
  Loop* loop_ = nullptr;
  SourceId source_ = kNoSource;
  std::string error_;

  // Only ever touched on the loop's thread, so they are not in Shared: putting
  // them there would mean holding the queue's lock across a call into the
  // daemon, which is the one thing a lock in this program must never do.
  Scanner::AdvertHandler on_advert_;
  Scanner::SightingHandler on_camera_;
  Scanner::StateHandler on_state_;
};

}  // namespace octo

#endif  // OCTO_SCANBRIDGE_H
