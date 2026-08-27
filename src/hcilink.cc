#include "hcilink.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace octo {
namespace hci {
namespace {


std::chrono::milliseconds to_ms(double seconds) {
  if (seconds < 0) seconds = 0;
  return std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
}

// The opcode a response carries for a given request. ATT numbers responses one
// above their request, with the write command and the notifications as the
// exceptions that have no response at all.
uint8_t response_opcode(uint8_t request) {
  return static_cast<uint8_t>(request + 1);
}

}  // namespace

Link::Link() = default;

Link::~Link() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    running_ = false;
  }
  // Closing the port is what wakes the reader out of poll(). Joining first
  // would wait for a read that has no reason to return.
  if (port_) port_->close();
  if (reader_.joinable()) reader_.join();
}

std::unique_ptr<Link> Link::open(const Options& opts, std::string* err) {
  std::unique_ptr<Port> port = open_port(opts.device, err);
  if (!port) return nullptr;

  std::unique_ptr<Link> link(new Link());
  link->port_ = std::move(port);
  link->opts_ = opts;
  link->running_ = true;
  link->reader_ = std::thread(&Link::reader, link.get());

  if (!link->init(err)) return nullptr;
  return link;
}

std::string Link::port_name() const {
  return port_ ? port_->name() : std::string();
}

Address Link::local_address() const {
  std::lock_guard<std::mutex> lock(mu_);
  return local_;
}

size_t Link::max_acl_payload() const {
  std::lock_guard<std::mutex> lock(mu_);
  return acl_payload_;
}

std::string Link::describe() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::string out = port_ ? port_->name() : std::string("(no port)");
  if (!version_.empty()) out += ", " + version_;
  out += ", address " + address_to_string(local_);
  out += own_addr_type_ == kAddrRandom ? " (random static)" : " (public)";
  return out;
}

void Link::log(const char* dir, const std::vector<uint8_t>& pkt) {
  if (!opts_.trace) return;
  std::fprintf(stderr, "hci %s %s\n", dir, to_hex(pkt).c_str());
}

bool Link::write_packet(const std::vector<uint8_t>& pkt, std::string* err) {
  log("->", pkt);
  if (!port_ || !port_->write(pkt.data(), pkt.size())) {
    if (err) *err = "write to " + port_name() + " failed";
    return false;
  }
  return true;
}

bool Link::command(uint16_t opcode, const std::vector<uint8_t>& params,
                   CommandComplete* out, std::string* err) {
  std::lock_guard<std::mutex> serialise(command_mu_);
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!running_) {
      if (err) *err = "link is closed";
      return false;
    }
    cmd_pending_ = true;
    cmd_done_ = false;
    cmd_opcode_ = opcode;
    cmd_result_ = CommandComplete();
  }

  if (!write_packet(build_command(opcode, params), err)) {
    std::lock_guard<std::mutex> lock(mu_);
    cmd_pending_ = false;
    return false;
  }

  std::unique_lock<std::mutex> lock(mu_);
  if (!cv_.wait_for(lock, to_ms(opts_.command_timeout),
                    [this] { return cmd_done_ || !running_; })) {
    cmd_pending_ = false;
    if (err) {
      *err = std::string(opcode_name(opcode)) + ": no answer from the controller";
    }
    return false;
  }
  cmd_pending_ = false;
  if (!cmd_done_) {
    if (err) *err = "link closed while waiting for " + std::string(opcode_name(opcode));
    return false;
  }
  CommandComplete result = cmd_result_;
  lock.unlock();

  if (out) *out = result;
  if (result.status != kSuccess) {
    if (err) {
      *err = std::string(opcode_name(opcode)) + ": " + status_name(result.status);
    }
    return false;
  }
  return true;
}

bool Link::init(std::string* err) {
  CommandComplete cc;
  if (!command(kOpReset, {}, &cc, err)) return false;

  // Ask for the events this host actually handles. The controller's default
  // mask leaves several of them off, and an event that is masked out is not an
  // error anywhere -- it simply never arrives.
  std::vector<uint8_t> mask(8, 0xff);
  if (!command(kOpSetEventMask, mask, &cc, err)) return false;
  // LE event mask: connection complete, advertising report, connection update,
  // read remote features, long term key request, data length change, the P-256
  // pair, and enhanced connection complete.
  std::vector<uint8_t> le_mask = {0xff, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  if (!command(kOpLeSetEventMask, le_mask, &cc, err)) return false;

  if (command(kOpReadLocalVersion, {}, &cc, nullptr) && cc.params.size() >= 9) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "HCI version %u, LMP subversion 0x%04x",
                  cc.params[1],
                  static_cast<unsigned>(cc.params[7] | (cc.params[8] << 8)));
    std::lock_guard<std::mutex> lock(mu_);
    version_ = buf;
  }

  if (command(kOpLeReadBufferSize, {}, &cc, nullptr) && cc.params.size() >= 4) {
    size_t payload = static_cast<size_t>(cc.params[1] | (cc.params[2] << 8));
    int total = cc.params[3];
    std::lock_guard<std::mutex> lock(mu_);
    // A controller that reports zero here is telling us to use the BR/EDR
    // buffer pool. Nothing this program does needs more than one packet in
    // flight, so the conservative floor is a correct answer rather than a
    // reason to go and read the other buffer size.
    acl_payload_ = payload ? payload : 27;
    acl_total_ = total ? total : 1;
    acl_credits_ = acl_total_;
  }

  // An address to be. The nRF52840 has no public address assigned to it, so
  // Read BD_ADDR comes back all zeros and every attempt to advertise or
  // connect from a "public" address is rejected -- with a status that says
  // nothing about addresses. Installing a random static address is the fix,
  // and it has to happen before anything else touches the air.
  Address addr;
  bool have_public = false;
  if (command(kOpReadBdAddr, {}, &cc, nullptr) && cc.params.size() >= 7) {
    for (size_t i = 0; i < 6; ++i) addr.bytes[i] = cc.params[6 - i];
    addr.type = kAddrPublic;
    for (uint8_t b : addr.bytes) {
      if (b != 0x00) have_public = true;
    }
  }

  if (!have_public) {
    Address rnd;
    if (command(kOpLeRand, {}, &cc, nullptr) && cc.params.size() >= 9) {
      for (size_t i = 0; i < 6; ++i) rnd.bytes[i] = cc.params[1 + i];
    } else {
      // No randomness from the controller. A fixed fallback is still a valid
      // static address; it only has to be constant for as long as the device
      // is up, and being predictable costs nothing here.
      const uint8_t seed[6] = {0xc0, 0x0c, 0x70, 0x11, 0x4a, 0x11};
      std::memcpy(rnd.bytes.data(), seed, 6);
    }
    // The top two bits must be 11 for a static address. Without them the
    // controller rejects the address as malformed.
    rnd.bytes[0] = static_cast<uint8_t>(rnd.bytes[0] | 0xc0);
    rnd.type = kAddrRandom;

    std::vector<uint8_t> params;
    for (size_t i = 0; i < 6; ++i) params.push_back(rnd.bytes[5 - i]);
    if (!command(kOpLeSetRandomAddress, params, &cc, err)) return false;
    std::lock_guard<std::mutex> lock(mu_);
    local_ = rnd;
    own_addr_type_ = kAddrRandom;
  } else {
    std::lock_guard<std::mutex> lock(mu_);
    local_ = addr;
    own_addr_type_ = kAddrPublic;
  }
  return true;
}

// ------------------------------------------------------------ reader thread

void Link::reader() {
  std::vector<uint8_t> buf;
  uint8_t chunk[512];
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!running_) break;
    }
    int n = port_->read(chunk, sizeof chunk, 0.25);
    if (n < 0) {
      // The port died -- almost always the dongle being unplugged. Wake
      // everyone waiting rather than leaving them on their timeouts.
      std::lock_guard<std::mutex> lock(mu_);
      running_ = false;
      cv_.notify_all();
      break;
    }
    if (n == 0) continue;
    buf.insert(buf.end(), chunk, chunk + n);

    std::vector<Packet> packets;
    size_t used = parse_stream(buf.data(), buf.size(), &packets);
    buf.erase(buf.begin(), buf.begin() + used);

    for (const Packet& pkt : packets) {
      if (opts_.trace) {
        std::vector<uint8_t> whole{pkt.type};
        whole.insert(whole.end(), pkt.payload.begin(), pkt.payload.end());
        log("<-", whole);
      }
      if (pkt.type == kPacketEvent) {
        Event evt;
        if (parse_event(pkt.payload, &evt)) dispatch_event(evt);
      } else if (pkt.type == kPacketAclData) {
        dispatch_acl(pkt.payload);
      }
    }
  }
  cv_.notify_all();
}

void Link::dispatch_event(const Event& evt) {
  EventHandler on_event;
  {
    std::lock_guard<std::mutex> lock(mu_);
    on_event = on_event_;
  }
  if (on_event) on_event(evt);

  switch (evt.code) {
    case kEvtCommandComplete: {
      CommandComplete cc;
      if (!parse_command_complete(evt, &cc)) return;
      std::lock_guard<std::mutex> lock(mu_);
      // Restore the controller's command credit; a Command Complete with
      // opcode zero exists only to do that and matches no caller.
      if (cmd_pending_ && cc.opcode == cmd_opcode_) {
        cmd_result_ = cc;
        cmd_done_ = true;
        cv_.notify_all();
      }
      return;
    }

    case kEvtCommandStatus: {
      CommandStatus cs;
      if (!parse_command_status(evt, &cs)) return;
      std::lock_guard<std::mutex> lock(mu_);
      if (!cmd_pending_ || cs.opcode != cmd_opcode_) return;
      // A Command Status means the command was accepted and its result will
      // arrive later as its own event -- except when it failed outright, in
      // which case this is the whole answer.
      cmd_result_ = CommandComplete();
      cmd_result_.opcode = cs.opcode;
      cmd_result_.status = cs.status;
      cmd_done_ = true;
      cv_.notify_all();
      return;
    }

    case kEvtNumCompletedPackets: {
      // params: num handles, then (handle, count) pairs.
      if (evt.params.empty()) return;
      int freed = 0;
      size_t count = evt.params[0];
      for (size_t i = 0; i < count; ++i) {
        size_t off = 1 + i * 4;
        if (off + 4 > evt.params.size()) break;
        freed += evt.params[off + 2] | (evt.params[off + 3] << 8);
      }
      std::lock_guard<std::mutex> lock(mu_);
      acl_credits_ += freed;
      if (acl_credits_ > acl_total_) acl_credits_ = acl_total_;
      cv_.notify_all();
      return;
    }

    case kEvtEncryptionChange:
    case kEvtEncryptionKeyRefresh: {
      EncryptionChange ec;
      if (evt.code == kEvtEncryptionChange) {
        if (!parse_encryption_change(evt, &ec)) return;
      } else {
        // A key refresh carries no "enabled" byte; it only ever happens on a
        // link that is already encrypted, so success means it still is.
        if (evt.params.size() < 3) return;
        ec.status = evt.params[0];
        ec.handle = static_cast<uint16_t>(
            (evt.params[1] | (evt.params[2] << 8)) & 0x0fff);
        ec.enabled = ec.status == kSuccess ? 1 : 0;
      }
      std::lock_guard<std::mutex> lock(mu_);
      encrypted_[ec.handle] = ec.status == kSuccess && ec.enabled != 0;
      if (encrypt_pending_) {
        encrypt_status_ = ec.status;
        encrypt_enabled_ = ec.enabled;
        encrypt_done_ = true;
      }
      cv_.notify_all();
      return;
    }

    case kEvtDisconnectionComplete: {
      DisconnectionComplete dc;
      if (!parse_disconnection_complete(evt, &dc)) return;
      DisconnectedHandler handler;
      {
        std::lock_guard<std::mutex> lock(mu_);
        conns_.erase(dc.handle);
        reasm_.erase(dc.handle);
        encrypted_.erase(dc.handle);
        // Anyone blocked on a request over this connection is never going to
        // get an answer. Failing them now beats a thirty-second timeout.
        auto it = att_waits_.find(dc.handle);
        if (it != att_waits_.end() && it->second.waiting) {
          it->second.done = true;
          it->second.rsp.clear();
        }
        handler = on_disconnect_;
        cv_.notify_all();
      }
      if (handler) handler(dc.handle, dc.reason);
      return;
    }

    case kEvtLeMeta:
      break;

    default:
      return;
  }

  switch (evt.subevent) {
    case kLeAdvertisingReport: {
      std::vector<AdvReport> reports;
      if (!parse_adv_reports(evt, &reports)) return;
      AdvHandler handler;
      {
        std::lock_guard<std::mutex> lock(mu_);
        handler = on_adv_;
      }
      if (!handler) return;
      for (const AdvReport& r : reports) handler(r);
      return;
    }

    case kLeConnectionComplete:
    case kLeEnhancedConnectionComplete: {
      ConnectionComplete cc;
      if (!parse_connection_complete(evt, &cc)) return;
      ConnectedHandler handler;
      Conn conn;
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (connect_pending_) {
          connect_result_ = cc;
          connect_done_ = true;
        }
        if (cc.status == kSuccess) {
          conn.handle = cc.handle;
          conn.peer = cc.peer;
          conn.role = cc.role;
          conns_[cc.handle] = conn;
          reasm_[cc.handle] = att::Reassembler();
          // A peripheral goes back to not advertising the moment it is
          // connected. Tracking that is what stops a caller believing it is
          // still discoverable.
          if (cc.role == 1) advertising_ = false;
        }
        handler = on_connect_;
        cv_.notify_all();
      }
      if (cc.status == kSuccess && handler) handler(conn);
      return;
    }

    case kLeLongTermKeyRequest: {
      // Nothing here holds long term keys yet, and the honest answer to a
      // request for one is that we have none. Ignoring it instead leaves the
      // peer waiting for an encrypted link that never starts.
      LongTermKeyRequest req;
      if (!parse_ltk_request(evt, &req)) return;
      std::vector<uint8_t> params = {
          static_cast<uint8_t>(req.handle & 0xff),
          static_cast<uint8_t>(req.handle >> 8)};
      // Sent without waiting: this is the reader thread, and command() blocks
      // on it.
      write_packet(build_command(kOpLeLtkRequestNegReply, params), nullptr);
      return;
    }

    default:
      return;
  }
}

void Link::dispatch_acl(const std::vector<uint8_t>& payload) {
  AclHeader hdr;
  std::vector<uint8_t> data;
  if (!parse_acl(payload, &hdr, &data)) return;

  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> frames;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = reasm_.find(hdr.handle);
    if (it == reasm_.end()) return;  // a connection we do not know about
    it->second.push(hdr.pb_flag, data.data(), data.size(), &frames);
  }

  for (const auto& frame : frames) {
    uint16_t cid = frame.first;
    const std::vector<uint8_t>& pdu = frame.second;

    L2capHandler on_l2cap;
    {
      std::lock_guard<std::mutex> lock(mu_);
      on_l2cap = on_l2cap_;
    }
    if (on_l2cap) on_l2cap(hdr.handle, cid, pdu);

    if (cid == att::kCidSmp) {
      SmpHandler on_smp;
      {
        std::lock_guard<std::mutex> lock(mu_);
        on_smp = on_smp_;
      }
      if (on_smp) on_smp(hdr.handle, pdu);
      continue;
    }
    if (cid != att::kCidAtt || pdu.empty()) continue;

    uint8_t op = pdu[0];
    bool is_notification = op == att::kHandleValueNotification ||
                           op == att::kHandleValueIndication;

    // Is somebody waiting for this as a response? Notifications never are,
    // even though they arrive on the same channel -- which is exactly the
    // confusion that makes a subscribed characteristic look like a stuck
    // request.
    if (!is_notification) {
      AttWait* wait = nullptr;
      std::lock_guard<std::mutex> lock(mu_);
      auto it = att_waits_.find(hdr.handle);
      if (it != att_waits_.end() && it->second.waiting && !it->second.done) {
        wait = &it->second;
      }
      if (wait) {
        bool matches = op == response_opcode(wait->req_opcode);
        if (!matches && op == att::kErrorResponse && pdu.size() >= 2) {
          matches = pdu[1] == wait->req_opcode;
        }
        if (matches) {
          wait->rsp = pdu;
          wait->done = true;
          cv_.notify_all();
          continue;
        }
      }
    }

    AttHandler on_att;
    {
      std::lock_guard<std::mutex> lock(mu_);
      on_att = on_att_;
    }
    if (on_att) on_att(hdr.handle, pdu);
  }
}

// ---------------------------------------------------------------- scanning

bool Link::start_scan(bool active, bool filter_duplicates, AdvHandler on_report,
                      std::string* err) {
  {
    // A null handler means "keep whatever is already installed". That is what
    // lets connect() put scanning back the way it found it without having to
    // carry the caller's callback around -- and stops it clearing the handler
    // outright, which would leave a scan running that reports to nobody.
    std::lock_guard<std::mutex> lock(mu_);
    if (on_report) on_adv_ = std::move(on_report);
  }
  CommandComplete cc;
  // A 100% duty cycle: window equal to interval. Nothing else here needs the
  // radio while scanning, and a Tentacle advertisement missed is a second of
  // drift history lost.
  uint8_t own;
  {
    std::lock_guard<std::mutex> lock(mu_);
    own = own_addr_type_;
  }
  std::vector<uint8_t> params =
      le_set_scan_params(active, ms_to_scan_units(60), ms_to_scan_units(60), own);
  if (!command(kOpLeSetScanParams, params, &cc, err)) return false;
  if (!command(kOpLeSetScanEnable, le_set_scan_enable(true, filter_duplicates),
               &cc, err)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  scanning_ = true;
  return true;
}

bool Link::stop_scan(std::string* err) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!scanning_) return true;
  }
  CommandComplete cc;
  bool ok = command(kOpLeSetScanEnable, le_set_scan_enable(false, false), &cc,
                    err);
  std::lock_guard<std::mutex> lock(mu_);
  scanning_ = false;
  return ok;
}

bool Link::scanning() const {
  std::lock_guard<std::mutex> lock(mu_);
  return scanning_;
}

// ------------------------------------------------------------- advertising

bool Link::start_advertising(const AdvConfig& cfg, std::string* err) {
  if (cfg.adv_data.size() > 31) {
    if (err) {
      *err = "advertising data is " + std::to_string(cfg.adv_data.size()) +
             " bytes; the limit is 31";
    }
    return false;
  }
  if (cfg.scan_response.size() > 31) {
    if (err) *err = "scan response data is longer than 31 bytes";
    return false;
  }

  CommandComplete cc;
  uint8_t own;
  {
    std::lock_guard<std::mutex> lock(mu_);
    own = own_addr_type_;
  }

  // Parameters before data: the controller latches the advertising type here,
  // and a scan response set on a non-scannable type is quietly never sent.
  uint16_t interval = ms_to_adv_units(cfg.interval_ms);
  Address none;
  std::vector<uint8_t> params =
      le_set_adv_params(interval, interval, cfg.type, own, none);
  if (!command(kOpLeSetAdvParams, params, &cc, err)) return false;

  std::vector<uint8_t> data;
  if (!le_set_adv_data(cfg.adv_data, &data)) {
    if (err) *err = "advertising data does not fit";
    return false;
  }
  if (!command(kOpLeSetAdvData, data, &cc, err)) return false;

  if (!cfg.scan_response.empty()) {
    std::vector<uint8_t> rsp;
    le_set_scan_response_data(cfg.scan_response, &rsp);
    if (!command(kOpLeSetScanResponseData, rsp, &cc, err)) return false;
  }

  if (!command(kOpLeSetAdvEnable, le_set_adv_enable(true), &cc, err)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  advertising_ = true;
  return true;
}

bool Link::stop_advertising(std::string* err) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!advertising_) return true;
  }
  CommandComplete cc;
  bool ok = command(kOpLeSetAdvEnable, le_set_adv_enable(false), &cc, err);
  std::lock_guard<std::mutex> lock(mu_);
  advertising_ = false;
  return ok;
}

bool Link::advertising() const {
  std::lock_guard<std::mutex> lock(mu_);
  return advertising_;
}

// ------------------------------------------------------------- connections

void Link::set_connection_handlers(ConnectedHandler on_connect,
                                   DisconnectedHandler on_disconnect) {
  std::lock_guard<std::mutex> lock(mu_);
  on_connect_ = std::move(on_connect);
  on_disconnect_ = std::move(on_disconnect);
}

bool Link::connect(const Address& peer, double timeout, uint16_t* handle,
                   std::string* err) {
  bool was_scanning = scanning();
  if (was_scanning && !stop_scan(err)) return false;

  uint8_t own;
  {
    std::lock_guard<std::mutex> lock(mu_);
    own = own_addr_type_;
    connect_pending_ = true;
    connect_done_ = false;
  }

  std::vector<uint8_t> params = le_create_connection(
      peer, ms_to_scan_units(60), ms_to_scan_units(30), ms_to_conn_units(30),
      ms_to_conn_units(50), 0, ms_to_supervision_units(4000), own);

  CommandComplete cc;
  // LE Create Connection answers with a Command Status; the connection itself
  // arrives later as an LE Connection Complete.
  if (!command(kOpLeCreateConnection, params, &cc, err)) {
    std::lock_guard<std::mutex> lock(mu_);
    connect_pending_ = false;
    return false;
  }

  std::unique_lock<std::mutex> lock(mu_);
  bool ok = cv_.wait_for(lock, to_ms(timeout),
                         [this] { return connect_done_ || !running_; });
  ConnectionComplete result = connect_result_;
  bool done = connect_done_;
  connect_pending_ = false;
  lock.unlock();

  if (!ok || !done) {
    // The controller is still trying. Cancelling is not optional: an initiator
    // left running blocks every later scan with "command disallowed".
    command(kOpLeCreateConnectionCancel, {}, &cc, nullptr);
    if (err) *err = "connection to " + address_to_string(peer) + " timed out";
    if (was_scanning) start_scan(false, false, nullptr, nullptr);
    return false;
  }
  if (result.status != kSuccess) {
    if (err) {
      *err = "connection to " + address_to_string(peer) + ": " +
             status_name(result.status);
    }
    return false;
  }
  if (handle) *handle = result.handle;
  return true;
}

bool Link::disconnect(uint16_t handle, uint8_t reason) {
  CommandComplete cc;
  return command(kOpDisconnect, disconnect_params(handle, reason), &cc, nullptr);
}

std::vector<Link::Conn> Link::connections() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<Conn> out;
  out.reserve(conns_.size());
  for (const auto& kv : conns_) out.push_back(kv.second);
  return out;
}

bool Link::connected(uint16_t handle) const {
  std::lock_guard<std::mutex> lock(mu_);
  return conns_.count(handle) != 0;
}

// --------------------------------------------------------------------- ATT

void Link::set_att_handler(AttHandler on_att) {
  std::lock_guard<std::mutex> lock(mu_);
  on_att_ = std::move(on_att);
}

void Link::set_l2cap_handler(L2capHandler on_frame) {
  std::lock_guard<std::mutex> lock(mu_);
  on_l2cap_ = std::move(on_frame);
}

void Link::set_event_handler(EventHandler on_event) {
  std::lock_guard<std::mutex> lock(mu_);
  on_event_ = std::move(on_event);
}

bool Link::send_acl(uint16_t conn, const std::vector<uint8_t>& frame,
                    std::string* err) {
  size_t payload;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!conns_.count(conn)) {
      if (err) *err = "not connected";
      return false;
    }
    payload = acl_payload_;
  }

  std::vector<std::vector<uint8_t>> frags = att::fragment(frame, payload);
  for (size_t i = 0; i < frags.size(); ++i) {
    {
      // Wait for a controller buffer. Sending without one is not refused --
      // the packet is dropped -- so the peer simply stops answering and
      // nothing anywhere reports an error.
      std::unique_lock<std::mutex> lock(mu_);
      if (!cv_.wait_for(lock, to_ms(5.0),
                        [this] { return acl_credits_ > 0 || !running_; })) {
        if (err) *err = "controller has no ACL buffers free";
        return false;
      }
      if (!running_) {
        if (err) *err = "link is closed";
        return false;
      }
      --acl_credits_;
    }
    uint8_t pb = i == 0 ? kAclFirstFlushable : kAclContinuing;
    if (!write_packet(build_acl(conn, pb, frags[i].data(), frags[i].size()),
                      err)) {
      return false;
    }
  }
  return true;
}

bool Link::send_l2cap(uint16_t conn, uint16_t cid,
                      const std::vector<uint8_t>& payload, std::string* err) {
  return send_acl(conn, att::build_l2cap(cid, payload), err);
}

bool Link::send_att(uint16_t conn, const std::vector<uint8_t>& pdu,
                    std::string* err) {
  return send_l2cap(conn, att::kCidAtt, pdu, err);
}

bool Link::att_request(uint16_t conn, const std::vector<uint8_t>& req,
                       std::vector<uint8_t>* rsp, double timeout,
                       std::string* err) {
  if (req.empty()) {
    if (err) *err = "empty ATT request";
    return false;
  }
  // One request at a time, per the protocol. Enforced rather than assumed:
  // two responses on one channel cannot be told apart.
  std::lock_guard<std::mutex> serialise(att_request_mu_);
  {
    std::lock_guard<std::mutex> lock(mu_);
    AttWait& wait = att_waits_[conn];
    wait.waiting = true;
    wait.done = false;
    wait.req_opcode = req[0];
    wait.rsp.clear();
  }

  if (!send_att(conn, req, err)) {
    std::lock_guard<std::mutex> lock(mu_);
    att_waits_[conn].waiting = false;
    return false;
  }

  std::unique_lock<std::mutex> lock(mu_);
  bool signalled = cv_.wait_for(lock, to_ms(timeout), [this, conn] {
    auto it = att_waits_.find(conn);
    return !running_ || (it != att_waits_.end() && it->second.done);
  });
  AttWait& wait = att_waits_[conn];
  bool done = wait.done;
  std::vector<uint8_t> got = wait.rsp;
  wait.waiting = false;
  lock.unlock();

  if (!signalled || !done) {
    if (err) {
      *err = std::string(att::opcode_name(req[0])) + ": no response";
    }
    return false;
  }
  if (got.empty()) {
    if (err) *err = "connection dropped during " +
                    std::string(att::opcode_name(req[0]));
    return false;
  }
  if (rsp) *rsp = got;
  return true;
}

// --------------------------------------------------------------------- SMP

void Link::set_smp_handler(SmpHandler on_smp) {
  std::lock_guard<std::mutex> lock(mu_);
  on_smp_ = std::move(on_smp);
}

bool Link::send_smp(uint16_t conn, const std::vector<uint8_t>& pdu,
                    std::string* err) {
  return send_l2cap(conn, att::kCidSmp, pdu, err);
}

bool Link::encrypted(uint16_t conn) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = encrypted_.find(conn);
  return it != encrypted_.end() && it->second;
}

bool Link::start_encryption(uint16_t conn, const std::array<uint8_t, 16>& ltk,
                            uint16_t ediv, const std::array<uint8_t, 8>& rand,
                            double timeout, std::string* err) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!conns_.count(conn)) {
      if (err) *err = "not connected";
      return false;
    }
    encrypt_pending_ = true;
    encrypt_done_ = false;
    encrypt_status_ = 0;
    encrypt_enabled_ = 0;
  }

  // handle, Rand, EDIV, LTK -- all little-endian, so the key goes on the wire
  // reversed from the order crypto.h computes it in.
  std::vector<uint8_t> params;
  params.push_back(static_cast<uint8_t>(conn & 0xff));
  params.push_back(static_cast<uint8_t>(conn >> 8));
  params.insert(params.end(), rand.begin(), rand.end());
  params.push_back(static_cast<uint8_t>(ediv & 0xff));
  params.push_back(static_cast<uint8_t>(ediv >> 8));
  for (size_t i = 0; i < 16; ++i) params.push_back(ltk[15 - i]);

  CommandComplete cc;
  if (!command(kOpLeStartEncryption, params, &cc, err)) {
    std::lock_guard<std::mutex> lock(mu_);
    encrypt_pending_ = false;
    return false;
  }

  std::unique_lock<std::mutex> lock(mu_);
  bool signalled = cv_.wait_for(lock, to_ms(timeout),
                                [this] { return encrypt_done_ || !running_; });
  bool done = encrypt_done_;
  uint8_t status = encrypt_status_;
  uint8_t enabled = encrypt_enabled_;
  encrypt_pending_ = false;
  lock.unlock();

  if (!signalled || !done) {
    if (err) *err = "the controller never reported encryption starting";
    return false;
  }
  if (status != kSuccess) {
    if (err) {
      // "PIN or key missing" is what a disagreement about the key looks like
      // from here, and on a camera that almost always means the passkey was
      // wrong rather than anything being absent.
      *err = std::string("encryption failed: ") + status_name(status) +
             (status == kPinOrKeyMissing
                  ? " (the peer rejected the key; the passkey was probably"
                    " wrong)"
                  : "");
    }
    return false;
  }
  if (!enabled) {
    if (err) *err = "the controller reported encryption off";
    return false;
  }
  return true;
}

}  // namespace hci
}  // namespace octo
