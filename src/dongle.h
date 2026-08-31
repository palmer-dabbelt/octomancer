// A second radio, on the end of a USB cable, seen from the control daemon.
//
// octomancerd listens with the Mac's own Bluetooth. A dongle running the same
// sync daemon as firmware listens with its own, from wherever it happens to
// be plugged in, and says what it hears over the box protocol
// (src/boxmsg.h). This turns that conversation into rows octomancerd can put
// in a snapshot beside its own.
//
// **The rows are never merged with ours, and that is forced rather than
// chosen.** The two radios cannot agree on what to call a box. CoreBluetooth
// hands out a per-host UUID and will not show the hardware address; the
// dongle sees the address and nothing else. Nor is there anything in the
// advertisement to match on: a Tentacle broadcasts a clock and no serial
// number (src/tentacle.h). Anything that claimed two rows were the same box
// would be guessing, and a confident wrong answer here means somebody
// re-jamming a box on the strength of a reading from a different one.
//
// So a box in earshot of both appears twice, tagged with which radio heard
// it. That is also the more useful thing to look at: the two rows should
// agree about how far the box is from its own bench, and noticing that they
// do not is the entire reason for having a second radio.
//
// **Nothing here talks to a cable.** It is fed Messages and hands back rows,
// so tests/test_dongle.cc drives a whole dongle -- attaching, answering,
// going quiet, being unplugged mid-sentence -- with no hardware in the
// building. src/boxcdc.h is the transport, and octomancerd is the glue.
#ifndef OCTO_DONGLE_H
#define OCTO_DONGLE_H

#include <string>
#include <vector>

#include "boxmsg.h"
#include "registry.h"

namespace octo {

class DongleView {
 public:
  // How often to ask what it can hear. A dongle answers a `devices` request
  // out of a roster it already has, so this costs a few hundred bytes down a
  // USB pipe and no radio time at all; the limit on how fast it is worth
  // asking is how fast a person can read the answer.
  static constexpr double kPollEvery = 5.0;

  // How long a set of rows may stand after the batch that produced it. Three
  // missed polls: enough that one slow answer does not blank the page,
  // little enough that a dongle which has stopped answering stops being
  // quoted before anybody acts on it.
  //
  // Rows are dropped entirely rather than aged, because ageing them would be
  // a claim about when the *box* was last heard, and what has actually gone
  // quiet is the dongle. Those want opposite reactions and must not render
  // the same way.
  static constexpr double kStaleAfter = 3.0 * kPollEvery;

  explicit DongleView(std::string radio = "dongle");

  // --- what the link does -------------------------------------------------

  void opened(double now_mono);
  // Whatever the reason, this forgets everything the dongle said. A dongle
  // that is not there is not evidence, and the last thing it said about a box
  // ages into a lie without anything to correct it.
  void closed(double now_mono);
  bool attached() const { return attached_; }

  // One message from the box. Unknown verbs are ignored rather than refused:
  // a Mac and a dongle will be running different versions most of the time,
  // and the failure to design out is the one where a new field makes an old
  // reader give up on the whole conversation.
  void observe(const Message& msg, double now_mono);

  // Whether the thing on the other end has identified itself as a sync
  // daemon. Nothing is sent to it until it has.
  //
  // This matters more than it looks. A dongle is found by opening serial
  // devices -- on macOS, whatever matches /dev/cu.usbmodem* -- and that list
  // includes anybody else's microcontroller. Writing `devices` at a stranger
  // is at best rude and at worst a command in some other protocol, so the
  // greeting is what turns an open port into a dongle. A port that never
  // greets is let go rather than held.
  bool greeted() const { return greeted_; }

  // --- what the daemon asks -----------------------------------------------

  // True when it is time to ask again, filling *out with the request. Asking
  // is the caller's job because only the caller can fail to send it.
  bool wants_poll(double now_mono, Message* out);
  // Call when the request in wants_poll() actually went out. Separated so a
  // write that failed does not start a poll interval that never ends.
  void polled(double now_mono);

  // The rows, ready to append to a snapshot. Empty when there is nothing
  // current to say -- not attached, nothing heard yet, or the last complete
  // answer is older than kStaleAfter.
  std::vector<DeviceSnapshot> devices(double now_mono) const;

  const std::string& radio() const { return radio_; }

  // Whether the box's offsets can be compared to real time, or only to each
  // other. False on a dongle nobody has told the time -- which is the normal
  // standalone case, not a fault. Rows are quoted against the dongle's own
  // median precisely so that this does not matter to a reader; it is reported
  // because anything wanting an absolute figure has to ask first.
  bool clock_is_real() const { return clock_real_; }

  // The last thing the box said about itself in words. This is how a box with
  // no console reports that its watchdog fired, so it is kept rather than
  // logged and dropped.
  const std::string& last_words() const { return said_; }

  // Complete answers received. A dongle that greets us and then never
  // finishes a batch is a different problem from one that is not there, and
  // this is what tells them apart.
  uint64_t answers() const { return answers_; }

 private:
  std::string radio_;
  bool attached_ = false;
  bool greeted_ = false;
  bool clock_real_ = false;
  std::string said_;
  uint64_t answers_ = 0;

  // The batch being assembled, and the last one that finished. A half-arrived
  // answer is never shown: a `dev` list is a complete statement about the
  // room, so publishing it partway through would report every box that had
  // not been mentioned yet as having gone away.
  std::vector<DeviceSnapshot> pending_;
  std::vector<DeviceSnapshot> current_;
  bool in_batch_ = false;
  double current_mono_ = 0.0;
  double asked_mono_ = 0.0;
  bool ever_asked_ = false;
};

// One `dev` line as a row. Exposed for the tests, and because the mapping
// from the wire to a snapshot is the part most likely to want checking
// against a real box.
DeviceSnapshot device_from_message(const Message& msg,
                                   const std::string& radio);

}  // namespace octo

#endif  // OCTO_DONGLE_H
