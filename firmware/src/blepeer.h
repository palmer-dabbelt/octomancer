// The box protocol over a BLE characteristic, on the loop.
//
// The third of the three transports src/boxsock.h names: "a unix socket when
// the sync daemon is a process on this Mac, a USB CDC serial port when it is
// a Nordic on the end of a cable, and a BLE characteristic when that Nordic is
// powered from something else". This is the last one, and it is what makes a
// dongle in a phone charger a usable member of the system rather than a thing
// that has to stay tethered to a laptop.
//
// It is a peer like any other. src/syncd.h keeps a list of them and announces
// to all of them, so a box with a cable *and* a radio link simply has two, and
// nothing here has to decide which is in charge. That decision belongs to the
// host -- see want_bluetooth() and carrier() in src/dongle.h -- because the
// host is the end that knows whether it also has a cable. A box that tried to
// arbitrate would have to guess at facts only the other end can see.
//
// **The UUIDs are Nordic's UART Service, on purpose.** Not because anything
// here is a UART, but because NUS is the one well-known "bytes in, bytes out"
// GATT service, and every generic Bluetooth tool on every phone can open it
// and type into it. This device has no console and no screen; its failures
// have twice now been invisible for a day. Being openable by software nobody
// had to write first is worth more than a private UUID would buy. What travels
// over it is exactly what travels over the cable: one line per message.
//
// **Nothing here parses.** Bytes arrive on Bluetooth's own thread and are put
// in a ring buffer; a signal wakes the loop, and the loop does the rest. That
// is the same arrangement as firmware/src/cdcpeer.h and for the same reason:
// the registry, the daemon and every handler run on one thread, and the whole
// design in src/loop.h depends on that staying true.
#ifndef OCTO_FW_BLEPEER_H
#define OCTO_FW_BLEPEER_H

#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

struct bt_conn;

#include <cstdint>
#include <functional>
#include <string>

#include "boxmsg.h"
#include "loop.h"
#include "syncd.h"

namespace octo {

class BlePeer : public MsgPeer {
 public:
  explicit BlePeer(Loop* loop);
  ~BlePeer();

  BlePeer(const BlePeer&) = delete;
  BlePeer& operator=(const BlePeer&) = delete;

  using LineHandler = std::function<void(const std::string& line)>;
  using OpenHandler = std::function<void()>;
  using CloseHandler = std::function<void()>;

  // All called on the loop's thread, never from Bluetooth's.
  void on_line(LineHandler h) { on_line_ = std::move(h); }
  void on_open(OpenHandler h) { on_open_ = std::move(h); }
  void on_close(CloseHandler h) { on_close_ = std::move(h); }

  // Begins advertising. bt_enable() must have completed first.
  bool start(std::string* err);
  void stop();

  // MsgPeer. Appends the newline, and splits the result across as many
  // notifications as the negotiated MTU requires -- a GATT notification is not
  // a message boundary any more than a serial read is, which is exactly why
  // LineReader exists at both ends.
  void send(const std::string& line) override;

  // Whether a central is connected and listening. False while advertising:
  // a box nobody has connected to is working correctly.
  bool attached() const { return attached_; }
  // Connections accepted since boot. A central that keeps reconnecting is a
  // different problem from one that never arrives, and this is what tells
  // them apart on a device with no log.
  uint32_t sessions() const { return sessions_; }

  // Notifications that would not fit or would not go. Cumulative and never
  // reset: the question is "has this ever happened", not "is it happening
  // now". A radio link drops where a cable does not, so this one is expected
  // to be nonzero on a busy box and is not by itself a fault.
  uint32_t dropped_tx() const { return dropped_tx_; }
  uint32_t dropped_rx() const { return dropped_rx_; }
  uint32_t long_lines() const { return long_lines_; }

  // Bluetooth's thread calls these. Public only because the GATT and
  // connection callbacks are C functions; nothing else should.
  void on_written(const uint8_t* data, uint16_t len);
  // Takes ownership of a reference; it is released on the loop's thread.
  void on_connected(struct bt_conn* conn);
  void on_disconnected();
  void on_subscribed(bool on);

 private:
  void drain();          // loop thread: ring buffer -> LineReader -> on_line_
  void settle();         // loop thread: act on a connection change
  void advertise();

  Loop* loop_ = nullptr;

  // Raised by Bluetooth's thread, waited on by the loop. See
  // firmware/src/loop_zephyr.cc: this is the only thing another thread is
  // allowed to do to the loop.
  struct k_poll_signal rx_signal_;
  SourceId source_ = kNoSource;

  struct ring_buf rx_;
  // Smaller than the cable's. A central writes a command and waits; nothing
  // streams into this the way a host can stream into a serial port.
  uint8_t rx_storage_[512];

  LineReader reader_;
  LineHandler on_line_;
  OpenHandler on_open_;
  CloseHandler on_close_;

  bool started_ = false;
  bool attached_ = false;
  bool advertising_ = false;

  // Written by Bluetooth's thread, read by the loop. Aligned words on a
  // Cortex-M, so a torn read is not possible and a lock would buy nothing --
  // and could not exist anyway: this SDK's libstdc++ has no std::mutex. See
  // src/loop.h.
  // Held from the connection callback until the loop notices the
  // disconnection, so that a send() in progress cannot be left holding a
  // connection that Bluetooth's thread has already freed.
  struct bt_conn* conn_ = nullptr;
  volatile bool connected_ = false;
  volatile bool notify_on_ = false;
  volatile uint32_t sessions_ = 0;
  volatile uint32_t dropped_tx_ = 0;
  volatile uint32_t dropped_rx_ = 0;
  volatile uint32_t long_lines_ = 0;
};

}  // namespace octo

#endif  // OCTO_FW_BLEPEER_H
