// The radio, behind an interface thin enough to keep CoreBluetooth out of the
// rest of the program.
//
// Everything above this line is portable C++ that can be tested on any host;
// everything below it is Objective-C++ that can only run on a Mac with a real
// antenna. Keeping the seam here is what lets the decoder and the drift
// arithmetic be tested at all.
#ifndef OCTO_SCANNER_H
#define OCTO_SCANNER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace octo {

struct Advert {
  std::string id;    // stable per host: CoreBluetooth gives a UUID, not a MAC
  std::string name;
  int rssi = 0;
  std::vector<uint8_t> data;  // the FDAC service-data payload, verbatim
  double mono = 0.0;
  double wall = 0.0;
};

// A device advertising the Blackmagic camera-control service.
//
// Nothing is decoded here, and nothing can be: unlike a Tentacle, a camera
// puts no clock in its advertisement, so the only fact available from a
// distance is that it is on the air. That happens to be exactly the question
// worth answering cheaply -- a camera that is switched off cannot be synced,
// and finding that out by connecting costs a twenty-second scan.
//
// A camera stops advertising while something holds a connection to it, so
// absence means "not advertising", not "switched off". The caller is the one
// that knows whether the connection is its own; see Presence in
// octomancer-sync.cc.
struct Sighting {
  std::string id;
  std::string name;
  int rssi = 0;
  double mono = 0.0;
  double wall = 0.0;
};

class Scanner {
 public:
  // All callbacks arrive on the scanner's own queue, not the caller's thread.
  using AdvertHandler = std::function<void(const Advert&)>;
  using SightingHandler = std::function<void(const Sighting&)>;
  using StateHandler = std::function<void(const std::string&)>;

  virtual ~Scanner() = default;
  virtual bool start(std::string* err) = 0;
  virtual void stop() = 0;
};

// Returns nullptr on a host with no CoreBluetooth.
//
// `on_camera` may be empty, which is what a caller listening only for the
// bench wants; the scan itself is unfiltered either way, so watching for a
// camera costs no extra radio time.
std::unique_ptr<Scanner> make_ble_scanner(Scanner::AdvertHandler on_advert,
                                          Scanner::SightingHandler on_camera,
                                          Scanner::StateHandler on_state);

inline std::unique_ptr<Scanner> make_ble_scanner(
    Scanner::AdvertHandler on_advert, Scanner::StateHandler on_state) {
  return make_ble_scanner(std::move(on_advert), Scanner::SightingHandler(),
                          std::move(on_state));
}

}  // namespace octo

#endif  // OCTO_SCANNER_H
