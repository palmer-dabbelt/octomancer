// The passive scanner, over the dongle.
//
// The same job scanner_mac.mm does, and deliberately the same shape: listen to
// everything, pull the FDAC service data out of anything that carries it, and
// notice a Blackmagic camera by the service it advertises rather than by its
// name. Nothing connects to anything here.
//
// Starting is now in two parts, which is what the shared Scanner interface was
// always shaped for. Opening the port either works or does not and says so
// straight away; the controller coming up and the scan actually starting are
// several round trips later, and arrive as a state report -- the same way
// CoreBluetooth has always reported them. A caller that watches on_state
// rather than the return value of start() behaves identically over both
// radios.
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

#include "advert.h"
#include "hci.h"
#include "hcilink.h"
#include "hcishare.h"
#include "radio.h"
#include "scanner.h"
#include "timeutil.h"

namespace octo {
namespace {

class HciScanner : public Scanner {
 public:
  // `radio` null means "open a dongle for this scanner alone", which is what
  // the plain factory does and what a program that only listens wants.
  // Non-null means somebody else owns the radio and this is one of its jobs;
  // see src/hcishare.h.
  HciScanner(hci::SharedLink* radio, AdvertHandler on_advert,
             SightingHandler on_camera, StateHandler on_state)
      : borrowed_(radio),
        on_advert_(std::move(on_advert)),
        on_camera_(std::move(on_camera)),
        on_state_(std::move(on_state)) {}

  ~HciScanner() override { stop(); }

  bool start(std::string* err) override {
    // Back to the radio this scanner was built with, which stop() cleared.
    // Deriving it here rather than leaving it in place is what stops a
    // stop-then-start from silently deciding it owns the dongle: it does not,
    // and opening a second link on a port somebody else is already driving is
    // the exact failure src/hcishare.h exists to end.
    radio_ = borrowed_;
    if (!radio_) {
      hci::Link::Options opts;
      opts.device = radio_options().device;
      opts.trace = radio_options().trace;

      std::string open_err;
      own_ = hci::SharedLink::open(&default_loop(), opts, &open_err);
      if (!own_) {
        // "unsupported" is the word octomancerd already prints when a host has
        // no radio, and hci::no_port_found is how a missing dongle is told
        // apart from a dongle that would not open.
        report(hci::no_port_found(open_err) ? "unsupported" : "unauthorized");
        if (err) *err = open_err;
        return false;
      }
      radio_ = own_.get();
    }

    user_ = radio_->add_user("tentacle scan");
    // The dongle being unplugged mid-scan is the same event to a caller as the
    // radio being switched off, and is the one thing that must not pass
    // silently: a scanner reporting nothing looks exactly like a quiet room.
    user_->set_closed_handler([this](const std::string&) {
      report("poweredOff");
    });
    user_->when_ready([this](bool ok, const std::string& why) {
      on_ready(ok, why);
    });
    return true;
  }

  void stop() override {
    // Destroying the user is what releases this scanner's share of the radio.
    // If somebody else still wants it scanning, it keeps scanning; if not, it
    // stops. Either way nothing further arrives here.
    user_.reset();
    own_.reset();
    radio_ = nullptr;
  }

 private:
  void report(const std::string& state) {
    if (on_state_) on_state_(state);
  }

  void on_ready(bool ok, const std::string& why) {
    if (!ok) {
      // The port opened and the controller would not come up. That is a broken
      // dongle rather than an absent one, so it is not "unsupported".
      report("poweredOff");
      (void)why;
      return;
    }
    if (!user_) return;
    // Passive: the clock is in the advertisement, so there is never a reason
    // to provoke a scan response and announce ourselves. If something else
    // sharing this radio wants an active scan it will get one, and this still
    // sees everything it would have seen -- see src/hcishare.h.
    user_->start_scan(/*active=*/false,
                      [this](const hci::AdvReport& r) { on_report(r); },
                      [this](bool started, const std::string&) {
                        report(started ? "poweredOn" : "poweredOff");
                      });
  }

  void on_report(const hci::AdvReport& r) {
    // One classifier for every radio -- see src/advert.h. The firmware
    // scanner asks the same question of the same bytes, and a second copy of
    // this is how the two radios would come to disagree about what a device
    // is.
    const AdvertMatch m = classify_ad(r.data);
    if (!m.is_box && !m.is_camera) return;

    // Computed once. It is the same string for both handlers, and it is the
    // expensive part of this function.
    const std::string id = device_id(r);

    if (m.is_camera && on_camera_) {
      Sighting seen;
      seen.id = id;
      seen.name = m.name;
      seen.rssi = r.rssi;
      seen.mono = mono_now();
      seen.wall = wall_now();
      on_camera_(seen);
    }

    if (m.is_box && on_advert_) {
      Advert advert;
      advert.id = id;
      advert.name = m.name;
      advert.rssi = r.rssi;
      advert.data = m.box_data;
      advert.mono = mono_now();
      advert.wall = wall_now();
      on_advert_(advert);
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

  // The radio this scanner was handed, or null if it is meant to open its
  // own. Never cleared, because it is the answer to "whose radio is this",
  // which does not change when the scan stops.
  hci::SharedLink* const borrowed_ = nullptr;
  // Either borrowed_ or own_'s, and null while stopped.
  hci::SharedLink* radio_ = nullptr;
  std::unique_ptr<hci::SharedLink> own_;
  std::unique_ptr<hci::SharedLink::User> user_;
  AdvertHandler on_advert_;
  SightingHandler on_camera_;
  StateHandler on_state_;
};

}  // namespace

std::unique_ptr<Scanner> make_hci_scanner(Scanner::AdvertHandler on_advert,
                                          Scanner::SightingHandler on_camera,
                                          Scanner::StateHandler on_state) {
  return std::unique_ptr<Scanner>(new HciScanner(
      nullptr, std::move(on_advert), std::move(on_camera),
      std::move(on_state)));
}

std::unique_ptr<Scanner> make_hci_scanner_on(hci::SharedLink* radio,
                                             Scanner::AdvertHandler on_advert,
                                             Scanner::SightingHandler on_camera,
                                             Scanner::StateHandler on_state) {
  return std::unique_ptr<Scanner>(new HciScanner(
      radio, std::move(on_advert), std::move(on_camera), std::move(on_state)));
}

}  // namespace octo
