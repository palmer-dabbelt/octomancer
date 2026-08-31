// The handful of things only the box can be asked, answered before the daemon
// sees them.
//
// src/syncd.h's message dispatch is shared code and must stay that way: it is
// the same on a Mac and on a dongle, and a verb that only means something on
// one of them does not belong in it. But there are two such verbs, and both
// earn their place.
//
// `dfu` is the important one. Getting a new image onto this dongle otherwise
// means holding a button while plugging it in -- doc/dongle-notes.md records
// how board-specific and easy to get wrong that is -- and a device that can be
// asked to reboot into its bootloader can be reprogrammed over the same cable
// it already talks on. The firmware that is running is what makes the next one
// installable, which is worth getting in early: an image without this can only
// ever be replaced by hand.
//
// `boxstats` is the counters. Everything on this device that can discard
// something counts what it discarded -- the advertisement queue, the transmit
// ring, over-long lines -- and none of that is visible unless something asks.
// A silent drop is indistinguishable from a quiet room, which is the failure
// this whole project keeps rediscovering.
#ifndef OCTO_FW_BOXADMIN_H
#define OCTO_FW_BOXADMIN_H

#include <cstdint>
#include <string>

#include "syncd.h"

namespace octo {

// What the transport this line arrived on has had to throw away.
//
// Passed in rather than read off the peer, because there are two kinds of peer
// now -- a cable and a radio link -- and the counters are the only thing about
// them these verbs care about. A `boxstats` asked over Bluetooth should report
// what Bluetooth dropped, not what the cable did.
struct PeerStats {
  uint32_t dropped_tx = 0;
  uint32_t dropped_rx = 0;
  uint32_t long_lines = 0;
};

// True if the line was one of ours and has been answered. False means the
// daemon should see it, which is every ordinary message.
bool handle_box_admin(const std::string& line, MsgPeer* peer,
                      const PeerStats& stats);

}  // namespace octo

#endif  // OCTO_FW_BOXADMIN_H
