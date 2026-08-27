// The camera factory, alone in its own translation unit.
//
// Not fussiness. A static library is linked an object at a time, and
// octomancerd asks for a scanner while never linking CoreBluetooth's camera
// half -- so a single radio.o defining both factories drags that half into a
// program that has no use for it, and the build fails on a symbol nobody
// asked for. Splitting the two is what keeps each program paying only for the
// radio it actually uses.
#include <memory>

#include "radio.h"

namespace octo {

std::unique_ptr<CameraLink> make_camera_link() {
  if (dongle_selected()) return make_hci_camera_link();
#ifdef OCTO_HAVE_COREBLUETOOTH
  return make_corebluetooth_camera_link();
#else
  // No CoreBluetooth and no dongle. Every caller already treats a null link as
  // "this host has no radio".
  return nullptr;
#endif
}

}  // namespace octo
