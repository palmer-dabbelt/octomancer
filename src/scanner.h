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

class Scanner {
 public:
  // Both callbacks arrive on the scanner's own queue, not the caller's thread.
  using AdvertHandler = std::function<void(const Advert&)>;
  using StateHandler = std::function<void(const std::string&)>;

  virtual ~Scanner() = default;
  virtual bool start(std::string* err) = 0;
  virtual void stop() = 0;
};

// Returns nullptr on a host with no CoreBluetooth.
std::unique_ptr<Scanner> make_ble_scanner(Scanner::AdvertHandler on_advert,
                                          Scanner::StateHandler on_state);

}  // namespace octo

#endif  // OCTO_SCANNER_H
