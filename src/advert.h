// What a scanner makes of one advertising report, decided once for every radio.
//
// There are three radios now -- CoreBluetooth on the Mac, a dongle driven over
// HCI from the Mac, and the dongle driving itself as firmware -- and all three
// face the same question about every packet in the air: is this a Tentacle, is
// this a camera, and what is it called. That question is pure byte arithmetic
// over src/hci.h's AD decoder, so it is answered here rather than three times.
//
// It used to live inside src/scanner_hci.cc, which was fine while there was
// one HCI scanner. The firmware scanner in firmware/src/scanner_zephyr.cc is
// the second, and a copied classifier is how a bench comes to disagree with
// itself about what a device is depending on which radio heard it.
//
// The rule worth stating: a camera is identified by the service it advertises
// and never by its name. There is a Tentacle on this bench called "BMPCC", and
// a name match would hand the sync daemon a box with no control
// characteristic on it.
#ifndef OCTO_ADVERT_H
#define OCTO_ADVERT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace octo {

// What one advertising report turned out to be. Both flags false is the
// ordinary case -- most of the air is neither -- and both being true is
// allowed rather than guarded against, because nothing here needs the two
// questions to be exclusive.
struct AdvertMatch {
  bool is_box = false;     // carried FDAC service data
  bool is_camera = false;  // advertised the Blackmagic camera-control service
  std::string name;
  std::vector<uint8_t> box_data;  // the FDAC payload, verbatim
};

// `data` is the raw AD structures as they came off the air. A truncated or
// malformed advertisement is not an error: parse_ad stops at the first
// structure that runs off the end, and whatever was already decoded stands.
AdvertMatch classify_ad(const uint8_t* data, size_t len);
AdvertMatch classify_ad(const std::vector<uint8_t>& data);

}  // namespace octo

#endif  // OCTO_ADVERT_H
