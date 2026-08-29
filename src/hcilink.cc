#include "hcilink.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace octo {
namespace hci {
namespace {

// The opcode a response carries for a given request. ATT numbers responses one
// above their request, with the write command and the notifications as the
// exceptions that have no response at all.
uint8_t response_opcode(uint8_t request) {
  return static_cast<uint8_t>(request + 1);
}

const char kLinkClosed[] = "the link is closed";

}  // namespace

Link::Link(Loop* loop) : loop_(loop), alive_(std::make_shared<bool>(true)) {}

Link::~Link() {
  *alive_ = false;
  if (loop_) {
    if (source_ != kNoSource) loop_->remove_source(source_);
    loop_->cancel(cmd_timer_);
    loop_->cancel(connect_timer_);
    loop_->cancel(encrypt_timer_);
    for (auto& kv : att_) loop_->cancel(kv.second.timer);
  }
  if (port_) port_->close();
}

std::unique_ptr<Link> Link::open(Loop* loop, const Options& opts,
                                 DoneHandler on_ready, std::string* err) {
  std::unique_ptr<Port> port = open_port(opts.device, err);
  if (!port) return nullptr;
  return attach(loop, std::move(port), opts, std::move(on_ready));
}

std::unique_ptr<Link> Link::attach(Loop* loop, std::unique_ptr<Port> port,
                                   const Options& opts, DoneHandler on_ready) {
  std::unique_ptr<Link> link(new Link(loop));
  link->port_ = std::move(port);
  link->opts_ = opts;
  link->on_ready_ = std::move(on_ready);

  Link* raw = link.get();
  link->source_ = loop->add_source(
      link->port_->handle(), kRead,
      [raw](int interest) { raw->on_readable(interest); },
      [raw](const std::string& why) { raw->on_port_error(why); });

  link->init_step(0);
  return link;
}

std::string Link::port_name() const {
  return port_ ? port_->name() : std::string();
}

std::string Link::describe() const {
  std::string out = port_ ? port_->name() : std::string("(no port)");
  if (!version_.empty()) out += ", " + version_;
  out += ", address " + address_to_string(local_);
  out += own_addr_type_ == kAddrRandom ? " (random static)" : " (public)";
  return out;
}

void Link::set_closed_handler(ClosedHandler on_closed) {
  on_closed_ = std::move(on_closed);
}

void Link::set_connection_handlers(ConnectedHandler on_connect,
                                   DisconnectedHandler on_disconnect) {
  on_connect_ = std::move(on_connect);
  on_disconnect_ = std::move(on_disconnect);
}

void Link::set_att_handler(AttHandler on_att) { on_att_ = std::move(on_att); }
void Link::set_smp_handler(SmpHandler on_smp) { on_smp_ = std::move(on_smp); }
void Link::set_l2cap_handler(L2capHandler f) { on_l2cap_ = std::move(f); }
void Link::set_event_handler(EventHandler f) { on_event_ = std::move(f); }

void Link::defer(std::function<void()> fn) {
  std::shared_ptr<bool> alive = alive_;
  loop_->after(0.0, [alive, fn] {
    if (*alive) fn();
  });
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

// ------------------------------------------------------------------ reading

void Link::on_readable(int interest) {
  (void)interest;
  if (closed_) return;

  uint8_t chunk[512];
  int n = port_->read(chunk, sizeof chunk, 0.0);
  if (n < 0) {
    // The port died -- almost always the dongle being unplugged.
    fail("the dongle at " + port_name() + " went away");
    return;
  }
  if (n == 0) return;
  rx_.insert(rx_.end(), chunk, chunk + n);

  std::vector<Packet> packets;
  size_t used = parse_stream(rx_.data(), rx_.size(), &packets);
  rx_.erase(rx_.begin(), rx_.begin() + used);

  for (const Packet& pkt : packets) {
    if (closed_) return;
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

void Link::on_port_error(const std::string& why) {
  fail("the dongle at " + port_name() + ": " + why);
}

// Everything outstanding fails at once, and nothing is left holding a
// completion that will never be called. Containers are moved out before any
// handler runs: a handler is entitled to start something new, and it must not
// find itself iterating a list that is being torn down underneath it.
void Link::fail(const std::string& why) {
  if (closed_) return;
  closed_ = true;
  if (loop_ && source_ != kNoSource) {
    loop_->remove_source(source_);
    source_ = kNoSource;
  }
  if (port_) port_->close();

  std::deque<QueuedCommand> commands;
  commands.swap(cmd_queue_);
  CommandHandler cmd_done = std::move(cmd_done_);
  cmd_done_ = nullptr;
  cmd_in_flight_ = false;
  loop_->cancel(cmd_timer_);
  cmd_timer_ = kNoTimer;

  std::map<uint16_t, AttChannel> att;
  att.swap(att_);
  for (auto& kv : att) loop_->cancel(kv.second.timer);

  ConnectHandler connect_done = std::move(connect_done_);
  connect_done_ = nullptr;
  connect_pending_ = false;
  loop_->cancel(connect_timer_);
  connect_timer_ = kNoTimer;

  DoneHandler encrypt_done = std::move(encrypt_done_);
  encrypt_done_ = nullptr;
  encrypt_pending_ = false;
  loop_->cancel(encrypt_timer_);
  encrypt_timer_ = kNoTimer;

  DoneHandler on_ready = std::move(on_ready_);
  on_ready_ = nullptr;

  acl_queue_.clear();
  scanning_ = false;
  advertising_ = false;

  if (cmd_done) cmd_done(false, CommandComplete(), why);
  for (auto& c : commands) {
    if (c.done) c.done(false, CommandComplete(), why);
  }
  for (auto& kv : att) {
    if (kv.second.done) kv.second.done(false, {}, why);
    for (auto& q : kv.second.queue) {
      if (q.done) q.done(false, {}, why);
    }
  }
  if (connect_done) connect_done(false, 0, why);
  if (encrypt_done) encrypt_done(false, why);
  if (on_ready) on_ready(false, why);
  if (on_closed_) on_closed_(why);
}

// ------------------------------------------------------------- bringing up

void Link::finish_init(bool ok, const std::string& err) {
  DoneHandler done = std::move(on_ready_);
  on_ready_ = nullptr;
  if (done) done(ok, err);
}

void Link::init_step(int step) {
  switch (step) {
    case 0:
      command(kOpReset, {}, [this](bool ok, const CommandComplete&,
                                   const std::string& err) {
        if (!ok) return finish_init(false, err);
        init_step(1);
      });
      return;

    case 1: {
      // Ask for the events this host actually handles. The controller's
      // default mask leaves several of them off, and an event that is masked
      // out is not an error anywhere -- it simply never arrives.
      std::vector<uint8_t> mask(8, 0xff);
      command(kOpSetEventMask, mask,
              [this](bool ok, const CommandComplete&, const std::string& err) {
                if (!ok) return finish_init(false, err);
                init_step(2);
              });
      return;
    }

    case 2: {
      // LE event mask: connection complete, advertising report, connection
      // update, read remote features, long term key request, data length
      // change, the P-256 pair, and enhanced connection complete.
      std::vector<uint8_t> le_mask = {0xff, 0x07, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00};
      command(kOpLeSetEventMask, le_mask,
              [this](bool ok, const CommandComplete&, const std::string& err) {
                if (!ok) return finish_init(false, err);
                init_step(3);
              });
      return;
    }

    case 3:
      // Everything from here on is best-effort: a controller that will not
      // answer these still works, and refusing to come up over a version
      // string would be a worse outcome than not printing one.
      command(kOpReadLocalVersion, {},
              [this](bool ok, const CommandComplete& cc, const std::string&) {
                if (ok && cc.params.size() >= 9) {
                  char buf[64];
                  std::snprintf(
                      buf, sizeof buf, "HCI version %u, LMP subversion 0x%04x",
                      cc.params[1],
                      static_cast<unsigned>(cc.params[7] | (cc.params[8] << 8)));
                  version_ = buf;
                }
                init_step(4);
              });
      return;

    case 4:
      command(kOpLeReadBufferSize, {},
              [this](bool ok, const CommandComplete& cc, const std::string&) {
                if (ok && cc.params.size() >= 4) {
                  size_t payload =
                      static_cast<size_t>(cc.params[1] | (cc.params[2] << 8));
                  int total = cc.params[3];
                  // A controller that reports zero here is telling us to use
                  // the BR/EDR buffer pool. Nothing this program does needs
                  // more than one packet in flight, so the conservative floor
                  // is a correct answer rather than a reason to go and read
                  // the other buffer size.
                  acl_payload_ = payload ? payload : 27;
                  acl_total_ = total ? total : 1;
                  acl_credits_ = acl_total_;
                }
                init_address();
              });
      return;

    default:
      finish_init(true, std::string());
      return;
  }
}

// An address to be.
//
// The nRF52840 has no public address assigned to it, so Read BD_ADDR comes
// back all zeros and every attempt to advertise or connect from a "public"
// address is rejected -- with a status that says nothing about addresses.
// Installing a random static address is the fix, and it has to happen before
// anything else touches the air.
void Link::init_address() {
  command(kOpReadBdAddr, {}, [this](bool ok, const CommandComplete& cc,
                                    const std::string&) {
    Address addr;
    bool have_public = false;
    if (ok && cc.params.size() >= 7) {
      for (size_t i = 0; i < 6; ++i) addr.bytes[i] = cc.params[6 - i];
      addr.type = kAddrPublic;
      for (uint8_t b : addr.bytes) {
        if (b != 0x00) have_public = true;
      }
    }
    if (have_public) {
      local_ = addr;
      own_addr_type_ = kAddrPublic;
      return finish_init(true, std::string());
    }

    command(kOpLeRand, {}, [this](bool rand_ok, const CommandComplete& rc,
                                  const std::string&) {
      Address rnd;
      if (rand_ok && rc.params.size() >= 9) {
        for (size_t i = 0; i < 6; ++i) rnd.bytes[i] = rc.params[1 + i];
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
      command(kOpLeSetRandomAddress, params,
              [this, rnd](bool set_ok, const CommandComplete&,
                          const std::string& err) {
                if (!set_ok) return finish_init(false, err);
                local_ = rnd;
                own_addr_type_ = kAddrRandom;
                finish_init(true, std::string());
              });
    });
  });
}

// --------------------------------------------------------------- commands

void Link::command(uint16_t opcode, const std::vector<uint8_t>& params,
                   CommandHandler done) {
  if (closed_) {
    if (done) {
      defer([done] { done(false, CommandComplete(), kLinkClosed); });
    }
    return;
  }
  QueuedCommand q;
  q.opcode = opcode;
  q.params = params;
  q.done = std::move(done);
  cmd_queue_.push_back(std::move(q));
  pump_commands();
}

void Link::pump_commands() {
  if (cmd_in_flight_ || cmd_queue_.empty() || closed_) return;

  QueuedCommand q = std::move(cmd_queue_.front());
  cmd_queue_.pop_front();
  cmd_in_flight_ = true;
  cmd_opcode_ = q.opcode;
  cmd_done_ = std::move(q.done);

  std::string err;
  if (!write_packet(build_command(q.opcode, q.params), &err)) {
    fail(err);
    return;
  }

  const uint16_t opcode = q.opcode;
  cmd_timer_ = loop_->after(opts_.command_timeout, [this, opcode] {
    cmd_timer_ = kNoTimer;
    if (!cmd_in_flight_ || cmd_opcode_ != opcode) return;
    finish_command(false, CommandComplete(),
                   std::string(opcode_name(opcode)) +
                       ": no answer from the controller");
  });
}

void Link::finish_command(bool ok, const CommandComplete& cc,
                          const std::string& err) {
  if (!cmd_in_flight_) return;
  loop_->cancel(cmd_timer_);
  cmd_timer_ = kNoTimer;
  cmd_in_flight_ = false;
  CommandHandler done = std::move(cmd_done_);
  cmd_done_ = nullptr;

  if (done) done(ok, cc, err);
  pump_commands();
}

void Link::command_sequence(
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> steps,
    DoneHandler done) {
  auto seq = std::make_shared<Sequence>();
  seq->steps = std::move(steps);
  seq->done = std::move(done);
  run_sequence(seq);
}

void Link::run_sequence(std::shared_ptr<Sequence> seq) {
  if (seq->index >= seq->steps.size()) {
    DoneHandler done = std::move(seq->done);
    seq->done = nullptr;
    if (done) done(true, std::string());
    return;
  }
  const auto& step = seq->steps[seq->index];
  ++seq->index;
  command(step.first, step.second,
          [this, seq](bool ok, const CommandComplete&,
                      const std::string& err) {
            if (!ok) {
              DoneHandler done = std::move(seq->done);
              seq->done = nullptr;
              if (done) done(false, err);
              return;
            }
            run_sequence(seq);
          });
}

// ---------------------------------------------------------------- events

void Link::dispatch_event(const Event& evt) {
  if (on_event_) on_event_(evt);

  switch (evt.code) {
    case kEvtCommandComplete: {
      CommandComplete cc;
      if (!parse_command_complete(evt, &cc)) return;
      // A Command Complete with opcode zero exists only to restore the
      // controller's command credit and matches no caller.
      if (!cmd_in_flight_ || cc.opcode != cmd_opcode_) return;
      if (cc.status != kSuccess) {
        finish_command(false, cc,
                       std::string(opcode_name(cc.opcode)) + ": " +
                           status_name(cc.status));
      } else {
        finish_command(true, cc, std::string());
      }
      return;
    }

    case kEvtCommandStatus: {
      CommandStatus cs;
      if (!parse_command_status(evt, &cs)) return;
      if (!cmd_in_flight_ || cs.opcode != cmd_opcode_) return;
      // A Command Status means the command was accepted and its result will
      // arrive later as its own event -- except when it failed outright, in
      // which case this is the whole answer.
      CommandComplete cc;
      cc.opcode = cs.opcode;
      cc.status = cs.status;
      if (cs.status != kSuccess) {
        finish_command(false, cc,
                       std::string(opcode_name(cs.opcode)) + ": " +
                           status_name(cs.status));
      } else {
        finish_command(true, cc, std::string());
      }
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
      acl_credits_ += freed;
      if (acl_credits_ > acl_total_) acl_credits_ = acl_total_;
      pump_acl();
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
      encrypted_[ec.handle] = ec.status == kSuccess && ec.enabled != 0;
      if (!encrypt_pending_) return;
      if (ec.status != kSuccess) {
        // "PIN or key missing" is what a disagreement about the key looks like
        // from here, and on a camera that almost always means the passkey was
        // wrong rather than anything being absent.
        finish_encryption(
            false,
            std::string("encryption failed: ") + status_name(ec.status) +
                (ec.status == kPinOrKeyMissing
                     ? " (the peer rejected the key; the passkey was probably"
                       " wrong)"
                     : ""));
      } else if (!ec.enabled) {
        finish_encryption(false, "the controller reported encryption off");
      } else {
        finish_encryption(true, std::string());
      }
      return;
    }

    case kEvtDisconnectionComplete: {
      DisconnectionComplete dc;
      if (!parse_disconnection_complete(evt, &dc)) return;
      drop_connection(dc.handle, "the connection dropped");
      if (on_disconnect_) on_disconnect_(dc.handle, dc.reason);
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
      if (!on_adv_) return;
      for (const AdvReport& r : reports) {
        if (closed_) return;
        on_adv_(r);
      }
      return;
    }

    case kLeConnectionComplete:
    case kLeEnhancedConnectionComplete: {
      ConnectionComplete cc;
      if (!parse_connection_complete(evt, &cc)) return;
      Conn conn;
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
      if (connect_pending_) {
        if (cc.status == kSuccess) {
          finish_connect(true, cc.handle, std::string());
        } else {
          finish_connect(false, 0,
                         "connection to " + address_to_string(connect_peer_) +
                             ": " + status_name(cc.status));
        }
      }
      if (cc.status == kSuccess && on_connect_) on_connect_(conn);
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
      // Queued like any other command now. The old code wrote this straight to
      // the port because it ran on the reader thread and command() would have
      // blocked on it; with no thread there is nothing to deadlock against and
      // no reason to bypass the one-command-at-a-time rule.
      command(kOpLeLtkRequestNegReply, params, nullptr);
      return;
    }

    default:
      return;
  }
}

// Retire everything that belonged to a connection. Called both when the peer
// disconnects and when the whole link dies, so it has to be safe to call for a
// handle that was never known.
void Link::drop_connection(uint16_t handle, const std::string& why) {
  conns_.erase(handle);
  reasm_.erase(handle);
  encrypted_.erase(handle);

  // Fragments still queued for a dead handle would be sent to whichever
  // connection inherits the number next.
  for (auto it = acl_queue_.begin(); it != acl_queue_.end();) {
    it = it->conn == handle ? acl_queue_.erase(it) : it + 1;
  }

  auto it = att_.find(handle);
  if (it == att_.end()) return;
  AttChannel channel = std::move(it->second);
  att_.erase(it);
  loop_->cancel(channel.timer);

  // Anyone waiting on this connection is never going to get an answer.
  // Failing them now beats a thirty-second timeout.
  if (channel.done) channel.done(false, {}, why);
  for (auto& q : channel.queue) {
    if (q.done) q.done(false, {}, why);
  }

  if (encrypt_pending_) finish_encryption(false, why);
}

// ------------------------------------------------------------------ ACL

void Link::dispatch_acl(const std::vector<uint8_t>& payload) {
  AclHeader hdr;
  std::vector<uint8_t> data;
  if (!parse_acl(payload, &hdr, &data)) return;

  auto it = reasm_.find(hdr.handle);
  if (it == reasm_.end()) return;  // a connection we do not know about

  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> frames;
  it->second.push(hdr.pb_flag, data.data(), data.size(), &frames);

  for (const auto& frame : frames) {
    if (closed_) return;
    uint16_t cid = frame.first;
    const std::vector<uint8_t>& pdu = frame.second;

    if (on_l2cap_) on_l2cap_(hdr.handle, cid, pdu);

    if (cid == att::kCidSmp) {
      if (on_smp_) on_smp_(hdr.handle, pdu);
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
    //
    // Belt and braces, and worth saying which is which. The opcode arithmetic
    // below already rules a notification out: responses are numbered one above
    // their request, and no defined request is 0x1a or 0x1c, so nothing can
    // ever be waiting for 0x1b or 0x1d. This test is what keeps that true if
    // the matching rule is ever loosened -- it is cheap, and the failure it
    // prevents is silent.
    if (!is_notification) {
      auto ch = att_.find(hdr.handle);
      if (ch != att_.end() && ch->second.in_flight) {
        bool matches = op == response_opcode(ch->second.req_opcode);
        if (!matches && op == att::kErrorResponse && pdu.size() >= 2) {
          matches = pdu[1] == ch->second.req_opcode;
        }
        if (matches) {
          finish_att(hdr.handle, true, pdu, std::string());
          continue;
        }
      }
    }

    if (on_att_) on_att_(hdr.handle, pdu);
  }
}

bool Link::queue_acl(uint16_t conn, const std::vector<uint8_t>& frame,
                     std::string* err) {
  if (closed_) {
    if (err) *err = kLinkClosed;
    return false;
  }
  if (!conns_.count(conn)) {
    if (err) *err = "not connected";
    return false;
  }
  std::vector<std::vector<uint8_t>> frags = att::fragment(frame, acl_payload_);
  if (acl_queue_.size() + frags.size() > kMaxQueuedFragments) {
    if (err) {
      *err = "the controller is not keeping up; " +
             std::to_string(acl_queue_.size()) + " fragments are already queued";
    }
    return false;
  }
  for (size_t i = 0; i < frags.size(); ++i) {
    QueuedFragment q;
    q.conn = conn;
    q.pb = i == 0 ? kAclFirstFlushable : kAclContinuing;
    q.data = std::move(frags[i]);
    acl_queue_.push_back(std::move(q));
  }
  pump_acl();
  if (closed_) {
    // The port broke while these were going out. The fragments are gone and so
    // is the link; saying "accepted" would be a lie the caller cannot check.
    if (err) *err = kLinkClosed;
    return false;
  }
  return true;
}

void Link::pump_acl() {
  while (!closed_ && acl_credits_ > 0 && !acl_queue_.empty()) {
    QueuedFragment q = std::move(acl_queue_.front());
    acl_queue_.pop_front();
    --acl_credits_;
    std::string err;
    if (!write_packet(build_acl(q.conn, q.pb, q.data.data(), q.data.size()),
                      &err)) {
      fail(err);
      return;
    }
  }
}

bool Link::send_l2cap(uint16_t conn, uint16_t cid,
                      const std::vector<uint8_t>& payload, std::string* err) {
  return queue_acl(conn, att::build_l2cap(cid, payload), err);
}

bool Link::send_att(uint16_t conn, const std::vector<uint8_t>& pdu,
                    std::string* err) {
  return send_l2cap(conn, att::kCidAtt, pdu, err);
}

bool Link::send_smp(uint16_t conn, const std::vector<uint8_t>& pdu,
                    std::string* err) {
  return send_l2cap(conn, att::kCidSmp, pdu, err);
}

// ------------------------------------------------------------------- ATT

void Link::att_request(uint16_t conn, const std::vector<uint8_t>& req,
                       double timeout, ResponseHandler done) {
  if (req.empty()) {
    if (done) defer([done] { done(false, {}, "empty ATT request"); });
    return;
  }
  if (closed_ || !conns_.count(conn)) {
    std::string why = closed_ ? kLinkClosed : "not connected";
    if (done) defer([done, why] { done(false, {}, why); });
    return;
  }
  QueuedRequest q;
  q.req = req;
  q.timeout = timeout;
  q.done = std::move(done);
  att_[conn].queue.push_back(std::move(q));
  pump_att(conn);
}

void Link::pump_att(uint16_t conn) {
  auto it = att_.find(conn);
  if (it == att_.end()) return;
  AttChannel& ch = it->second;
  if (ch.in_flight || ch.queue.empty() || closed_) return;

  QueuedRequest q = std::move(ch.queue.front());
  ch.queue.pop_front();
  ch.in_flight = true;
  ch.req_opcode = q.req[0];
  ch.done = std::move(q.done);
  const uint8_t opcode = q.req[0];

  std::string err;
  if (!send_att(conn, q.req, &err)) {
    finish_att(conn, false, {}, err);
    return;
  }

  // A broken port fails the whole link from inside that send, and failing the
  // link retires every channel. Nothing held across it is still valid, so the
  // channel has to be found again rather than remembered.
  auto live = att_.find(conn);
  if (live == att_.end() || !live->second.in_flight) return;
  live->second.timer = loop_->after(q.timeout, [this, conn, opcode] {
    auto now = att_.find(conn);
    if (now == att_.end()) return;
    now->second.timer = kNoTimer;
    if (!now->second.in_flight || now->second.req_opcode != opcode) return;
    finish_att(conn, false, {},
               std::string(att::opcode_name(opcode)) + ": no response");
  });
}

void Link::finish_att(uint16_t conn, bool ok, const std::vector<uint8_t>& rsp,
                      const std::string& err) {
  auto it = att_.find(conn);
  if (it == att_.end()) return;
  AttChannel& ch = it->second;
  if (!ch.in_flight) return;

  loop_->cancel(ch.timer);
  ch.timer = kNoTimer;
  ch.in_flight = false;
  ResponseHandler done = std::move(ch.done);
  ch.done = nullptr;

  // The handler is entitled to issue the next request, drop the connection, or
  // close the link, so nothing may be read out of `ch` after this point.
  if (done) done(ok, rsp, err);
  pump_att(conn);
}

// --------------------------------------------------------------- scanning

void Link::start_scan(bool active, bool filter_duplicates, AdvHandler on_report,
                      DoneHandler done) {
  // A null handler means "keep whatever is already installed". That is what
  // lets connect() put scanning back the way it found it without having to
  // carry the caller's callback around -- and stops it clearing the handler
  // outright, which would leave a scan running that reports to nobody.
  if (on_report) on_adv_ = std::move(on_report);

  // A 100% duty cycle: window equal to interval. Nothing else here needs the
  // radio while scanning, and a Tentacle advertisement missed is a second of
  // drift history lost.
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> steps = {
      {kOpLeSetScanParams, le_set_scan_params(active, ms_to_scan_units(60),
                                              ms_to_scan_units(60),
                                              own_addr_type_)},
      {kOpLeSetScanEnable, le_set_scan_enable(true, filter_duplicates)},
  };
  command_sequence(std::move(steps),
                   [this, done](bool ok, const std::string& err) {
                     if (ok) scanning_ = true;
                     if (done) done(ok, err);
                   });
}

void Link::stop_scan(DoneHandler done) {
  if (!scanning_) {
    if (done) defer([done] { done(true, std::string()); });
    return;
  }
  command_sequence({{kOpLeSetScanEnable, le_set_scan_enable(false, false)}},
                   [this, done](bool ok, const std::string& err) {
                     // Off either way: a controller that refused to stop
                     // scanning is not one this host can keep pretending to
                     // drive, and leaving the flag set would make every later
                     // stop_scan a no-op.
                     scanning_ = false;
                     if (done) done(ok, err);
                   });
}

// ------------------------------------------------------------ advertising

void Link::start_advertising(const AdvConfig& cfg, DoneHandler done) {
  if (cfg.adv_data.size() > 31) {
    std::string why = "advertising data is " +
                      std::to_string(cfg.adv_data.size()) +
                      " bytes; the limit is 31";
    if (done) defer([done, why] { done(false, why); });
    return;
  }
  if (cfg.scan_response.size() > 31) {
    if (done) {
      defer([done] {
        done(false, "scan response data is longer than 31 bytes");
      });
    }
    return;
  }
  std::vector<uint8_t> data;
  if (!le_set_adv_data(cfg.adv_data, &data)) {
    if (done) defer([done] { done(false, "advertising data does not fit"); });
    return;
  }

  // Parameters before data: the controller latches the advertising type here,
  // and a scan response set on a non-scannable type is quietly never sent.
  uint16_t interval = ms_to_adv_units(cfg.interval_ms);
  Address none;
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> steps = {
      {kOpLeSetAdvParams, le_set_adv_params(interval, interval, cfg.type,
                                            own_addr_type_, none)},
      {kOpLeSetAdvData, data},
  };
  if (!cfg.scan_response.empty()) {
    std::vector<uint8_t> rsp;
    le_set_scan_response_data(cfg.scan_response, &rsp);
    steps.push_back({kOpLeSetScanResponseData, rsp});
  }
  steps.push_back({kOpLeSetAdvEnable, le_set_adv_enable(true)});

  command_sequence(std::move(steps),
                   [this, done](bool ok, const std::string& err) {
                     if (ok) advertising_ = true;
                     if (done) done(ok, err);
                   });
}

void Link::stop_advertising(DoneHandler done) {
  if (!advertising_) {
    if (done) defer([done] { done(true, std::string()); });
    return;
  }
  command_sequence({{kOpLeSetAdvEnable, le_set_adv_enable(false)}},
                   [this, done](bool ok, const std::string& err) {
                     advertising_ = false;
                     if (done) done(ok, err);
                   });
}

// ------------------------------------------------------------ connections

void Link::connect(const Address& peer, double timeout, ConnectHandler done) {
  if (closed_) {
    if (done) defer([done] { done(false, 0, kLinkClosed); });
    return;
  }
  if (connect_pending_) {
    if (done) {
      defer([done] {
        done(false, 0,
             "a connection attempt is already in progress; a controller has"
             " one initiator");
      });
    }
    return;
  }

  connect_pending_ = true;
  connect_peer_ = peer;
  connect_done_ = std::move(done);
  connect_restore_scan_ = scanning_;

  // A controller cannot scan and initiate at the same time, and the failure if
  // it is asked to is a bare "command disallowed".
  stop_scan([this, peer, timeout](bool ok, const std::string& err) {
    if (!connect_pending_) return;  // cancelled underneath us
    if (!ok) {
      finish_connect(false, 0, "could not stop scanning to connect: " + err);
      return;
    }
    std::vector<uint8_t> params = le_create_connection(
        peer, ms_to_scan_units(60), ms_to_scan_units(30), ms_to_conn_units(30),
        ms_to_conn_units(50), 0, ms_to_supervision_units(4000),
        own_addr_type_);

    // LE Create Connection answers with a Command Status; the connection
    // itself arrives later as an LE Connection Complete.
    command(kOpLeCreateConnection, params,
            [this, timeout](bool sent, const CommandComplete&,
                            const std::string& cmd_err) {
              if (!connect_pending_) return;
              if (!sent) {
                finish_connect(false, 0, cmd_err);
                return;
              }
              connect_timer_ = loop_->after(timeout, [this] {
                connect_timer_ = kNoTimer;
                if (!connect_pending_) return;
                // The controller is still trying. Cancelling is not optional:
                // an initiator left running blocks every later scan with
                // "command disallowed".
                command(kOpLeCreateConnectionCancel, {}, nullptr);
                finish_connect(false, 0,
                               "connection to " +
                                   address_to_string(connect_peer_) +
                                   " timed out");
              });
            });
  });
}

void Link::finish_connect(bool ok, uint16_t handle, const std::string& err) {
  if (!connect_pending_) return;
  loop_->cancel(connect_timer_);
  connect_timer_ = kNoTimer;
  connect_pending_ = false;
  ConnectHandler done = std::move(connect_done_);
  connect_done_ = nullptr;

  // Put the radio back the way it was found. A caller that was scanning before
  // it tried to connect is still expecting reports afterwards, whether or not
  // the connection happened.
  if (connect_restore_scan_ && !ok && !closed_) {
    start_scan(false, false, nullptr, nullptr);
  }
  connect_restore_scan_ = false;

  if (done) done(ok, handle, err);
}

void Link::disconnect(uint16_t handle, uint8_t reason) {
  if (closed_ || !conns_.count(handle)) return;
  command(kOpDisconnect, disconnect_params(handle, reason), nullptr);
}

std::vector<Link::Conn> Link::connections() const {
  std::vector<Conn> out;
  out.reserve(conns_.size());
  for (const auto& kv : conns_) out.push_back(kv.second);
  return out;
}

// --------------------------------------------------------------------- SMP

bool Link::encrypted(uint16_t conn) const {
  auto it = encrypted_.find(conn);
  return it != encrypted_.end() && it->second;
}

void Link::start_encryption(uint16_t conn, const std::array<uint8_t, 16>& ltk,
                            uint16_t ediv, const std::array<uint8_t, 8>& rand,
                            double timeout, DoneHandler done) {
  if (closed_ || !conns_.count(conn)) {
    std::string why = closed_ ? kLinkClosed : "not connected";
    if (done) defer([done, why] { done(false, why); });
    return;
  }
  if (encrypt_pending_) {
    if (done) {
      defer([done] { done(false, "encryption is already being started"); });
    }
    return;
  }

  encrypt_pending_ = true;
  encrypt_done_ = std::move(done);

  // handle, Rand, EDIV, LTK -- all little-endian, so the key goes on the wire
  // reversed from the order crypto.h computes it in.
  std::vector<uint8_t> params;
  params.push_back(static_cast<uint8_t>(conn & 0xff));
  params.push_back(static_cast<uint8_t>(conn >> 8));
  params.insert(params.end(), rand.begin(), rand.end());
  params.push_back(static_cast<uint8_t>(ediv & 0xff));
  params.push_back(static_cast<uint8_t>(ediv >> 8));
  for (size_t i = 0; i < 16; ++i) params.push_back(ltk[15 - i]);

  command(kOpLeStartEncryption, params,
          [this, timeout](bool ok, const CommandComplete&,
                          const std::string& err) {
            if (!encrypt_pending_) return;
            if (!ok) {
              finish_encryption(false, err);
              return;
            }
            encrypt_timer_ = loop_->after(timeout, [this] {
              encrypt_timer_ = kNoTimer;
              if (!encrypt_pending_) return;
              finish_encryption(
                  false, "the controller never reported encryption starting");
            });
          });
}

void Link::finish_encryption(bool ok, const std::string& err) {
  if (!encrypt_pending_) return;
  loop_->cancel(encrypt_timer_);
  encrypt_timer_ = kNoTimer;
  encrypt_pending_ = false;
  DoneHandler done = std::move(encrypt_done_);
  encrypt_done_ = nullptr;
  if (done) done(ok, err);
}

}  // namespace hci
}  // namespace octo
