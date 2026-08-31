// The box protocol over Bluetooth, from the Mac's end.
//
// A dongle in a USB port is reachable down the cable. The same dongle in a
// phone charger, across the room, is reachable only this way -- and that is
// the arrangement the whole design is aimed at, so this is the piece that
// makes a dongle a member of the system rather than a peripheral of a laptop.
//
// **This makes octomancerd connect to something, which it has never done.**
// Worth stating plainly, because the opening comment of src/octomancerd.cc has
// always said it "never connects to anything and never writes to anything, so
// it cannot disturb the Tentacle app, a camera, or a recording in progress".
// That promise is about other people's equipment and it still holds: this
// connects to a dongle running our own firmware, identified by its own
// greeting, and to nothing else. It will not connect to a camera, a timecode
// box, or an unknown peripheral.
//
// It is not free, though, and the cost lands on the one job octomancerd
// exists to do. A CoreBluetooth central that holds a connection is a radio
// interleaving connection events with scanning, so the Mac hears slightly
// fewer advertisements while this link is up. That is the argument for
// preferring the cable whenever there is one -- see want_bluetooth() in
// src/dongle.h -- rather than any doubt about whether the radio link works.
//
// **The greeting is what makes a peripheral ours**, exactly as it is over USB.
// Nordic's UART Service is a well-known UUID and anybody may advertise it, so
// finding it is not identification. Nothing is written to a peripheral beyond
// what GATT discovery requires until it has said `hello`, and a peripheral
// that does not is disconnected and left alone.
#ifndef OCTO_BOXBLE_H
#define OCTO_BOXBLE_H

#include <memory>
#include <string>

#include "boxlink.h"

namespace octo {

// Look for a dongle over the air and speak the box protocol to it.
//
// Returns immediately, with the search still running: scanning, connecting,
// discovering and subscribing take seconds, and a control daemon may not block
// on any of them. Until it has all happened, ready() is false and send() is a
// no-op -- so a caller polls pump() and watches ready(), the same as it would
// for a cable that has not been plugged in yet.
//
// `want_name` restricts the search to one peripheral by advertised name, for a
// bench with two dongles on it. Empty means the first one that greets us.
//
// Null with *err set means this host cannot do it at all -- no CoreBluetooth,
// or no permission -- which is different from "no dongle has answered yet" and
// is the only case a caller should give up over.
std::unique_ptr<BoxTransport> open_box_ble(const std::string& want_name,
                                           std::string* err);

}  // namespace octo

#endif  // OCTO_BOXBLE_H
