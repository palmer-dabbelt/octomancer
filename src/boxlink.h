// One way of talking to a sync daemon, whichever pipe it is on.
//
// A dongle in a USB port is reachable down the cable; the same dongle in a
// phone charger is reachable only over the air. Both carry the box protocol,
// one line per message, and the code that decides what to do with those lines
// -- src/dongle.h -- should not be able to tell which it is holding.
//
// So this is the seam. src/boxcdc.h is the cable, src/boxble.h is the radio,
// and src/octomancerd.cc holds one of these without caring which.
//
// **Both are glue, and neither is tested.** What is tested is what happens to
// the messages once they arrive (tests/test_dongle.cc) and which transport
// should be carrying them (want_bluetooth and carrier, also in src/dongle.h).
// That is the seam CLAUDE.md asks for: the decisions on the testable side, the
// file descriptors and the CoreBluetooth callbacks on the other.
#ifndef OCTO_BOXLINK_H
#define OCTO_BOXLINK_H

#include <functional>
#include <string>

#include "boxmsg.h"
#include "syncd.h"

namespace octo {

class BoxTransport : public MsgPeer {
 public:
  using LineHandler = std::function<void(const std::string& line)>;
  using MessageHandler = std::function<void(const Message& msg)>;
  // Why the link ended, in a form fit to show a person. Called at most once.
  using ClosedHandler = std::function<void(const std::string& why)>;

  ~BoxTransport() override = default;

  // MsgPeer::send(const std::string&) adds the terminator; framing belongs to
  // the transport because only it knows what a frame costs.
  virtual void send(const Message& msg) = 0;

  virtual void on_line(LineHandler handler) = 0;
  virtual void on_message(MessageHandler handler) = 0;
  virtual void on_closed(ClosedHandler handler) = 0;

  virtual void close(const std::string& why) = 0;
  virtual bool is_open() const = 0;
  // What to call this link in a sentence a person reads: a device path, or a
  // peripheral's name. Never an identifier only a program could love.
  virtual std::string name() const = 0;

  // Whether the far end has been reached, as opposed to merely being looked
  // for. A cable is connected the moment it opens; a radio link spends time
  // scanning, connecting and subscribing first, and during all of it there is
  // nothing to talk to. A caller that treated "attaching" as "attached" would
  // write into a connection that does not exist yet.
  virtual bool ready() const { return is_open(); }

  // Give the transport the loop's thread for a moment.
  //
  // The cable does not need this -- the loop watches its descriptor and calls
  // back -- but CoreBluetooth delivers on a queue of its own, and the only
  // safe thing another thread may do is leave bytes somewhere. This is where
  // they are picked up. Called often; must be cheap when there is nothing.
  virtual void pump() {}

  virtual uint64_t long_lines() const { return 0; }
  virtual uint64_t bad_lines() const { return 0; }
};

}  // namespace octo

#endif  // OCTO_BOXLINK_H
