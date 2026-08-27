// The wire to the dongle: a USB CDC serial port carrying H4-framed HCI.
//
// Deliberately the dumbest layer in the stack. It moves bytes and finds
// devices; it knows nothing about packets, which is what lets hcilink.cc be
// tested against a pipe or a canned capture instead of hardware.
#ifndef OCTO_HCIPORT_H
#define OCTO_HCIPORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace octo {
namespace hci {

// A byte pipe. Virtual so a test can substitute one without a dongle -- the
// HCI host above is a state machine, and a state machine that can only be
// exercised through real hardware is one that gets exercised rarely.
class Port {
 public:
  virtual ~Port() = default;

  // Blocks up to `timeout` seconds. Returns the number of bytes read, 0 on
  // timeout, and -1 on a closed or broken port -- the last of which is what
  // unplugging the dongle looks like, and has to be distinguishable from a
  // quiet radio.
  virtual int read(uint8_t* buf, size_t len, double timeout) = 0;

  // Writes everything or fails. A partial HCI command on the wire desyncs the
  // controller for good, so there is no partial-write success here.
  virtual bool write(const uint8_t* data, size_t len) = 0;

  virtual void close() = 0;
  virtual bool is_open() const = 0;
  virtual std::string name() const = 0;
};

// Every serial device that could plausibly be a dongle, best guess first.
//
// On macOS these are /dev/cu.usbmodem*; the /dev/tty.* twin of each is the
// one that blocks on open waiting for carrier detect, which is a hang with no
// error message and no output, so it is never returned here.
std::vector<std::string> list_candidate_ports();

// Opens `device`, or the first candidate when `device` is empty.
//
// A named device that cannot be opened is an error. No device at all is also
// an error, but a distinguishable one: callers use it to fall back to
// CoreBluetooth rather than to fail, which is what keeps the dongle optional.
std::unique_ptr<Port> open_port(const std::string& device, std::string* err);

// True when the failure from open_port was "there is no dongle here", rather
// than "the dongle is here and would not open". Only the first is a reason to
// quietly use the other radio.
bool no_port_found(const std::string& err);

}  // namespace hci
}  // namespace octo

#endif  // OCTO_HCIPORT_H
