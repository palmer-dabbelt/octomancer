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
  if (radio_options().kind == RadioKind::kFake) {
    return make_fake_camera_link();
  }
  // Only when the dongle was actually asked for.
  //
  // This used to test dongle_selected(), which is true under `--radio auto`
  // the moment a dongle is plugged into any USB port. The consequence was not
  // a worse choice of radio, it was a restart loop: the shipped
  // octomancer-sync LaunchAgent runs the blocking path with no --radio flag,
  // that path exits 1 when this returns null, and launchd's KeepAlive brings
  // it back every ten seconds forever. Plugging in a dongle -- which the
  // README invites -- was enough to do it, and the only visible symptom was
  // an agent that always looked like it was running.
  //
  // Auto means "pick something that works". For a blocking camera link the
  // only thing that works is CoreBluetooth, so auto picks it and says
  // nothing. Insisting on the dongle still gets the honest refusal below.
  if (dongle_requested()) {
    // Loud rather than silent. A null link is reported by every caller as "no
    // radio", which here would be a lie: the dongle is plugged in and the
    // scanner half of it works. What is missing is a *blocking* camera client
    // for it, and there will not be one -- the dongle's camera half is
    // src/camhci.h, on the event loop, driven by `octomancer-sync --daemon`.
    std::fprintf(stderr,
                 "octomancer: the dongle's camera half runs on the event loop"
                 " (src/camhci.h), which this mode cannot drive.\n"
                 "  Use `octomancer-sync --daemon` to set a clock over the"
                 " dongle, or --radio=corebluetooth here; see"
                 " doc/box-notes.md.\n");
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
