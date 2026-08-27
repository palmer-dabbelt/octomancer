// The passive scanner, over the dongle.
//
// The same job scanner_mac.mm does, and deliberately the same shape: listen to
// everything, pull the FDAC service data out of anything that carries it, and
// notice a Blackmagic camera by the service it advertises rather than by its
// name. Nothing connects to anything here.
//
// One difference is visible to callers and worth stating plainly, because it
// affects stored state. CoreBluetooth hands out an opaque per-host UUID for
// each device; HCI hands out the real Bluetooth address. Both are stable
// enough to key a registry on, but they are not the same string, so a bench
// learned over one radio is not recognised over the other. See the note on
// Advert::id below.
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bmd.h"
#include "hci.h"
#include "hcilink.h"
#include "radio.h"
#include "scanner.h"
#include "timeutil.h"

namespace octo {
namespace {

class HciScanner : public Scanner {
 public:
  HciScanner(AdvertHandler on_advert, SightingHandler on_camera,
             StateHandler on_state)
      : on_advert_(std::move(on_advert)),
        on_camera_(std::move(on_camera)),
        on_state_(std::move(on_state)),
        fdac_(hci::uuid_from_16(0xfdac)),
        camera_(hci::uuid_const(bmd::kServiceCamera)) {}

  ~HciScanner() override { stop(); }

  bool start(std::string* err) override {
    hci::Link::Options opts;
    opts.device = radio_options().device;
    opts.trace = radio_options().trace;

    std::string open_err;
    link_ = hci::Link::open(opts, &open_err);
    if (!link_) {
      // "unsupported" is the word octomancerd already prints when a host has
      // no radio, and hci::no_port_found is how a missing dongle is told apart
      // from a dongle that would not open.
      report(hci::no_port_found(open_err) ? "unsupported" : "unauthorized");
      if (err) *err = open_err;
      return false;
    }

    // Duplicate filtering off. The controller would otherwise report each
    // device once and never again, and a Tentacle's whole value is that it
    // repeats its clock several times a second.
    if (!link_->start_scan(/*active=*/false, /*filter_duplicates=*/false,
                           [this](const hci::AdvReport& r) { on_report(r); },
                           err)) {
      report("poweredOff");
      link_.reset();
      return false;
    }
    report("poweredOn");
    return true;
  }

  void stop() override {
    if (!link_) return;
    link_->stop_scan(nullptr);
    link_.reset();
  }

 private:
  void report(const std::string& state) {
    if (on_state_) on_state_(state);
  }

  void on_report(const hci::AdvReport& r) {
    hci::AdInfo info = hci::summarise_ad(hci::parse_ad(r.data));

    // A camera is identified by its service UUID, never by its name. There is
    // a Tentacle on this bench called "BMPCC", and a name match would hand the
    // sync daemon a box that has no control characteristic on it.
    if (on_camera_) {
      for (const hci::Uuid& u : info.services) {
        if (u == camera_) {
          Sighting seen;
          seen.id = device_id(r);
          seen.name = info.name;
          seen.rssi = r.rssi;
          seen.mono = mono_now();
          seen.wall = wall_now();
          on_camera_(seen);
          break;
        }
      }
    }

    if (!on_advert_) return;
    for (const auto& sd : info.service_data) {
      if (!(sd.first == fdac_) || sd.second.empty()) continue;
      Advert advert;
      advert.id = device_id(r);
      advert.name = info.name;
      advert.rssi = r.rssi;
      advert.data = sd.second;
      advert.mono = mono_now();
      advert.wall = wall_now();
      on_advert_(advert);
      break;
    }
  }

  // The address, printed. Unlike CoreBluetooth's per-host UUID this is the
  // device's real identity, so a bench keyed on it means the same thing on
  // every machine -- but a device using a resolvable private address changes
  // it every fifteen minutes or so, and no amount of care here can make that
  // stable. Such a device gets a marked id so a registry full of one-sighting
  // entries is self-explanatory rather than mysterious.
  std::string device_id(const hci::AdvReport& r) const {
    std::string id = hci::address_to_string(r.addr);
    if (!hci::address_is_stable(r.addr)) id += " (private)";
    return id;
  }

  AdvertHandler on_advert_;
  SightingHandler on_camera_;
  StateHandler on_state_;
  hci::Uuid fdac_;
  hci::Uuid camera_;
  std::unique_ptr<hci::Link> link_;
};

}  // namespace

std::unique_ptr<Scanner> make_hci_scanner(Scanner::AdvertHandler on_advert,
                                          Scanner::SightingHandler on_camera,
                                          Scanner::StateHandler on_state) {
  return std::unique_ptr<Scanner>(new HciScanner(
      std::move(on_advert), std::move(on_camera), std::move(on_state)));
}

}  // namespace octo
