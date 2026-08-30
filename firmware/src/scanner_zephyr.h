// The passive scanner, on the dongle's own radio.
//
// The third implementation of src/scanner.h, and deliberately the same shape
// as the other two: listen to everything, pull the FDAC service data out of
// anything that carries it, and notice a Blackmagic camera by the service it
// advertises rather than by its name. Nothing connects to anything here.
//
// What it decides about a packet is not decided here. src/advert.h is the one
// classifier all three radios share, and src/hci.h's AD decoder underneath it
// is the same code the Mac runs -- so a device is the same device, with the
// same identifier, whichever radio heard it. That is the property the two-radio
// comparison in doc/box-notes.md turns on: if the classifier were written
// twice, a disagreement between the radios would be indistinguishable from a
// disagreement between the two copies of the code.
#ifndef OCTO_FW_SCANNER_ZEPHYR_H
#define OCTO_FW_SCANNER_ZEPHYR_H

#include <cstdint>
#include <memory>

#include "boxclock.h"
#include "loop.h"
#include "scanner.h"

namespace octo {

// `loop` and `clock` outlive the scanner. Bluetooth must already be enabled;
// bringing the controller up is main()'s job because the camera client wants
// it too, and a radio switched on twice is a radio nobody owns.
std::unique_ptr<Scanner> make_zephyr_scanner(Loop* loop, const BoxClock* clock,
                                             Scanner::AdvertHandler on_advert,
                                             Scanner::SightingHandler on_camera,
                                             Scanner::StateHandler on_state);

// Advertisements the radio delivered and the queue could not hold. An
// advertisement is a perishable statement about the present and the next one
// is a few hundred milliseconds behind it, so a bounded queue is right --
// silence about what it dropped would not be. Same argument as
// src/scanbridge.h, which is the Mac's version of this hand-off.
uint32_t zephyr_scanner_dropped();

}  // namespace octo

#endif  // OCTO_FW_SCANNER_ZEPHYR_H
