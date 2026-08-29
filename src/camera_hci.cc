#include "camhci.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bmd.h"
#include "crypto.h"
#include "radio.h"
#include "timeutil.h"

namespace octo {

HciCamera::HciCamera(Loop* loop) : loop_(loop) {}

HciCamera::~HciCamera() {
  if (!loop_) return;
  loop_->cancel(pairing_timer_);
  loop_->cancel(scan_timer_);
}

std::unique_ptr<HciCamera> HciCamera::open(Loop* loop, DoneHandler on_ready,
                                           std::string* err) {
  std::unique_ptr<HciCamera> cam(new HciCamera(loop));
  cam->on_ready_ = std::move(on_ready);

  hci::Link::Options opts;
  opts.device = radio_options().device;
  opts.trace = radio_options().trace;

  HciCamera* raw = cam.get();
  cam->link_ = hci::Link::open(
      loop, opts,
      [raw](bool ok, const std::string& why) { raw->on_ready(ok, why); }, err);
  if (!cam->link_) return nullptr;

  cam->link_->set_connection_handlers(
      nullptr, [raw](uint16_t handle, uint8_t) { raw->on_gone(handle); });
  cam->link_->set_att_handler(
      [raw](uint16_t conn, const std::vector<uint8_t>& pdu) {
        raw->on_att(conn, pdu);
      });
  cam->link_->set_smp_handler(
      [raw](uint16_t conn, const std::vector<uint8_t>& pdu) {
        raw->on_smp(conn, pdu);
      });
  return cam;
}

void HciCamera::on_ready(bool ok, const std::string& err) {
  DoneHandler done = std::move(on_ready_);
  on_ready_ = nullptr;
  if (done) done(ok, err);
}

void HciCamera::set_passkey_provider(PasskeyProvider provider) {
  passkey_ = std::move(provider);
}

void HciCamera::set_view_handler(ViewHandler on_change) {
  on_view_ = std::move(on_change);
}

void HciCamera::set_disconnect_handler(std::function<void()> on_gone) {
  on_gone_ = std::move(on_gone);
}

const CameraView& HciCamera::view() const { return live_; }

void HciCamera::forget_timecode() { live_.has_timecode = false; }

bool HciCamera::connected() const { return conn_ != 0; }

bool HciCamera::subscribed() const { return conn_ != 0 && subscribed_; }

void HciCamera::note(const CameraView& v) {
  if (on_view_) on_view_(v);
}

// -------------------------------------------------------------- scanning

void HciCamera::scan(double seconds, const std::string& name_hint,
                     bool want_all, ScanHandler done) {
  // Held by the scan and by the timer that ends it, and by nothing else once
  // both have finished with it.
  struct Collected {
    std::map<std::string, CameraDevice> seen;
    std::set<std::string> tentacles;
  };
  auto state = std::make_shared<Collected>();

  hci::Uuid camera_service = hci::uuid_const(bmd::kServiceCamera);
  hci::Uuid fdac = hci::uuid_from_16(0xfdac);

  // An active scan, so a device that keeps its name in the scan response still
  // gets one. Unlike the passive Tentacle scan, here the name is worth
  // provoking: it is what a person recognises the camera by.
  link_->start_scan(
      /*active=*/true, /*filter_duplicates=*/false,
      [state, camera_service, fdac](const hci::AdvReport& r) {
        hci::AdInfo info = hci::summarise_ad(hci::parse_ad(r.data));
        std::string id = hci::address_to_string(r.addr);

        for (const auto& sd : info.service_data) {
          if (sd.first == fdac) state->tentacles.insert(id);
        }

        bool by_uuid = false;
        for (const hci::Uuid& u : info.services) {
          if (u == camera_service) {
            by_uuid = true;
            break;
          }
        }

        CameraDevice& dev = state->seen[id];
        dev.id = id;
        if (!info.name.empty()) dev.name = info.name;
        dev.rssi = r.rssi;
        // Once proven, always proven: a later advertisement from the same
        // device may be a scan response with no service list in it, and
        // demoting the device on that basis would lose it.
        dev.by_service_uuid = dev.by_service_uuid || by_uuid;
      },
      [this, state, seconds, name_hint, want_all, done](
          bool ok, const std::string&) {
        if (!ok) {
          if (done) done(ScanResult());
          return;
        }
        scan_timer_ = loop_->after(seconds, [this, state, name_hint, want_all,
                                             done] {
          scan_timer_ = kNoTimer;
          link_->stop_scan(nullptr);

          ScanResult result;
          result.total = static_cast<int>(state->seen.size());
          result.tentacles = static_cast<int>(state->tentacles.size());

          std::string hint_lower = name_hint;
          std::transform(hint_lower.begin(), hint_lower.end(),
                         hint_lower.begin(), ::tolower);

          for (const auto& kv : state->seen) {
            const CameraDevice& dev = kv.second;
            if (want_all) result.all.push_back(dev);
            bool matches = dev.by_service_uuid;
            if (!matches && !hint_lower.empty() && !dev.name.empty()) {
              std::string name_lower = dev.name;
              std::transform(name_lower.begin(), name_lower.end(),
                             name_lower.begin(), ::tolower);
              matches = name_lower.find(hint_lower) != std::string::npos;
            }
            if (matches) result.cameras.push_back(dev);
          }

          // Proof before guesswork, then signal strength. A name match is only
          // ever a guess -- there is a Tentacle on this bench called "BMPCC".
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
          if (done) done(result);
        });
      });
}

// ------------------------------------------------------------ connecting

void HciCamera::connect(const std::string& id, double timeout,
                        DoneHandler done) {
  hci::Address peer;
  if (!hci::address_from_string(id, &peer)) {
    if (done) {
      const std::string why =
          "\"" + id +
          "\" is not a Bluetooth address. Over the dongle a camera is named by"
          " its address, not by the identifier CoreBluetooth uses; run"
          " --scan-only to list them.";
      loop_->after(0.0, [done, why] { done(false, why); });
    }
    return;
  }
  disconnect();

  // The address type is not in the printed form, so both are tried. A
  // connection request with the wrong type is simply never answered, which
  // would otherwise present as a camera that has gone away.
  try_connect(peer, 0, timeout, std::move(done));
}

void HciCamera::try_connect(const hci::Address& peer, size_t type_index,
                            double timeout, DoneHandler done) {
  static const uint8_t kTypes[2] = {hci::kAddrPublic, hci::kAddrRandom};
  if (type_index >= 2) return;

  hci::Address target = peer;
  target.type = kTypes[type_index];
  link_->connect(
      target, timeout / 2.0,
      [this, peer, target, type_index, timeout, done](
          bool ok, uint16_t handle, const std::string& err) {
        if (!ok) {
          if (type_index + 1 < 2) {
            try_connect(peer, type_index + 1, timeout, done);
          } else if (done) {
            done(false, err);
          }
          return;
        }
        conn_ = handle;
        peer_ = target;
        live_ = CameraView();
        subscribed_ = false;
        timecode_ = CharHandles();
        incoming_ = CharHandles();
        outgoing_ = CharHandles();
        mtu_ = att::kDefaultMtu;

        // A larger MTU is not a nicety here: a camera's incoming control
        // stream routinely exceeds the 23-byte default, and the remainder
        // would have to be fetched a blob at a time. A camera that refuses is
        // still usable, so this is best-effort.
        link_->att_request(
            handle, att::exchange_mtu_request(247), 5.0,
            [this, done](bool got, const std::vector<uint8_t>& rsp,
                         const std::string&) {
              uint16_t mtu = att::kDefaultMtu;
              if (got && att::parse_exchange_mtu_response(rsp, &mtu)) {
                mtu_ = mtu < att::kDefaultMtu ? att::kDefaultMtu : mtu;
              }
              discover([this, done](bool ok_disc, const std::string& why) {
                if (!ok_disc) disconnect();
                if (done) done(ok_disc, why);
              });
            });
      });
}

void HciCamera::disconnect() {
  uint16_t handle = conn_;
  conn_ = 0;
  subscribed_ = false;
  finish_pairing(false, "the connection was dropped");
  if (link_ && handle) link_->disconnect(handle);
}

void HciCamera::on_gone(uint16_t handle) {
  if (handle != conn_) return;
  conn_ = 0;
  subscribed_ = false;
  finish_pairing(false, "the camera disconnected during pairing");
  if (on_gone_) on_gone_();
}

// ------------------------------------------------------------- discovery

void HciCamera::discover(DoneHandler done) {
  svc_start_ = 0;
  svc_end_ = 0;
  chars_.clear();
  walk_services(0x0001, 0, std::move(done));
}

// Find the camera control service by walking the primary services. Read By
// Group Type returns one UUID width per response, so this asks repeatedly from
// just past the last handle it saw.
void HciCamera::walk_services(uint16_t start, int round, DoneHandler done) {
  static const char kNotACamera[] =
      "this device does not have the Blackmagic camera control service; it is"
      " not a camera";
  if (round >= 16 || start == 0) {
    if (done) done(false, kNotACamera);
    return;
  }
  const uint16_t conn = conn_;
  link_->att_request(
      conn,
      att::read_by_group_type_request(
          start, 0xffff, hci::uuid_from_16(att::kUuidPrimaryService)),
      10.0,
      [this, round, done](bool ok, const std::vector<uint8_t>& rsp,
                          const std::string& err) {
        if (!ok) {
          if (done) done(false, err);
          return;
        }
        std::vector<att::ServiceRange> svcs;
        if (!att::parse_read_by_group_type_response(rsp, &svcs) ||
            svcs.empty()) {
          // An Error Response here means there are no more services.
          if (done) done(false, kNotACamera);
          return;
        }
        hci::Uuid service = hci::uuid_const(bmd::kServiceCamera);
        uint16_t next = 0;
        for (const att::ServiceRange& s : svcs) {
          if (s.uuid == service) {
            svc_start_ = s.start;
            svc_end_ = s.end;
          }
          next = s.end == 0xffff ? 0 : static_cast<uint16_t>(s.end + 1);
        }
        if (svc_start_) {
          walk_chars(svc_start_, 0, done);
          return;
        }
        walk_services(next, round + 1, done);
      });
}

void HciCamera::walk_chars(uint16_t cursor, int round, DoneHandler done) {
  if (round >= 24 || cursor > svc_end_) {
    walk_cccds(0, std::move(done));
    return;
  }
  const uint16_t conn = conn_;
  link_->att_request(
      conn,
      att::read_by_type_request(cursor, svc_end_,
                                hci::uuid_from_16(att::kUuidCharacteristic)),
      10.0,
      [this, round, done](bool ok, const std::vector<uint8_t>& rsp,
                          const std::string& err) {
        if (!ok) {
          if (done) done(false, err);
          return;
        }
        std::vector<att::CharDecl> chars;
        if (!att::parse_read_by_type_response_chars(rsp, &chars) ||
            chars.empty()) {
          walk_cccds(0, done);
          return;
        }
        uint16_t next = 0;
        for (const att::CharDecl& c : chars) {
          chars_.push_back(c);
          next = static_cast<uint16_t>(c.value_handle + 1);
        }
        walk_chars(next, round + 1, done);
      });
}

// Then the descriptors. Only the characteristics this program subscribes to
// need one, so this walks the collected declarations rather than the whole
// table.
void HciCamera::walk_cccds(size_t index, DoneHandler done) {
  while (index < chars_.size()) {
    const att::CharDecl& c = chars_[index];
    bool wanted = c.uuid == hci::uuid_const(bmd::kCharIncomingControl) ||
                  c.uuid == hci::uuid_const(bmd::kCharTimecode);
    if (wanted && (c.properties & (att::kPropNotify | att::kPropIndicate))) {
      // The CCCD lives between this characteristic's value and the next
      // characteristic's declaration.
      uint16_t end = index + 1 < chars_.size()
                         ? static_cast<uint16_t>(chars_[index + 1].handle - 1)
                         : svc_end_;
      find_cccd(index, static_cast<uint16_t>(c.value_handle + 1), end, 0,
                std::move(done));
      return;
    }
    ++index;
  }
  finish_discovery(std::move(done));
}

void HciCamera::find_cccd(size_t index, uint16_t cursor, uint16_t end,
                          int round, DoneHandler done) {
  if (round >= 8 || cursor > end) {
    walk_cccds(index + 1, std::move(done));
    return;
  }
  const uint16_t conn = conn_;
  link_->att_request(
      conn, att::find_information_request(cursor, end), 10.0,
      [this, index, end, round, done](bool ok, const std::vector<uint8_t>& rsp,
                                      const std::string&) {
        std::vector<att::HandleUuid> descs;
        if (!ok || !att::parse_find_information_response(rsp, &descs) ||
            descs.empty()) {
          // No descriptor found is not fatal here: a characteristic without a
          // CCCD simply cannot be subscribed to, and subscribe() says so.
          walk_cccds(index + 1, done);
          return;
        }
        uint16_t next = 0;
        uint16_t found = 0;
        for (const att::HandleUuid& d : descs) {
          if (d.uuid == hci::uuid_from_16(att::kUuidClientCharConfig)) {
            found = d.handle;
            break;
          }
          next = static_cast<uint16_t>(d.handle + 1);
        }
        if (found) {
          chars_cccd_[index] = found;
          walk_cccds(index + 1, done);
          return;
        }
        find_cccd(index, next, end, round + 1, done);
      });
}

void HciCamera::finish_discovery(DoneHandler done) {
  hci::Uuid u_outgoing = hci::uuid_const(bmd::kCharOutgoingControl);
  hci::Uuid u_incoming = hci::uuid_const(bmd::kCharIncomingControl);
  hci::Uuid u_timecode = hci::uuid_const(bmd::kCharTimecode);

  for (size_t i = 0; i < chars_.size(); ++i) {
    CharHandles* target = nullptr;
    if (chars_[i].uuid == u_outgoing) target = &outgoing_;
    else if (chars_[i].uuid == u_incoming) target = &incoming_;
    else if (chars_[i].uuid == u_timecode) target = &timecode_;
    if (!target) continue;
    target->value = chars_[i].value_handle;
    target->properties = chars_[i].properties;
    auto it = chars_cccd_.find(i);
    if (it != chars_cccd_.end()) target->cccd = it->second;
  }
  chars_cccd_.clear();

  if (!outgoing_.found() || !timecode_.found()) {
    if (done) {
      done(false,
           "the camera control service is missing the characteristics this"
           " program needs");
    }
    return;
  }
  if (done) done(true, std::string());
}

// ------------------------------------------------------------ subscribing

void HciCamera::subscribe(double timeout, DoneHandler done) {
  if (!conn_) {
    if (done) loop_->after(0.0, [done] { done(false, "not connected"); });
    return;
  }
  if (subscribed_) {
    if (done) loop_->after(0.0, [done] { done(true, std::string()); });
    return;
  }
  subscribe_next(0, timeout, std::move(done));
}

void HciCamera::subscribe_next(size_t index, double timeout,
                               DoneHandler done) {
  const CharHandles* order[2] = {&timecode_, &incoming_};
  while (index < 2 && (!order[index]->found() || !order[index]->cccd)) {
    ++index;
  }
  if (index >= 2) {
    subscribed_ = true;
    if (done) done(true, std::string());
    return;
  }
  const uint16_t cccd = order[index]->cccd;
  write_cccd(cccd, timeout,
             [this, index, timeout, done](bool ok, const std::string& err) {
               if (!ok) {
                 if (done) done(false, err);
                 return;
               }
               subscribe_next(index + 1, timeout, done);
             });
}

void HciCamera::write_cccd(uint16_t cccd, double timeout, DoneHandler done) {
  const std::vector<uint8_t> value = {0x01, 0x00};  // notifications on
  const std::vector<uint8_t> req = att::write_request(cccd, value);
  const uint16_t conn = conn_;

  link_->att_request(
      conn, req, timeout,
      [this, req, timeout, done](bool ok, const std::vector<uint8_t>& rsp,
                                 const std::string& err) {
        if (!ok) {
          if (done) done(false, err);
          return;
        }
        att::ErrorResponse e;
        if (!att::parse_error_response(rsp, &e)) {
          if (done) done(true, std::string());
          return;
        }
        if (!needs_encryption(e.error)) {
          if (done) {
            done(false, std::string("subscribing failed: ") +
                            att::error_name(e.error));
          }
          return;
        }
        // The expected path on a fresh camera: it wants an encrypted link
        // before it will let anything subscribe.
        ensure_encrypted(timeout, [this, req, timeout, done](
                                      bool paired, const std::string& why) {
          if (!paired) {
            if (done) done(false, why);
            return;
          }
          link_->att_request(
              conn_, req, timeout,
              [done](bool again, const std::vector<uint8_t>& rsp2,
                     const std::string& err2) {
                if (!again) {
                  if (done) done(false, err2);
                  return;
                }
                att::ErrorResponse e2;
                if (att::parse_error_response(rsp2, &e2)) {
                  if (done) {
                    done(false,
                         std::string("subscribing failed even after pairing: ") +
                             att::error_name(e2.error));
                  }
                  return;
                }
                if (done) done(true, std::string());
              });
        });
      });
}

// --------------------------------------------------------------- writing

void HciCamera::write_control(const std::vector<uint8_t>& packet,
                              double timeout, DoneHandler done) {
  if (!conn_) {
    if (done) loop_->after(0.0, [done] { done(false, "not connected"); });
    return;
  }
  if (!outgoing_.found()) {
    if (done) {
      loop_->after(0.0, [done] {
        done(false,
             "the camera has no Outgoing Camera Control characteristic");
      });
    }
    return;
  }

  const std::vector<uint8_t> req = att::write_request(outgoing_.value, packet);
  link_->att_request(
      conn_, req, timeout,
      [this, req, timeout, done](bool ok, const std::vector<uint8_t>& rsp,
                                 const std::string& err) {
        if (!ok) {
          if (done) done(false, err);
          return;
        }
        att::ErrorResponse e;
        if (!att::parse_error_response(rsp, &e)) {
          if (done) done(true, std::string());
          return;
        }
        if (!needs_encryption(e.error)) {
          if (done) {
            done(false, std::string("the camera refused the write: ") +
                            att::error_name(e.error));
          }
          return;
        }
        ensure_encrypted(timeout, [this, req, timeout, done](
                                      bool paired, const std::string& why) {
          if (!paired) {
            if (done) done(false, why);
            return;
          }
          link_->att_request(
              conn_, req, timeout,
              [done](bool again, const std::vector<uint8_t>& rsp2,
                     const std::string& err2) {
                if (!again) {
                  if (done) done(false, err2);
                  return;
                }
                att::ErrorResponse e2;
                if (att::parse_error_response(rsp2, &e2)) {
                  if (done) {
                    done(false, std::string("the camera refused the write: ") +
                                    att::error_name(e2.error));
                  }
                  return;
                }
                if (done) done(true, std::string());
              });
        });
      });
}

// --------------------------------------------------------------- pairing

bool HciCamera::needs_encryption(uint8_t att_error) {
  return att_error == att::kInsufficientAuthentication ||
         att_error == att::kInsufficientEncryption ||
         att_error == att::kInsufficientKeySize;
}

void HciCamera::ensure_encrypted(double timeout, DoneHandler done) {
  if (!conn_) {
    if (done) loop_->after(0.0, [done] { done(false, "not connected"); });
    return;
  }
  if (link_->encrypted(conn_)) {
    if (done) loop_->after(0.0, [done] { done(true, std::string()); });
    return;
  }
  if (pairing_) {
    if (done) {
      loop_->after(0.0,
                   [done] { done(false, "pairing is already in progress"); });
    }
    return;
  }

  smp::Initiator::Config cfg;
  cfg.local = link_->local_address();
  cfg.remote = peer_;
  // We have a keyboard of sorts and no display, which is what makes the camera
  // the one that shows the number.
  cfg.io_capability = smp::kKeyboardOnly;
  cfg.want_mitm = true;
  cfg.want_bonding = true;

  pairing_.reset(new smp::Initiator(cfg));
  PasskeyProvider provider = passkey_;
  pairing_->set_passkey_provider([provider](uint32_t* out) {
    return provider ? provider(out) : false;
  });
  pairing_done_ = std::move(done);
  pairing_timeout_ = timeout;

  std::string err;
  if (!link_->send_smp(conn_, pairing_->begin(), &err)) {
    finish_pairing(false, err);
    return;
  }
  // A person has to read six digits off the back of a camera and type them, so
  // the budget here is the caller's timeout plus time for that to happen.
  pairing_timer_ = loop_->after(timeout + 30.0, [this] {
    pairing_timer_ = kNoTimer;
    finish_pairing(false, "the camera never finished pairing");
  });
}

void HciCamera::on_smp(uint16_t conn, const std::vector<uint8_t>& pdu) {
  if (!pairing_ || conn != conn_) return;

  std::vector<uint8_t> reply;
  std::string error;
  const bool ok = pairing_->handle(pdu, &reply, &error);
  if (!reply.empty()) link_->send_smp(conn, reply, nullptr);
  if (!ok) {
    finish_pairing(false, error);
    return;
  }
  if (pairing_->state() != smp::Initiator::State::kReadyToEncrypt) return;

  // Legacy pairing's first encryption uses a zero EDIV and Rand: the key is the
  // short term key both sides just derived, not a stored one.
  std::array<uint8_t, 16> stk{};
  const crypto::Block& b = pairing_->stk();
  std::copy(b.begin(), b.end(), stk.begin());
  std::array<uint8_t, 8> rand{};

  link_->start_encryption(conn, stk, 0, rand, 10.0,
                          [this](bool enc_ok, const std::string& why) {
                            finish_pairing(enc_ok, why);
                          });
}

void HciCamera::finish_pairing(bool ok, const std::string& err) {
  if (!pairing_ && !pairing_done_) return;
  loop_->cancel(pairing_timer_);
  pairing_timer_ = kNoTimer;
  pairing_.reset();
  DoneHandler done = std::move(pairing_done_);
  pairing_done_ = nullptr;
  if (done) done(ok, err);
}

// ------------------------------------------------------- what it tells us

void HciCamera::on_att(uint16_t conn, const std::vector<uint8_t>& pdu) {
  if (conn != conn_) return;
  att::Notification n;
  if (!att::parse_notification(pdu, &n)) return;
  if (n.wants_confirmation) {
    link_->send_att(conn, att::handle_value_confirmation(), nullptr);
  }

  if (n.handle == timecode_.value) {
    bmd::Timecode tc;
    if (!bmd::parse_timecode(n.value.data(), n.value.size(), &tc)) return;
    live_.has_timecode = true;
    live_.timecode = tc;
    live_.timecode_mono = mono_now();
    note(live_);
    return;
  }

  if (n.handle != incoming_.value) return;

  std::vector<bmd::Value> values;
  for (const bmd::Message& msg :
       bmd::parse_stream(n.value.data(), n.value.size())) {
    bmd::Value v;
    if (bmd::decode_value(msg, &v)) values.push_back(std::move(v));
  }
  if (values.empty()) return;

  for (const bmd::Value& v : values) {
    live_.state[{v.group, v.param}] = v;
    if (v.group == bmd::kGroupOutput && v.param == bmd::kParamTimecodeSource &&
        !v.ints.empty()) {
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
  note(live_);
}

}  // namespace octo
