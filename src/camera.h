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
#ifndef OCTO_CAMERA_H
#define OCTO_CAMERA_H

#include <cstdint>
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

  // Every (group, parameter) the camera has volunteered, for the probe modes.
  std::map<std::pair<int, int>, bmd::Value> state;
};

class CameraLink {
 public:
  virtual ~CameraLink() = default;

  // Wait for the radio to come up. Returns false with a human-readable reason
  // when Bluetooth is off, or when this binary has not been granted access --
  // which on macOS otherwise presents as a scan that simply never finds
  // anything, rather than as an error.
  virtual bool ready(double timeout, std::string* err) = 0;

  virtual ScanResult scan(double seconds, const std::string& name_hint,
                          bool want_all) = 0;

  virtual bool connect(const std::string& id, double timeout,
                       std::string* err) = 0;
  virtual void disconnect() = 0;
  virtual bool connected() const = 0;

  // Subscribe to the Timecode and Incoming Control characteristics. Once per
  // connection: subscribing twice is an error, so the view keeps updating
  // itself and the verification pass after a write samples the same object.
  virtual bool subscribe(double timeout, std::string* err) = 0;

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
