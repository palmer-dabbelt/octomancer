// The camera half of the radio: connect, subscribe, write.
//
// This is the one place in octomancer that is not passive. octomancerd never
// connects to anything and never writes to anything, on purpose -- a service
// that runs unattended should not also be able to act unattended. Setting a
// clock is an action, so it lives in a separate program with a separate
// binary, and this header is the seam between it and CoreBluetooth.
//
// Everything here blocks. The daemon above it is a plain loop -- listen, look,
// decide, maybe write, sleep -- and a callback-shaped API would turn that into
// a state machine for no gain. CoreBluetooth's callbacks arrive on a private
// dispatch queue and are turned back into return values behind this interface.
//
// That is now true of CoreBluetooth only. The dongle used to implement this as
// well, and cannot any more: blocking here means waiting for a reader thread,
// and the box has no threads to wait for -- see src/loop.h for why that is a
// property of the toolchain rather than a preference. The dongle's camera half
// is octo::HciCamera in src/camhci.h, which is the same logic with completion
// handlers instead of return values. This interface stays as it is for as long
// as CoreBluetooth is behind it and the tools above it are written as loops.
#ifndef OCTO_CAMERA_H
#define OCTO_CAMERA_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bmd.h"

namespace octo {

struct CameraDevice {
  std::string id;    // stable per host: CoreBluetooth gives a UUID, not a MAC
  std::string name;
  int rssi = 0;
  // A service-UUID match is proof of what a device is; a name match is only a
  // guess. Tentacle boxes are named after the camera they ride on, so a box
  // advertising as "BMPCC" will happily answer a name guess and then fail to
  // produce a control characteristic for the rest of the night.
  bool by_service_uuid = false;
};

struct ScanResult {
  std::vector<CameraDevice> cameras;  // proof first, then by signal strength
  std::vector<CameraDevice> all;      // everything seen, for --scan-only --all
  int total = 0;                      // how many LE devices answered at all
  int tentacles = 0;
};

// What the camera has told us over the current connection.
struct CameraView {
  bool has_timecode = false;
  bmd::Timecode timecode;
  double timecode_mono = 0.0;

  bool has_transport = false;
  int64_t transport = 0;

  bool has_fps = false;
  int fps = 0;

  // 4.7, which decides whether the timecode follows the RTC at all. Pulled out
  // of `state` rather than left in it because it gates writing: a camera that
  // has not said which mode it is in is a different thing from one that has
  // said it is in the mode we cannot help, and a map lookup returning nothing
  // blurs the two.
  bool has_timecode_source = false;
  int64_t timecode_source = 0;

  // Every (group, parameter) the camera has volunteered, for the probe modes.
  std::map<std::pair<int, int>, bmd::Value> state;
};

// Called as each camera is first identified, so that a twenty-second scan can
// say what it has found while it is still looking. A scan is mostly waiting,
// and when the answer is "there it is" the waiting was the expensive part of
// finding out. May be empty, and is never called for a Tentacle.
using CameraSeen = std::function<void(const CameraDevice&)>;

class CameraLink {
 public:
  virtual ~CameraLink() = default;

  // Wait for the radio to come up. Returns false with a human-readable reason
  // when Bluetooth is off, or when this binary has not been granted access --
  // which on macOS otherwise presents as a scan that simply never finds
  // anything, rather than as an error.
  virtual bool ready(double timeout, std::string* err) = 0;

  virtual ScanResult scan(double seconds, const std::string& name_hint,
                          bool want_all, const CameraSeen& on_camera) = 0;

  virtual bool connect(const std::string& id, double timeout,
                       std::string* err) = 0;
  virtual void disconnect() = 0;
  virtual bool connected() const = 0;

  // Subscribe to the Timecode and Incoming Control characteristics. Once per
  // connection: subscribing twice is an error, so the view keeps updating
  // itself and the verification pass after a write samples the same object.
  virtual bool subscribe(double timeout, std::string* err) = 0;

  // Whether the current connection has already been subscribed. Subscribing
  // twice is an error, so a caller that holds a connection across several
  // cycles needs to be able to ask rather than remember -- the link can be
  // dropped by the camera at any moment, and then the caller's memory is
  // wrong in the direction that silently produces no timecode.
  virtual bool subscribed() const = 0;

  // Read the Camera Status characteristic -- the only readable one in the
  // profile, per doc/protocol-notes.md. Reading is also the only operation
  // here that makes macOS negotiate encryption: subscribing to a notify
  // characteristic does not, which is why nothing ever asked for a passkey and
  // no bond was ever attempted. doc/pairing-notes.md has the rest of that.
  //
  // The value is a bitfield -- 0x01 power on, 0x02 connected, 0x04 paired --
  // so it answers "does the camera think it is paired" directly rather than by
  // inference.
  virtual bool read_status(std::vector<uint8_t>* out, double timeout,
                           std::string* err) = 0;

  virtual bool write_control(const std::vector<uint8_t>& packet, double timeout,
                             std::string* err) = 0;

  virtual CameraView view() = 0;

  // Drop the timecode so the next wait observes a fresh reading rather than
  // the one that arrived before a write landed.
  virtual void forget_timecode() = 0;

  // Wait until the camera has reported both a timecode and its transport mode,
  // or the deadline passes. Returns the view either way.
  virtual CameraView await_state(double seconds) = 0;
};

// Returns nullptr on a host with no CoreBluetooth.
std::unique_ptr<CameraLink> make_camera_link();

}  // namespace octo

#endif  // OCTO_CAMERA_H
