// The camera, asynchronously: what the sync daemon needs and nothing else.
//
// There are two camera backends in this tree and they are not the same shape.
// src/camera.h's CameraLink blocks by contract, which suits a tool written as
// a loop of statements and cannot be implemented over the dongle at all --
// blocking there means waiting for a reader thread, and the box has no threads
// to wait for. src/camhci.h's HciCamera is the same logic with completions
// instead of return values, and it is the one the box will run.
//
// This header is the second of those two written down as an interface, so that
// the daemon above it depends on the shape rather than on the implementation.
// That buys one thing worth the file: a fake camera in a test can answer with
// canned bytes on a clock that is a variable, and a whole sync cycle -- an
// hour of drift, a write that misses, a camera that stops answering halfway
// through -- runs in no wall-clock time with no radio present. It is the same
// argument src/hciport.h makes for the HCI port, and doc/box-notes.md records
// what happened when that promise went unkept for four years.
//
// Two rules an implementation must hold to, both of which the daemon relies on
// and neither of which the compiler checks:
//
//   * **A completion never runs inside the call that registered it.** An
//     immediate failure -- not connected, malformed argument, no link -- is
//     posted to the loop like any other answer. Without this, `connect()`
//     failing early calls its completion while `connect()`'s own frame is
//     still live, and that completion is entitled to call `connect()` again.
//
//   * **Every completion runs on the loop's thread**, so nothing a handler
//     touches needs a lock.
#ifndef OCTO_CAMASYNC_H
#define OCTO_CAMASYNC_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "camera.h"

namespace octo {

class AsyncCamera {
 public:
  using DoneHandler = std::function<void(bool ok, const std::string& err)>;
  using ScanHandler = std::function<void(const ScanResult& result)>;
  // Called every time the camera volunteers something: a timecode, a transport
  // mode, a frame rate. This is what replaces waiting for a state to arrive --
  // there is nobody to block, so the camera says what it knows as it learns
  // it and the caller decides when it has enough.
  using ViewHandler = std::function<void(const CameraView& view)>;

  virtual ~AsyncCamera() = default;

  virtual void set_view_handler(ViewHandler on_change) = 0;
  // Called when the camera goes away by itself, which it does whenever it is
  // switched off or walks out of range mid-cycle.
  virtual void set_disconnect_handler(std::function<void()> on_gone) = 0;

  virtual void scan(double seconds, const std::string& name_hint, bool want_all,
                    ScanHandler done) = 0;

  // `done` means "ready to be used", not "the radio link is up": connecting
  // includes negotiating an MTU and discovering the control service.
  virtual void connect(const std::string& id, double timeout,
                       DoneHandler done) = 0;
  virtual void disconnect() = 0;
  virtual bool connected() const = 0;

  // Once per connection; subscribing twice is an error. This is also where
  // pairing happens, because the characteristics are encrypted and the
  // subscription is what first demands an encrypted link.
  virtual void subscribe(double timeout, DoneHandler done) = 0;
  virtual bool subscribed() const = 0;

  virtual void write_control(const std::vector<uint8_t>& packet, double timeout,
                             DoneHandler done) = 0;

  virtual const CameraView& view() const = 0;
  // Drop the timecode so the next reading observed is a fresh one rather than
  // the one that arrived before a write landed.
  virtual void forget_timecode() = 0;
};

}  // namespace octo

#endif  // OCTO_CAMASYNC_H
