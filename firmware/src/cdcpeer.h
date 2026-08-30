// The box protocol over a USB CDC serial port, on the loop.
//
// The second of the three transports src/boxsock.h names -- "a unix socket
// when the sync daemon is a process on this Mac, a USB CDC serial port when
// it is a Nordic on the end of a cable, and a BLE characteristic when that
// Nordic is powered from something else". They differ in framing and in how
// much fits in one write, and in nothing else, so this file moves whole lines
// and parses none of them.
//
// Two things here are not in the socket version, and both come from a serial
// port being a wire rather than a connection.
//
// A wire has no accept() and no close(), so there is nothing to tell the
// daemon a peer arrived. DTR is the nearest true thing: a host that has
// opened the port asserts it and a host that has gone away drops it, which is
// exactly the peer_opened/peer_closed pair the daemon wants. Without that the
// greeting would go out once at boot, into a cable nobody was holding, and
// every later host would attach to a daemon that had already introduced
// itself.
//
// And a wire cannot push back. A socket write blocks or reports EAGAIN; bytes
// handed to a USB endpoint that nothing is draining simply accumulate until
// the buffer is full. So the transmit side is a ring buffer that drops, and
// counts what it dropped -- an announcement is a perishable statement about
// the present, and the alternative on a device with 256 KB of RAM is to grow
// until it dies. Silence about the discards would not be acceptable, which is
// what dropped_tx() is for.
#ifndef OCTO_FW_CDCPEER_H
#define OCTO_FW_CDCPEER_H

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "boxmsg.h"
#include "loop.h"
#include "syncd.h"

namespace octo {

class CdcPeer : public MsgPeer {
 public:
  // `loop` and `uart` outlive this object.
  CdcPeer(Loop* loop, const struct device* uart);
  ~CdcPeer();

  CdcPeer(const CdcPeer&) = delete;
  CdcPeer& operator=(const CdcPeer&) = delete;

  using LineHandler = std::function<void(const std::string& line)>;
  using OpenHandler = std::function<void()>;
  using CloseHandler = std::function<void()>;

  // Called on the loop's thread, never from the interrupt.
  void on_line(LineHandler h) { on_line_ = std::move(h); }
  void on_open(OpenHandler h) { on_open_ = std::move(h); }
  void on_close(CloseHandler h) { on_close_ = std::move(h); }

  bool start(std::string* err);
  void stop();

  // MsgPeer. Appends the newline; the caller supplies a line without one.
  void send(const std::string& line) override;

  bool attached() const { return attached_; }

  // What did not fit. Both are cumulative and neither is ever reset, because
  // the question a person asks is "has this ever happened", not "is it
  // happening right now".
  // Provoke the transmit path, and count how often it actually runs.
  //
  // Zephyr's CDC ACM does not call our handler from an interrupt -- it calls
  // it from a workqueue -- so "the loop is going round" says nothing about
  // whether the wire is. The two can be lost separately, and losing only the
  // wire is what leaves a dongle enumerated, blinking, and impossible to open.
  //
  // A thread that is idle by design cannot be watched by asking whether it has
  // run lately, because the honest answer is no. So ask it to: enabling the
  // transmit interrupt makes the driver queue our handler whether or not there
  // is anything to send, and the handler bumps this counter on its way past. A
  // counter that does not move after a poke is a workqueue that cannot run.
  // See src/watchdog.h's ProbeLiveness, which is the judgement half.
  void probe_wire();
  uint32_t wire_ticks() const { return wire_ticks_; }

  uint32_t dropped_tx() const { return dropped_tx_; }
  uint32_t dropped_rx() const { return dropped_rx_; }
  // Lines discarded for exceeding LineReader::kMaxLine.
  uint32_t long_lines() const { return long_lines_; }

 private:
  static void isr_trampoline(const struct device* dev, void* user);
  void isr();
  void drain();        // loop thread: ring buffer -> LineReader -> on_line_
  void poll_control();  // loop thread: has DTR changed?

  Loop* loop_ = nullptr;
  const struct device* uart_ = nullptr;

  // Raised by the receive interrupt, waited on by the loop. See
  // firmware/src/loop_zephyr.cc: this is the only thing an interrupt is
  // allowed to do to the loop.
  struct k_poll_signal rx_signal_;
  SourceId source_ = kNoSource;
  TimerId control_timer_ = kNoTimer;

  struct ring_buf rx_;
  struct ring_buf tx_;
  uint8_t rx_storage_[512];
  uint8_t tx_storage_[4096];

  LineReader reader_;
  LineHandler on_line_;
  OpenHandler on_open_;
  CloseHandler on_close_;

  bool started_ = false;
  bool attached_ = false;

  // Written by the interrupt, read by the loop. Aligned 32-bit words on a
  // Cortex-M, so a torn read is not possible and a lock would buy nothing.
  // Written from the workqueue, read from the loop. Nothing needs its exact
  // value, only whether it changed, so a plain counter is enough.
  volatile uint32_t wire_ticks_ = 0;
  volatile uint32_t dropped_tx_ = 0;
  volatile uint32_t dropped_rx_ = 0;
  uint32_t long_lines_ = 0;
};

}  // namespace octo

#endif  // OCTO_FW_CDCPEER_H
