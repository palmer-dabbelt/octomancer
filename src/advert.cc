// See src/advert.h. Nothing here touches a radio; it is the AD decoder plus
// two constants, which is exactly why it can be tested with no hardware.
#include "advert.h"

#include "bmd.h"
#include "hci.h"

namespace octo {
namespace {

// Built once. uuid_const parses a string, and doing that per advertisement is
// a parse per packet in a busy room for an answer that never changes.
const hci::Uuid& fdac_uuid() {
  static const hci::Uuid u = hci::uuid_from_16(0xfdac);
  return u;
}

const hci::Uuid& camera_uuid() {
  static const hci::Uuid u = hci::uuid_const(bmd::kServiceCamera);
  return u;
}

}  // namespace

AdvertMatch classify_ad(const uint8_t* data, size_t len) {
  AdvertMatch out;
  if (data == nullptr || len == 0) return out;

  const hci::AdInfo info = hci::summarise_ad(hci::parse_ad(data, len));
  out.name = info.name;

  for (const hci::Uuid& u : info.services) {
    if (u == camera_uuid()) {
      out.is_camera = true;
      break;
    }
  }

  for (const auto& sd : info.service_data) {
    // An empty payload is not a sighting of a box. The decoder downstream has
    // nothing to read, and reporting it would put a device on the roster that
    // can never acquire a time.
    if (!(sd.first == fdac_uuid()) || sd.second.empty()) continue;
    out.is_box = true;
    out.box_data = sd.second;
    break;
  }

  return out;
}

AdvertMatch classify_ad(const std::vector<uint8_t>& data) {
  return classify_ad(data.data(), data.size());
}

}  // namespace octo
