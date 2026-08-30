// The silicon half of the watchdog. See src/watchdog.h for the judgement half.
//
// The nRF52840 watchdog has one property that shapes everything here: **once
// started it cannot be stopped or reconfigured until the chip resets.** There
// is no turning it off for a moment, no lengthening the timeout because
// something is taking a while, and no second chance at the configuration. A
// mistake here is a dongle that reboots forever and can only be recovered by
// holding a button down while plugging it in.
//
// Two consequences, both deliberate:
//
//   * It is started last, after the radio is up and the daemon is running, so
//     that a slow boot cannot trip it. Nothing before that point is watched --
//     which is the right trade, because a box that never finishes booting is
//     a box a reset would not help.
//   * The timeout is generous. It is there to catch a machine that has stopped,
//     not to police latency, and the cost of being wrong is asymmetric: a late
//     feed that resets a working dongle mid-shoot is a far worse failure than
//     a wedge that takes an extra ten seconds to clear.
//
// Every check in the policy becomes one hardware reload register, and the part
// reloads only when all of them have been fed. So the machine resets when any
// single check stops holding, which is what makes "the loop is alive" and "the
// USB side is alive" separately enforceable.
#ifndef OCTO_FW_HWWATCHDOG_H
#define OCTO_FW_HWWATCHDOG_H

#include <string>

#include "loop.h"
#include "watchdog.h"

namespace octo {

// Installs one channel per check, starts the timer, and schedules the feeding
// on `loop`. The policy must outlive the loop, and no further checks may be
// added afterwards: the channels are allocated here and the hardware will not
// accept more.
//
// Feeding happens from the loop on purpose. A watchdog fed from a timer
// interrupt proves only that interrupts still work, which was never in doubt --
// it is the loop going round that is the claim worth making.
//
// Returns false and leaves the machine unwatched if the timer will not start,
// which is a state worth reporting rather than dying in: a dongle with no
// watchdog still works, it just cannot recover from a wedge by itself.
bool start_watchdog(Loop* loop, WatchdogPolicy* policy, double timeout_s,
                    std::string* err);

}  // namespace octo

#endif  // OCTO_FW_HWWATCHDOG_H
