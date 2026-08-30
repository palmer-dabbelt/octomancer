// The box protocol over a byte pipe -- in practice, a dongle on a USB cable.
//
// The third transport for src/boxmsg.h's message language, and the one that
// makes doc/box-notes.md's two-radio experiment possible: a Mac holds one of
// these open to an nRF52840 running the same sync daemon as firmware, and asks
// it what it can hear. Two radios, two rosters, one protocol.
//
// src/boxsock.h is the same job over a unix socket and is the *server* side of
// it; this is a client, because the box is the thing with the roster and the
// Mac is the thing asking. They share nothing but boxmsg.h, which is the point
// of boxmsg.h.
//
// Framing is one message per line, exactly as on the socket. What differs is
// everything underneath: a serial port has no connection to accept, no EOF
// worth the name, and a habit of coming back with a different device node
// after the dongle reboots. So "closed" here means the port broke or the
// caller said stop, and reopening is the caller's business -- see
// src/hciport.h, which finds the device.
//
// Nothing here parses a message beyond splitting lines, and nothing here knows
// what a Tentacle is. That belongs above.
#ifndef OCTO_BOXCDC_H
#define OCTO_BOXCDC_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "boxmsg.h"
#include "hciport.h"
#include "loop.h"
#include "syncd.h"

namespace octo {

class BoxLink : public MsgPeer {
 public:
  using LineHandler = std::function<void(const std::string& line)>;
  using MessageHandler = std::function<void(const Message& msg)>;
  // Why the link ended, in a form fit to show a person. Called at most once.
  using ClosedHandler = std::function<void(const std::string& why)>;

  // Takes the port. Reading starts immediately: a box announces without being
  // asked -- that is the whole difference between this protocol and
  // src/proto.h -- so a link that only read after a question would miss the
  // greeting that says what it is talking to.
  static std::unique_ptr<BoxLink> attach(Loop* loop,
                                         std::unique_ptr<hci::Port> port);
  ~BoxLink() override;

  BoxLink(const BoxLink&) = delete;
  BoxLink& operator=(const BoxLink&) = delete;

  // MsgPeer. Adds the terminator, because framing is this file's job.
  void send(const std::string& line) override;
  void send(const Message& msg);

  void on_line(LineHandler handler);
  void on_message(MessageHandler handler);
  void on_closed(ClosedHandler handler);

  void close(const std::string& why);
  bool is_open() const;
  std::string name() const;

  // A line longer than the reader will hold, which is a box that has gone
  // wrong or a wire that has. Counted rather than reported per occurrence: it
  // arrives in floods when it arrives at all.
  uint64_t long_lines() const { return long_lines_; }
  // Lines that arrived and did not decode. Distinct from long_lines: this is
  // a well-formed line that is not a well-formed message, which is a version
  // skew or a corrupted byte rather than a framing failure.
  uint64_t bad_lines() const { return bad_lines_; }

 private:
  BoxLink() = default;
  void on_readable();
  void on_port_error(const std::string& why);

  Loop* loop_ = nullptr;
  std::unique_ptr<hci::Port> port_;
  SourceId source_ = kNoSource;
  LineReader reader_;

  LineHandler on_line_;
  MessageHandler on_message_;
  ClosedHandler on_closed_;

  bool closed_ = false;
  uint64_t long_lines_ = 0;
  uint64_t bad_lines_ = 0;
  // Set while a handler is running, so that a handler which closes the link --
  // the ordinary response to hearing something alarming -- does not free the
  // object out from under the loop that is still walking it.
  std::shared_ptr<bool> alive_;
};

}  // namespace octo

#endif  // OCTO_BOXCDC_H
