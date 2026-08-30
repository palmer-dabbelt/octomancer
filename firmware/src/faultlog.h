// Why the last run ended.
//
// A dongle that dies has no way to say so. There is no console -- prj.conf
// switches it off, because the CDC port carries the box protocol and two
// writers on one wire is a protocol that cannot be parsed -- and Zephyr's
// default answer to a fatal error is to halt the CPU with interrupts locked.
// The USB pull-up stays asserted through all of that, so the host still sees
// a device, still lists it, and hangs forever on open() waiting for control
// transfers nobody is left to answer.
//
// That is a genuinely awful failure to be handed: an enumerated device that
// will not open looks exactly like a bad cable, a driver problem, or a port
// held by another process, and none of those are what happened. It cost the
// first evening this firmware ran.
//
// So: catch the fault, write down what it was in memory that survives a
// reset, and reboot. The dongle comes back, the host can open it again, and
// the first thing it says is what killed the last run. A crash becomes a line
// of text instead of a wedged port.
//
// Retained across the reset by living in a __noinit section: RAM keeps its
// contents through SYSRESETREQ, and __noinit is the promise that the C startup
// will not zero it. The magic is what distinguishes a real record from
// whatever was in that memory at power-on.
#ifndef OCTO_FW_FAULTLOG_H
#define OCTO_FW_FAULTLOG_H

#include <cstdint>
#include <string>

namespace octo {

struct FaultRecord {
  bool valid = false;
  // The machine was reset by the watchdog rather than by a fault. A different
  // kind of death and a more interesting one: a fault is code doing something
  // impossible, and this is code doing nothing at all. Nothing runs at the
  // moment it happens, so this is reconstructed on the way back up from the
  // reset cause the hardware records.
  bool watchdog = false;
  // Which liveness check had stopped holding, as of the last time anybody
  // looked. Empty when the watchdog fired without one -- which would mean the
  // loop itself stopped between feeds, since it is the loop that writes this.
  std::string starved;
  uint32_t reason = 0;  // Zephyr's K_ERR_*
  uint32_t pc = 0;      // where, if the exception frame had it
  uint32_t lr = 0;      // and who called it
  // How many faults in a row without a clean run in between. A box that dies
  // once on something odd and a box that cannot get through boot are different
  // problems, and the count is the only thing that tells them apart from here.
  uint32_t count = 0;
};

// What killed the previous run, or an invalid record if it ended cleanly.
// Clears the record, so it is reported once and not on every reconnection.
FaultRecord take_last_fault();

// One line for a person, in the shape src/boxmsg.h's `say` carries. Empty when
// there is nothing to report, so the caller can send it or not without asking.
std::string describe_fault(const FaultRecord& fault);

// Record which liveness check is currently failing, if any.
//
// Called from the watchdog's feeder every time it runs, not only when
// something is wrong. By the time the reset happens there is nowhere left to
// write from -- the nRF part allows about sixty microseconds between the
// timeout and the reset -- so the note has to already be there.
void note_watchdog_state(const std::string& starved);

// Called once a run has plainly got somewhere -- see kSettledSeconds in the
// implementation. Resets the consecutive-fault count, so a box that has been
// up for a minute is not still described as crash-looping.
void mark_run_settled();

}  // namespace octo

#endif  // OCTO_FW_FAULTLOG_H
