// One message language, spoken over three different pipes.
//
// A sync daemon is reachable three ways -- a unix socket when it is a process
// on this Mac, a USB CDC serial port when it is a Nordic on the end of a
// cable, and a BLE characteristic when that Nordic is powered from something
// else. Those differ in framing and in how much fits in one write, and in
// nothing else that matters. So the message layer is one piece of portable
// code and the transports are thin.
//
// The token layer is src/proto.h's, unchanged and already tested: escape and
// unescape, split on whitespace, then on the first '='. That format was
// chosen because every consumer was a C++ program that would otherwise need a
// JSON parser, and the same argument holds harder on a microcontroller. It
// also survives being read by a person, which is the entire reason USB comes
// before Bluetooth -- a terminal and a cable should be enough to see what the
// box is doing.
//
// What is new here is the framing and the shape of a message. proto.h renders
// a whole snapshot as a block of lines terminated by `end`, which suits a
// request-and-answer socket and does not suit a link that also carries
// unsolicited announcements. Here every message is exactly one line and is
// independent of every other, so a Tentacle sighting arriving in the middle of
// a settings push is not a parse error.
//
// Unknown keys must be ignored and unknown verbs must be answered rather than
// dropped. A Mac and a box will be running different versions almost all of
// the time, and the failure mode to design out is the one where a new field
// makes an old reader give up on the whole line.
#ifndef OCTO_BOXMSG_H
#define OCTO_BOXMSG_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace octo {

// The version of this message layer. Sent in the greeting so each end knows
// what the other can be told. Distinct from proto.h's kProtocolVersion, which
// versions a different format on a different socket.
inline constexpr int kBoxProtocolVersion = 1;

// A message is a verb and an ordered list of fields. Ordered because it is
// meant to be read by a person on a serial console, and `dev id=... rssi=...`
// reads better in a fixed order than whatever a map iterates in. Repeats are
// allowed: a field that can occur many times is how a list travels without
// inventing a nested syntax.
struct Message {
  std::string verb;
  std::vector<std::pair<std::string, std::string>> fields;

  bool has(const std::string& key) const;

  // First value for `key`, or `fallback`. First rather than last because a
  // repeated key is a list, and the reader that wants one of them wants the
  // one that was sent first.
  std::string get(const std::string& key,
                  const std::string& fallback = std::string()) const;

  // Parsed accessors. Each returns false and leaves *out alone when the key is
  // missing or the value does not parse, so a caller can tell "absent" from
  // "present and zero" -- which matters for a passkey, where 000000 is a value
  // a camera might really be showing.
  bool get_int(const std::string& key, int64_t* out) const;
  bool get_double(const std::string& key, double* out) const;
  bool get_bool(const std::string& key, bool* out) const;

  std::vector<std::string> all(const std::string& key) const;

  void set(const std::string& key, const std::string& value);
  void set_int(const std::string& key, int64_t value);
  void set_double(const std::string& key, double value, int digits = 4);
  void set_bool(const std::string& key, bool value);

  // Append without replacing, for the list case.
  void add(const std::string& key, const std::string& value);
};

// One line, without the terminator. Never contains a newline: values are
// escaped, and the verb is rejected at encode time if it is not a bare token.
std::string encode(const Message& msg);

// Parse one line. A trailing '\r' is tolerated, because a person typing into a
// terminal emulator over a USB serial port will send one and should not have
// to know that. Returns false and sets *err for an empty line or a malformed
// token; an unknown verb is NOT an error here -- that is a decision for the
// layer that dispatches, which is the only one that knows what it supports.
bool decode(const std::string& line, Message* out, std::string* err);

// Turns a byte stream into whole lines.
//
// This exists because none of the three transports delivers a message: a
// serial port delivers however many bytes the driver had, a socket the same,
// and a GATT write delivers at most one MTU. All three need the same
// accumulate-until-newline, and writing it once is the difference between one
// tested buffer and three subtly different ones.
class LineReader {
 public:
  // A line longer than this is discarded, along with everything up to the next
  // newline, and reported once. Without a cap, a peer that never sends a
  // newline is an unbounded allocation on a device with 256 KB of RAM.
  static constexpr size_t kMaxLine = 4096;

  // Feed arbitrary bytes. Appends every complete line found to `out`, without
  // terminators. Returns false if anything was discarded for length, so a
  // caller can say so rather than silently losing a command.
  bool feed(const char* data, size_t len, std::vector<std::string>* out);
  bool feed(const std::string& data, std::vector<std::string>* out);

  // Bytes held pending a newline. For a caller that wants to notice a peer
  // that has gone quiet mid-message.
  size_t buffered() const { return buf_.size(); }
  void reset();

 private:
  std::string buf_;
  bool dropping_ = false;
};

}  // namespace octo

#endif  // OCTO_BOXMSG_H
