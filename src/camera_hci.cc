// The camera link, over the dongle.
//
// Same contract as camera_mac.mm -- everything blocks, callbacks are turned
// back into return values -- but with one difference that cannot be hidden and
// should not be: pairing. CoreBluetooth pairs by putting a panel on the screen
// and remembers the bond in the system keychain, which is why
// doc/ble-write-failure-report.md could observe that everything "worked
// immediately". The dongle has no keychain and no screen. It arrives as a
// stranger with an address the camera has never seen, so the camera displays a
// six-digit passkey and waits.
//
// The passkey therefore has to come from somewhere: --passkey, or
// OCTOMANCER_PASSKEY, or a prompt on the terminal. Under launchd there is
// nobody to prompt, so an unattended agent needs the value configured or it
// will not be able to write a clock. That is a real operational difference
// from the CoreBluetooth path and is called out in doc/dongle-notes.md.
#include <cstdio>
#include <algorithm>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "att.h"
#include "bmd.h"
#include "camera.h"
#include "crypto.h"
#include "hci.h"
#include "hcilink.h"
#include "radio.h"
#include "smp.h"
#include "timeutil.h"

namespace octo {
namespace {

// Where a characteristic lives once discovery has found it.
struct CharHandles {
  uint16_t value = 0;
  uint16_t cccd = 0;
  uint8_t properties = 0;
  bool found() const { return value != 0; }
};

class HciCamera : public CameraLink {
 public:
  ~HciCamera() override { disconnect(); }

  bool start(std::string* err) {
    hci::Link::Options opts;
    opts.device = radio_options().device;
    opts.trace = radio_options().trace;
    link_ = hci::Link::open(opts, err);
    if (!link_) return false;

    link_->set_connection_handlers(
        nullptr, [this](uint16_t handle, uint8_t reason) {
          (void)reason;
          std::lock_guard<std::mutex> lock(mu_);
          if (handle == conn_) {
            conn_ = 0;
            subscribed_ = false;
          }
          cv_.notify_all();
        });
    link_->set_att_handler(
        [this](uint16_t conn, const std::vector<uint8_t>& pdu) {
          on_att(conn, pdu);
        });
    link_->set_smp_handler(
        [this](uint16_t conn, const std::vector<uint8_t>& pdu) {
          on_smp(conn, pdu);
        });
    return true;
  }

  bool ready(double timeout, std::string* err) override {
    (void)timeout;
    if (link_) return true;
    if (err) *err = "the dongle is not open";
    return false;
  }

  ScanResult scan(double seconds, const std::string& name_hint,
                  bool want_all, const CameraSeen& on_camera) override {
    ScanResult result;
    if (!link_) return result;

    hci::Uuid camera_service = hci::uuid_const(bmd::kServiceCamera);
    hci::Uuid fdac = hci::uuid_from_16(0xfdac);

    std::map<std::string, CameraDevice> seen;
    std::set<std::string> tentacles;

    std::string err;
    if (!link_->start_scan(
            /*active=*/true, /*filter_duplicates=*/false,
            [&](const hci::AdvReport& r) {
              hci::AdInfo info = hci::summarise_ad(hci::parse_ad(r.data));
              std::string id = hci::address_to_string(r.addr);

              for (const auto& sd : info.service_data) {
                if (sd.first == fdac) tentacles.insert(id);
              }

              bool by_uuid = false;
              for (const hci::Uuid& u : info.services) {
                if (u == camera_service) {
                  by_uuid = true;
                  break;
                }
              }

              CameraDevice& dev = seen[id];
              dev.id = id;
              if (!info.name.empty()) dev.name = info.name;
              dev.rssi = r.rssi;
              // Once proven, always proven: a later advertisement from the
              // same device may be a scan response with no service list in it,
              // and demoting the device on that basis would lose it.
              dev.by_service_uuid = dev.by_service_uuid || by_uuid;
            },
            &err)) {
      return result;
    }

    // An active scan, so a device that keeps its name in the scan response
    // still gets one. Unlike the passive Tentacle scan, here the name is worth
    // provoking: it is what a person recognises the camera by.
    double until = mono_now() + seconds;
    while (mono_now() < until) {
      struct timespec ts = {0, 100 * 1000 * 1000};
      nanosleep(&ts, nullptr);
    }
    link_->stop_scan(nullptr);

    result.total = static_cast<int>(seen.size());
    result.tentacles = static_cast<int>(tentacles.size());

    std::string hint_lower = name_hint;
    std::transform(hint_lower.begin(), hint_lower.end(), hint_lower.begin(),
                   ::tolower);

    for (const auto& kv : seen) {
      const CameraDevice& dev = kv.second;
      if (want_all) result.all.push_back(dev);
      bool matches = dev.by_service_uuid;
      if (!matches && !hint_lower.empty() && !dev.name.empty()) {
        std::string name_lower = dev.name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                       ::tolower);
        matches = name_lower.find(hint_lower) != std::string::npos;
      }
      if (matches) result.cameras.push_back(dev);
    }

    // Proof before guesswork, then signal strength. A name match is only ever
    // a guess -- there is a Tentacle on this bench called "BMPCC".
    std::sort(result.cameras.begin(), result.cameras.end(),
              [](const CameraDevice& a, const CameraDevice& b) {
                if (a.by_service_uuid != b.by_service_uuid) {
                  return a.by_service_uuid;
                }
                return a.rssi > b.rssi;
              });
    std::sort(result.all.begin(), result.all.end(),
              [](const CameraDevice& a, const CameraDevice& b) {
                return a.rssi > b.rssi;
              });

    // Every camera is reported, but only once the scan is over: this backend
    // collects advertisements and classifies them afterwards, so there is no
    // point during the scan at which it knows it has found one. The contract
    // is "called for each camera", not "called early", and on the dongle the
    // two differ.
    if (on_camera) {
      for (const CameraDevice& dev : result.cameras) on_camera(dev);
    }
    return result;
  }

  bool read_status(std::vector<uint8_t>* out, double timeout,
                   std::string* err) override {
    (void)out;
    (void)timeout;
    // Not wired up. The GATT client in src/att.h can read a characteristic,
    // and pairing over this backend is meant to go through src/smp.cc with a
    // passkey supplied rather than through an OS dialog, so this wants doing
    // properly alongside that rather than as a stub that half works. Nothing
    // has been run against a dongle yet; doc/dongle-notes.md says so.
    if (err) {
      *err =
          "reading Camera Status is not implemented for the dongle backend"
          " yet";
    }
    return false;
  }

  bool connect(const std::string& id, double timeout,
               std::string* err) override {
    if (!link_) {
      if (err) *err = "the dongle is not open";
      return false;
    }
    disconnect();

    hci::Address peer;
    if (!hci::address_from_string(id, &peer)) {
      if (err) {
        *err = "\"" + id +
               "\" is not a Bluetooth address. Over the dongle a camera is"
               " named by its address, not by the identifier CoreBluetooth"
               " uses; run --scan-only to list them.";
      }
      return false;
    }
    // The address type is not in the printed form, so both are tried. A
    // connection request with the wrong type is simply never answered, which
    // would otherwise present as a camera that has gone away.
    uint16_t handle = 0;
    bool ok = false;
    std::string last;
    for (uint8_t type : {hci::kAddrPublic, hci::kAddrRandom}) {
      peer.type = type;
      if (link_->connect(peer, timeout / 2, &handle, &last)) {
        ok = true;
        break;
      }
    }
    if (!ok) {
      if (err) *err = last;
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      conn_ = handle;
      peer_ = peer;
      live_ = CameraView();
      subscribed_ = false;
      timecode_ = CharHandles();
      incoming_ = CharHandles();
      outgoing_ = CharHandles();
    }

    // A larger MTU is not a nicety here: a camera's incoming control stream
    // routinely exceeds the 23-byte default, and the remainder would have to
    // be fetched a blob at a time.
    std::vector<uint8_t> rsp;
    if (link_->att_request(handle, att::exchange_mtu_request(247), &rsp, 5.0,
                           nullptr)) {
      uint16_t mtu = att::kDefaultMtu;
      if (att::parse_exchange_mtu_response(rsp, &mtu)) {
        std::lock_guard<std::mutex> lock(mu_);
        mtu_ = mtu < att::kDefaultMtu ? att::kDefaultMtu : mtu;
      }
    }

    if (!discover(err)) {
      disconnect();
      return false;
    }
    return true;
  }

  void disconnect() override {
    uint16_t handle;
    {
      std::lock_guard<std::mutex> lock(mu_);
      handle = conn_;
      conn_ = 0;
      subscribed_ = false;
    }
    if (link_ && handle) link_->disconnect(handle);
  }

  bool connected() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return conn_ != 0;
  }

  bool subscribe(double timeout, std::string* err) override {
    uint16_t handle;
    {
      std::lock_guard<std::mutex> lock(mu_);
      handle = conn_;
      if (!handle) {
        if (err) *err = "not connected";
        return false;
      }
      if (subscribed_) return true;  // once per connection
    }

    // The Timecode and Incoming Control characteristics are encrypted, so the
    // subscription itself is what first demands a paired link.
    for (const CharHandles* ch : {&timecode_, &incoming_}) {
      if (!ch->found() || !ch->cccd) continue;
      if (!write_cccd(handle, ch->cccd, timeout, err)) return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    subscribed_ = true;
    return true;
  }

  bool write_control(const std::vector<uint8_t>& packet, double timeout,
                     std::string* err) override {
    uint16_t handle;
    {
      std::lock_guard<std::mutex> lock(mu_);
      handle = conn_;
    }
    if (!handle) {
      if (err) *err = "not connected";
      return false;
    }
    if (!outgoing_.found()) {
      if (err) *err = "the camera has no Outgoing Camera Control characteristic";
      return false;
    }

    std::vector<uint8_t> rsp;
    std::vector<uint8_t> req = att::write_request(outgoing_.value, packet);
    if (!link_->att_request(handle, req, &rsp, timeout, err)) return false;

    att::ErrorResponse e;
    if (att::parse_error_response(rsp, &e)) {
      if (needs_encryption(e.error)) {
        if (!ensure_encrypted(handle, timeout, err)) return false;
        if (!link_->att_request(handle, req, &rsp, timeout, err)) return false;
        if (att::parse_error_response(rsp, &e)) {
          if (err) *err = std::string("the camera refused the write: ") +
                          att::error_name(e.error);
          return false;
        }
      } else {
        if (err) {
          *err = std::string("the camera refused the write: ") +
                 att::error_name(e.error);
        }
        return false;
      }
    }
    return true;
  }

  CameraView view() override {
    std::lock_guard<std::mutex> lock(mu_);
    return live_;
  }

  void forget_timecode() override {
    std::lock_guard<std::mutex> lock(mu_);
    live_.has_timecode = false;
  }

  CameraView await_state(double seconds) override {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, std::chrono::milliseconds(
                           static_cast<long long>(seconds * 1000.0)),
                 [this] { return live_.has_timecode && live_.has_transport; });
    return live_;
  }

 private:
  static bool needs_encryption(uint8_t att_error) {
    return att_error == att::kInsufficientAuthentication ||
           att_error == att::kInsufficientEncryption ||
           att_error == att::kInsufficientKeySize;
  }

  bool write_cccd(uint16_t handle, uint16_t cccd, double timeout,
                  std::string* err) {
    std::vector<uint8_t> value = {0x01, 0x00};  // notifications on
    std::vector<uint8_t> req = att::write_request(cccd, value);
    std::vector<uint8_t> rsp;
    if (!link_->att_request(handle, req, &rsp, timeout, err)) return false;

    att::ErrorResponse e;
    if (!att::parse_error_response(rsp, &e)) return true;
    if (!needs_encryption(e.error)) {
      if (err) {
        *err = std::string("subscribing failed: ") + att::error_name(e.error);
      }
      return false;
    }
    // The expected path on a fresh camera: it wants an encrypted link before
    // it will let anything subscribe.
    if (!ensure_encrypted(handle, timeout, err)) return false;
    if (!link_->att_request(handle, req, &rsp, timeout, err)) return false;
    if (att::parse_error_response(rsp, &e)) {
      if (err) {
        *err = std::string("subscribing failed even after pairing: ") +
               att::error_name(e.error);
      }
      return false;
    }
    return true;
  }

  // Pair, then turn on encryption. Runs at most once per connection.
  bool ensure_encrypted(uint16_t handle, double timeout, std::string* err) {
    if (link_->encrypted(handle)) return true;

    hci::Address local = link_->local_address();
    smp::Initiator::Config cfg;
    cfg.local = local;
    {
      std::lock_guard<std::mutex> lock(mu_);
      cfg.remote = peer_;
    }
    // We have a keyboard of sorts and no display, which is what makes the
    // camera the one that shows the number.
    cfg.io_capability = smp::kKeyboardOnly;
    cfg.want_mitm = true;
    cfg.want_bonding = true;

    {
      std::lock_guard<std::mutex> lock(mu_);
      pairing_.reset(new smp::Initiator(cfg));
      pairing_->set_passkey_provider(
          [](uint32_t* out) { return ask_passkey(out); });
      pairing_done_ = false;
      pairing_failed_ = false;
      pairing_error_.clear();
    }

    std::vector<uint8_t> first = pairing_->begin();
    if (!link_->send_smp(handle, first, err)) return false;

    {
      std::unique_lock<std::mutex> lock(mu_);
      if (!cv_.wait_for(lock,
                        std::chrono::milliseconds(
                            static_cast<long long>((timeout + 30.0) * 1000.0)),
                        [this] { return pairing_done_ || pairing_failed_; })) {
        if (err) *err = "the camera never finished pairing";
        return false;
      }
      if (pairing_failed_) {
        if (err) *err = pairing_error_;
        return false;
      }
    }

    // Legacy pairing's first encryption uses a zero EDIV and Rand: the key is
    // the short term key both sides just derived, not a stored one.
    std::array<uint8_t, 16> stk{};
    {
      std::lock_guard<std::mutex> lock(mu_);
      const crypto::Block& b = pairing_->stk();
      std::copy(b.begin(), b.end(), stk.begin());
    }
    std::array<uint8_t, 8> rand{};
    return link_->start_encryption(handle, stk, 0, rand, 10.0, err);
  }

  // Ask for the six-digit number the camera is displaying.
  static bool ask_passkey(uint32_t* out) {
    const RadioOptions& opts = radio_options();
    if (opts.passkey >= 0) {
      *out = static_cast<uint32_t>(opts.passkey);
      return true;
    }
    if (!opts.prompt_for_passkey || !isatty(STDIN_FILENO)) {
      std::fprintf(stderr,
                   "octomancer: the camera is showing a passkey and there is"
                   " nobody to ask.\n  Supply it with --passkey NNNNNN or"
                   " OCTOMANCER_PASSKEY.\n");
      return false;
    }
    std::fprintf(stderr, "Passkey shown on the camera: ");
    std::fflush(stderr);
    char buf[32];
    if (!std::fgets(buf, sizeof buf, stdin)) return false;
    char* end = nullptr;
    long n = std::strtol(buf, &end, 10);
    if (end == buf || n < 0 || n > 999999) {
      std::fprintf(stderr, "octomancer: that is not a six-digit passkey\n");
      return false;
    }
    *out = static_cast<uint32_t>(n);
    return true;
  }

  bool discover(std::string* err) {
    uint16_t handle;
    {
      std::lock_guard<std::mutex> lock(mu_);
      handle = conn_;
    }
    hci::Uuid service = hci::uuid_const(bmd::kServiceCamera);

    // Find the camera control service by walking the primary services. Read By
    // Group Type returns one UUID width per response, so this asks repeatedly
    // from just past the last handle it saw.
    uint16_t start = 0x0001;
    uint16_t svc_start = 0, svc_end = 0;
    for (int round = 0; round < 16 && start != 0; ++round) {
      std::vector<uint8_t> rsp;
      if (!link_->att_request(
              handle,
              att::read_by_group_type_request(
                  start, 0xffff, hci::uuid_from_16(att::kUuidPrimaryService)),
              &rsp, 10.0, err)) {
        return false;
      }
      std::vector<att::ServiceRange> svcs;
      if (!att::parse_read_by_group_type_response(rsp, &svcs) || svcs.empty()) {
        break;  // an Error Response here means there are no more services
      }
      for (const att::ServiceRange& s : svcs) {
        if (s.uuid == service) {
          svc_start = s.start;
          svc_end = s.end;
        }
        start = s.end == 0xffff ? 0 : static_cast<uint16_t>(s.end + 1);
      }
      if (svc_start) break;
    }

    if (!svc_start) {
      if (err) {
        *err =
            "this device does not have the Blackmagic camera control service;"
            " it is not a camera";
      }
      return false;
    }

    // Then the characteristics inside it.
    hci::Uuid u_outgoing = hci::uuid_const(bmd::kCharOutgoingControl);
    hci::Uuid u_incoming = hci::uuid_const(bmd::kCharIncomingControl);
    hci::Uuid u_timecode = hci::uuid_const(bmd::kCharTimecode);

    std::vector<att::CharDecl> all;
    uint16_t cursor = svc_start;
    for (int round = 0; round < 24 && cursor <= svc_end; ++round) {
      std::vector<uint8_t> rsp;
      if (!link_->att_request(
              handle,
              att::read_by_type_request(
                  cursor, svc_end, hci::uuid_from_16(att::kUuidCharacteristic)),
              &rsp, 10.0, err)) {
        return false;
      }
      std::vector<att::CharDecl> chars;
      if (!att::parse_read_by_type_response_chars(rsp, &chars) ||
          chars.empty()) {
        break;
      }
      for (const att::CharDecl& c : chars) {
        all.push_back(c);
        cursor = static_cast<uint16_t>(c.value_handle + 1);
      }
    }

    for (size_t i = 0; i < all.size(); ++i) {
      CharHandles* target = nullptr;
      if (all[i].uuid == u_outgoing) target = &outgoing_;
      else if (all[i].uuid == u_incoming) target = &incoming_;
      else if (all[i].uuid == u_timecode) target = &timecode_;
      if (!target) continue;
      target->value = all[i].value_handle;
      target->properties = all[i].properties;
      // The CCCD lives between this characteristic's value and the next
      // characteristic's declaration.
      uint16_t search_end =
          i + 1 < all.size() ? static_cast<uint16_t>(all[i + 1].handle - 1)
                             : svc_end;
      if (all[i].properties & (att::kPropNotify | att::kPropIndicate)) {
        target->cccd = find_cccd(handle, all[i].value_handle, search_end);
      }
    }

    if (!outgoing_.found() || !timecode_.found()) {
      if (err) {
        *err =
            "the camera control service is missing the characteristics this"
            " program needs";
      }
      return false;
    }
    return true;
  }

  uint16_t find_cccd(uint16_t handle, uint16_t value_handle, uint16_t end) {
    uint16_t cursor = static_cast<uint16_t>(value_handle + 1);
    for (int round = 0; round < 8 && cursor <= end; ++round) {
      std::vector<uint8_t> rsp;
      if (!link_->att_request(handle,
                              att::find_information_request(cursor, end), &rsp,
                              10.0, nullptr)) {
        return 0;
      }
      std::vector<att::HandleUuid> descs;
      if (!att::parse_find_information_response(rsp, &descs) || descs.empty()) {
        return 0;
      }
      for (const att::HandleUuid& d : descs) {
        if (d.uuid == hci::uuid_from_16(att::kUuidClientCharConfig)) {
          return d.handle;
        }
        cursor = static_cast<uint16_t>(d.handle + 1);
      }
    }
    return 0;
  }

  void on_att(uint16_t conn, const std::vector<uint8_t>& pdu) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (conn != conn_) return;
    }
    att::Notification n;
    if (!att::parse_notification(pdu, &n)) return;
    if (n.wants_confirmation) {
      link_->send_att(conn, att::handle_value_confirmation(), nullptr);
    }

    if (n.handle == timecode_.value) {
      bmd::Timecode tc;
      if (!bmd::parse_timecode(n.value.data(), n.value.size(), &tc)) return;
      {
        std::lock_guard<std::mutex> lock(mu_);
        live_.has_timecode = true;
        live_.timecode = tc;
        live_.timecode_mono = mono_now();
      }
      cv_.notify_all();
      return;
    }

    if (n.handle == incoming_.value) {
      std::vector<bmd::Value> values;
      for (const bmd::Message& msg :
           bmd::parse_stream(n.value.data(), n.value.size())) {
        bmd::Value v;
        if (bmd::decode_value(msg, &v)) values.push_back(std::move(v));
      }
      if (values.empty()) return;
      {
        std::lock_guard<std::mutex> lock(mu_);
        for (const bmd::Value& v : values) {
          live_.state[{v.group, v.param}] = v;
          if (v.group == bmd::kGroupOutput &&
              v.param == bmd::kParamTimecodeSource && !v.ints.empty()) {
            live_.has_timecode_source = true;
            live_.timecode_source = v.ints[0];
          }
          if (v.group == bmd::kGroupMedia && v.param == bmd::kParamTransport &&
              !v.ints.empty()) {
            live_.has_transport = true;
            live_.transport = v.ints[0];
          }
          if (v.group == bmd::kGroupVideo && v.param == bmd::kParamFrameRate &&
              !v.ints.empty() && v.ints[0] > 0) {
            live_.has_fps = true;
            live_.fps = static_cast<int>(v.ints[0]);
          }
        }
      }
      cv_.notify_all();
    }
  }

  void on_smp(uint16_t conn, const std::vector<uint8_t>& pdu) {
    std::vector<uint8_t> reply;
    std::string error;
    bool ok;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!pairing_ || conn != conn_) return;
      ok = pairing_->handle(pdu, &reply, &error);
      if (!ok) {
        pairing_failed_ = true;
        pairing_error_ = error;
      } else if (pairing_->state() ==
                 smp::Initiator::State::kReadyToEncrypt) {
        pairing_done_ = true;
      }
    }
    if (!reply.empty()) link_->send_smp(conn, reply, nullptr);
    cv_.notify_all();
  }

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::unique_ptr<hci::Link> link_;
  uint16_t conn_ = 0;
  hci::Address peer_;
  uint16_t mtu_ = att::kDefaultMtu;
  bool subscribed_ = false;

  CharHandles outgoing_;
  CharHandles incoming_;
  CharHandles timecode_;

  CameraView live_;

  std::unique_ptr<smp::Initiator> pairing_;
  bool pairing_done_ = false;
  bool pairing_failed_ = false;
  std::string pairing_error_;
};

}  // namespace

std::unique_ptr<CameraLink> make_hci_camera_link() {
  std::unique_ptr<HciCamera> link(new HciCamera());
  std::string err;
  if (!link->start(&err)) {
    // Matching make_corebluetooth_camera_link: a link that cannot be brought
    // up is a null pointer, and the caller reports "no radio".
    std::fprintf(stderr, "octomancer: dongle: %s\n", err.c_str());
    return nullptr;
  }
  return link;
}

}  // namespace octo
