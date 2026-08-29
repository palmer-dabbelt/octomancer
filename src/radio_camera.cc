// The camera factory, alone in its own translation unit.
//
// Not fussiness. A static library is linked an object at a time, and
// octomancerd asks for a scanner while never linking CoreBluetooth's camera
// half -- so a single radio.o defining both factories drags that half into a
// program that has no use for it, and the build fails on a symbol nobody
// asked for. Splitting the two is what keeps each program paying only for the
// radio it actually uses.
#include <cstdio>
#include <memory>

#include "radio.h"

namespace octo {

std::unique_ptr<CameraLink> make_camera_link() {
  if (dongle_selected()) {
    // Loud rather than silent. A null link is reported by every caller as "no
    // radio", which here would be a lie: the dongle is plugged in and the
    // scanner half of it works. What is missing is the tool that drives the
    // event-loop camera client in src/camhci.h.
    std::fprintf(stderr,
                 "octomancer: the dongle's camera half now runs on the event"
                 " loop (src/camhci.h) and no tool here drives it yet.\n"
                 "  Use --radio=corebluetooth to set a clock; see"
                 " doc/box-notes.md for what replaces this.\n");
    return nullptr;
  }
#ifdef OCTO_HAVE_COREBLUETOOTH
  return make_corebluetooth_camera_link();
#else
  // No CoreBluetooth and no dongle. Every caller already treats a null link as
  // "this host has no radio".
  return nullptr;
#endif
}

}  // namespace octo
