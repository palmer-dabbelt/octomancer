// See firmware/src/scanner_zephyr.h.
#include "scanner_zephyr.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>

#include <cstring>
#include <string>
#include <utility>

#include "advert.h"
#include "hci.h"

namespace octo {
namespace {

// A legacy advertising payload is at most 31 bytes, and this scanner does not
// ask for extended advertising: a Tentacle is a legacy beacon and a Blackmagic
// camera advertises legacy too.
constexpr size_t kMaxAd = 31;

// The hand-off, and the only place on this device where one thread's work
// becomes another's. Zephyr delivers advertising reports on the Bluetooth
// receive thread, which may not touch the loop's data -- exactly the situation
// src/scanbridge.h exists for on the Mac, solved the same way: a bounded queue
// and a signal.
//
// Raw bytes rather than an Advert. Building one means allocating three
// std::strings, and allocating on the Bluetooth thread is both a lock this
// design does not have and work done at the wrong priority. The decode happens
// on the loop's thread, from these bytes.
struct RawReport {
  bt_addr_le_t addr;
  int64_t ticks;  // when it arrived, not when it was read
  int8_t rssi;
  uint8_t len;
  uint8_t data[kMaxAd];
};

// Sixteen is about four seconds of one busy Tentacle, and the loop drains it
// within a millisecond of being woken. It is sized for a stall, not a backlog.
constexpr size_t kQueueDepth = 16;

K_MSGQ_DEFINE(g_reports, sizeof(RawReport), kQueueDepth, 4);

// Raised by the Bluetooth thread, waited on by the loop.
struct k_poll_signal g_signal;

// One radio, one scanner. A file-static instance pointer because Zephyr's scan
// callback carries no user data, and inventing a registry for a device that
// has exactly one antenna would be ceremony.
class ZephyrScanner;
ZephyrScanner* g_scanner = nullptr;

volatile uint32_t g_dropped = 0;

// What a device is called in the roster. One function because two spellings of
// the same device's identifier is a device the registry cannot match to
// itself: the advertisement creates the row and the scan response carries the
// name, and they have to agree.
//
// Same marking as the Mac's dongle path: a resolvable private address rotates
// every fifteen minutes or so, and nothing can make it stable, so a roster
// full of one-sighting entries explains itself.
std::string advert_id(const hci::Address& addr) {
  std::string id = hci::address_to_string(addr);
  if (!hci::address_is_stable(addr)) id += " (private)";
  return id;
}

hci::Address to_address(const bt_addr_le_t& in) {
  hci::Address out;
  // Zephyr holds an address the way it travels, least significant byte first.
  // src/hci.h holds it the way it is printed. Getting this backwards produces
  // a plausible-looking identifier for a device that does not exist, and a
  // roster that never matches the Mac's.
  for (size_t i = 0; i < 6; ++i) out.bytes[i] = in.a.val[5 - i];
  // BT_ADDR_LE_PUBLIC/RANDOM/PUBLIC_ID/RANDOM_ID are 0..3, and so are
  // hci::kAddrPublic and its neighbours. The static_asserts below keep that
  // true rather than trusting it.
  out.type = in.type;
  return out;
}

static_assert(BT_ADDR_LE_PUBLIC == hci::kAddrPublic, "address type drift");
static_assert(BT_ADDR_LE_RANDOM == hci::kAddrRandom, "address type drift");

class ZephyrScanner : public Scanner {
 public:
  ZephyrScanner(Loop* loop, const BoxClock* clock, AdvertHandler on_advert,
                SightingHandler on_camera, StateHandler on_state)
      : loop_(loop),
        clock_(clock),
        on_advert_(std::move(on_advert)),
        on_camera_(std::move(on_camera)),
        on_state_(std::move(on_state)) {}

  ~ZephyrScanner() override { stop(); }

  bool start(std::string* err) override {
    if (started_) return true;
    k_poll_signal_init(&g_signal);
    g_scanner = this;

    Handle handle;
    handle.object = &g_signal;
    source_ = loop_->add_source(
        handle, kRead, [this](int) { drain(); }, [](const std::string&) {});

    // Passive to begin with, for the reason src/scanner_hci.cc gives: the
    // clock is in the advertisement, so there is usually no reason to provoke
    // a scan response and announce ourselves. FILTER_DUPLICATE is off because
    // a Tentacle's whole content is that it changes every time.
    //
    // "Usually" is the change. A Tentacle puts its clock in the advertisement
    // and its *name* in the scan response, so a purely passive radio knows
    // exactly what time every box thinks it is and cannot name any of them --
    // which is why every device a dongle reported used to be listed by its
    // hardware address. See set_active(): the radio asks when it has something
    // to learn and goes quiet again once it has learned it.
    const int rc = restart(false);
    if (rc != 0) {
      loop_->remove_source(source_);
      source_ = kNoSource;
      g_scanner = nullptr;
      if (err) *err = "bt_le_scan_start failed";
      // The controller is up -- main() would have failed otherwise -- so this
      // is a radio that will not scan rather than a host with no radio.
      report("poweredOff");
      return false;
    }

    started_ = true;
    report("poweredOn");
    return true;
  }

  // Switch the scan between passive and active, which means stopping and
  // starting it -- there is no way to change the type of a running scan.
  //
  // Cheap but not free, and it drops whatever was in flight, which is why
  // src/naming.h damps how often it may happen rather than deciding afresh on
  // every advertisement.
  void set_active(bool active) override {
    if (!started_ || active == active_) return;
    bt_le_scan_stop();
    if (restart(active) != 0) {
      // Back to what was working. A radio that has stopped scanning is a room
      // that has gone silent, and that is much worse than one whose devices
      // are listed by address.
      restart(active_);
      return;
    }
    active_ = active;
  }

  bool active() const { return active_; }

  void stop() override {
    if (!started_) return;
    bt_le_scan_stop();
    if (source_ != kNoSource) loop_->remove_source(source_);
    source_ = kNoSource;
    g_scanner = nullptr;
    started_ = false;
  }

 private:
  void report(const std::string& state) {
    if (on_state_) on_state_(state);
  }

  // Bluetooth receive thread. Copies and signals, and does nothing else.
  static void scan_cb(const bt_addr_le_t* addr, int8_t rssi, uint8_t adv_type,
                      struct net_buf_simple* buf) {
    (void)adv_type;
    if (addr == nullptr || buf == nullptr) return;

    RawReport r;
    r.addr = *addr;
    r.ticks = k_uptime_ticks();
    r.rssi = rssi;
    r.len = static_cast<uint8_t>(buf->len > kMaxAd ? kMaxAd : buf->len);
    std::memcpy(r.data, buf->data, r.len);

    if (k_msgq_put(&g_reports, &r, K_NO_WAIT) != 0) {
      // Never block here. This thread blocking is the controller's receive
      // path blocking, which loses packets that are not even ours.
      ++g_dropped;
      return;
    }
    k_poll_signal_raise(&g_signal, 1);
  }

  // Loop thread.
  int restart(bool active) {
    struct bt_le_scan_param param = {};
    param.type = active ? BT_HCI_LE_SCAN_ACTIVE : BT_HCI_LE_SCAN_PASSIVE;
    param.options = BT_LE_SCAN_OPT_NONE;
    param.interval = BT_GAP_SCAN_FAST_INTERVAL;
    param.window = BT_GAP_SCAN_FAST_WINDOW;
    return bt_le_scan_start(&param, &ZephyrScanner::scan_cb);
  }

  void drain() {
    RawReport r;
    while (k_msgq_get(&g_reports, &r, K_NO_WAIT) == 0) {
      handle(r);
    }
  }

  void handle(const RawReport& r) {
    const AdvertMatch m = classify_ad(r.data, r.len);
    if (!m.is_box && !m.is_camera) {
      // A scan response, which is a separate packet from the advertisement it
      // answers: a Tentacle puts its clock in the advertisement and its name
      // in the response, so this report has a name in it and no service data
      // at all. That is the only reason to scan actively, and dropping it
      // here is why every device this dongle reported was listed by its
      // hardware address.
      //
      // Passed on as a name and nothing else. The registry ignores names for
      // devices it does not already hold, so a named stranger walking past
      // does not become a row.
      if (!m.name.empty() && on_advert_) {
        Advert named;
        // Built exactly as the advertisement's is, suffix and all. The
        // registry matches a name to a device by identifier and creates
        // nothing, so an identifier that differs by so much as " (private)"
        // is a name that silently goes nowhere -- and it would do so only for
        // devices with rotating addresses, which is the hardest case to
        // notice by looking at a table.
        named.id = advert_id(to_address(r.addr));
        named.name = m.name;
        named.rssi = r.rssi;
        named.name_only = true;
        on_advert_(named);
      }
      return;
    }

    // The instant the packet arrived, not the instant it reached here. An
    // advertisement can wait in the queue while the loop finishes something
    // else, and charging the box for that wait is the mistake
    // SyncDaemon::error_from() exists to undo.
    const double mono = static_cast<double>(k_ticks_to_us_floor64(r.ticks)) * 1e-6;
    const double wall = clock_->wall_at(mono);

    const std::string id = advert_id(to_address(r.addr));

    if (m.is_camera && on_camera_) {
      Sighting seen;
      seen.id = id;
      seen.name = m.name;
      seen.rssi = r.rssi;
      seen.mono = mono;
      seen.wall = wall;
      on_camera_(seen);
    }

    if (m.is_box && on_advert_) {
      Advert advert;
      advert.id = id;
      advert.name = m.name;
      advert.rssi = r.rssi;
      advert.data = m.box_data;
      advert.mono = mono;
      advert.wall = wall;
      on_advert_(advert);
    }
  }

  Loop* loop_ = nullptr;
  const BoxClock* clock_ = nullptr;
  SourceId source_ = kNoSource;
  AdvertHandler on_advert_;
  SightingHandler on_camera_;
  StateHandler on_state_;
  bool started_ = false;
  bool active_ = false;
};

}  // namespace

std::unique_ptr<Scanner> make_zephyr_scanner(Loop* loop, const BoxClock* clock,
                                             Scanner::AdvertHandler on_advert,
                                             Scanner::SightingHandler on_camera,
                                             Scanner::StateHandler on_state) {
  return std::unique_ptr<Scanner>(
      new ZephyrScanner(loop, clock, std::move(on_advert), std::move(on_camera),
                        std::move(on_state)));
}

uint32_t zephyr_scanner_dropped() { return g_dropped; }

}  // namespace octo
