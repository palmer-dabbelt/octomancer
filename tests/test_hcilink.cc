// The HCI host, driven against a controller that is a script.
//
// src/hciport.h has said since it was written that Port is virtual "so a test
// can substitute one without a dongle -- the HCI host above is a state machine,
// and a state machine that can only be exercised through real hardware is one
// that gets exercised rarely". It was exercised rarely. Nothing here had a test
// at all, because the host needed a reader thread to make progress and a thread
// needs real time to run in.
//
// With the loop the thread is gone, so both halves of the problem are gone with
// it: the clock is a variable, the port is a vector of bytes, and a connection,
// a pairing and a timed-out request all happen in no wall-clock time. What is
// pinned below is the behaviour that used to be defended by four mutexes and is
// now defended by queues -- and the flow control, which is the one thing here
// that fails silently when it is wrong.
//
// doc/dongle-notes.md still says connecting and writing a clock over a dongle
// have never been tried against real hardware. That is still true. What these
// tests remove is the other half of the doubt: when it is tried, a failure is
// the controller's behaviour rather than this host's arithmetic.

#include <cstring>
#include <string>
#include <vector>

#include "att.h"
#include "harness.h"
#include "hci.h"
#include "hcilink.h"
#include "loopfake.h"

using octo::FakeLoop;
using octo::Handle;
using octo::kRead;
using octo::hci::Link;
using octo::hci::Port;

namespace {

// ------------------------------------------------------------- the fixture

// A port that is two byte vectors. Everything the host writes lands in `tx`;
// everything the test wants the host to see is appended to `rx`.
class FakePort : public Port {
 public:
  Handle handle() const override {
    return Handle{-1, const_cast<FakePort*>(this)};
  }

  int read(uint8_t* buf, size_t len, double) override {
    if (broken_) return -1;
    if (rx_.empty()) return 0;
    size_t n = len < rx_.size() ? len : rx_.size();
    std::memcpy(buf, rx_.data(), n);
    rx_.erase(rx_.begin(), rx_.begin() + n);
    return static_cast<int>(n);
  }

  bool write(const uint8_t* data, size_t len) override {
    if (write_fails_) return false;
    tx_.insert(tx_.end(), data, data + len);
    return true;
  }

  void close() override { closed_ = true; }
  bool is_open() const override { return !closed_; }
  std::string name() const override { return "fake"; }

  void feed(const std::vector<uint8_t>& bytes) {
    rx_.insert(rx_.end(), bytes.begin(), bytes.end());
  }
  void unplug() { broken_ = true; }
  void break_writes() { write_fails_ = true; }
  bool closed() const { return closed_; }

  std::vector<uint8_t> tx_;

 private:
  std::vector<uint8_t> rx_;
  bool broken_ = false;
  bool write_fails_ = false;
  bool closed_ = false;
};

struct Command {
  uint16_t opcode = 0;
  std::vector<uint8_t> params;
};

// The loop, the port and the link, plus the handful of verbs a test needs.
struct Fixture {
  FakeLoop loop;
  FakePort* port = nullptr;
  std::unique_ptr<Link> link;
  bool ready_called = false;
  bool ready_ok = false;
  std::string ready_err;
  std::string closed_why;

  explicit Fixture(const Link::Options& opts = Link::Options()) {
    std::unique_ptr<FakePort> owned(new FakePort());
    port = owned.get();
    link = Link::attach(&loop, std::move(owned), opts,
                        [this](bool ok, const std::string& err) {
                          ready_called = true;
                          ready_ok = ok;
                          ready_err = err;
                        });
    link->set_closed_handler(
        [this](const std::string& why) { closed_why = why; });
  }

  // Let the loop run without moving time in any way a timer would notice.
  void settle() { loop.advance(0.0005); }

  // Hand the host bytes and tell the loop the port is readable.
  void deliver(const std::vector<uint8_t>& bytes) {
    port->feed(bytes);
    loop.deliver(loop.last_source(), kRead, 0.0);
    settle();
  }

  // Every command written since the last call, in order.
  std::vector<Command> commands() {
    std::vector<Command> out;
    std::vector<octo::hci::Packet> packets;
    size_t used = octo::hci::parse_stream(port->tx_.data(), port->tx_.size(),
                                          &packets);
    port->tx_.erase(port->tx_.begin(), port->tx_.begin() + used);
    for (const auto& p : packets) {
      if (p.type != octo::hci::kPacketCommand || p.payload.size() < 3) continue;
      Command c;
      c.opcode = static_cast<uint16_t>(p.payload[0] | (p.payload[1] << 8));
      c.params.assign(p.payload.begin() + 3, p.payload.end());
      out.push_back(c);
    }
    return out;
  }

  // Every ACL packet written since the last call.
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> acl() {
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> out;
    std::vector<octo::hci::Packet> packets;
    size_t used = octo::hci::parse_stream(port->tx_.data(), port->tx_.size(),
                                          &packets);
    port->tx_.erase(port->tx_.begin(), port->tx_.begin() + used);
    for (const auto& p : packets) {
      if (p.type != octo::hci::kPacketAclData) continue;
      octo::hci::AclHeader hdr;
      std::vector<uint8_t> data;
      if (octo::hci::parse_acl(p.payload, &hdr, &data)) {
        out.push_back({hdr.handle, data});
      }
    }
    return out;
  }
};

// --------------------------------------------------------- packet builders

std::vector<uint8_t> event(uint8_t code, const std::vector<uint8_t>& params) {
  std::vector<uint8_t> out{octo::hci::kPacketEvent, code,
                           static_cast<uint8_t>(params.size())};
  out.insert(out.end(), params.begin(), params.end());
  return out;
}

// `ret` is the command's return parameters, status included -- which is how
// they arrive on the wire and how parse_command_complete reads them.
std::vector<uint8_t> complete(uint16_t opcode, const std::vector<uint8_t>& ret) {
  std::vector<uint8_t> p{1, static_cast<uint8_t>(opcode & 0xff),
                         static_cast<uint8_t>(opcode >> 8)};
  p.insert(p.end(), ret.begin(), ret.end());
  return event(octo::hci::kEvtCommandComplete, p);
}

std::vector<uint8_t> complete_ok(uint16_t opcode) {
  return complete(opcode, {octo::hci::kSuccess});
}

std::vector<uint8_t> command_status(uint16_t opcode, uint8_t status) {
  return event(octo::hci::kEvtCommandStatus,
               {status, 1, static_cast<uint8_t>(opcode & 0xff),
                static_cast<uint8_t>(opcode >> 8)});
}

std::vector<uint8_t> connection_complete(uint8_t status, uint16_t handle,
                                         uint8_t role = 0) {
  std::vector<uint8_t> p{octo::hci::kLeConnectionComplete,
                         status,
                         static_cast<uint8_t>(handle & 0xff),
                         static_cast<uint8_t>(handle >> 8),
                         role,
                         0};
  for (int i = 0; i < 6; ++i) p.push_back(static_cast<uint8_t>(0x10 + i));
  p.insert(p.end(), {0x18, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x00});
  return event(octo::hci::kEvtLeMeta, p);
}

std::vector<uint8_t> num_completed(uint16_t handle, uint16_t count) {
  return event(octo::hci::kEvtNumCompletedPackets,
               {1, static_cast<uint8_t>(handle & 0xff),
                static_cast<uint8_t>(handle >> 8),
                static_cast<uint8_t>(count & 0xff),
                static_cast<uint8_t>(count >> 8)});
}

std::vector<uint8_t> disconnection(uint16_t handle, uint8_t reason) {
  return event(octo::hci::kEvtDisconnectionComplete,
               {octo::hci::kSuccess, static_cast<uint8_t>(handle & 0xff),
                static_cast<uint8_t>(handle >> 8), reason});
}

// An ATT PDU arriving from the peer, framed as L2CAP inside one ACL packet.
std::vector<uint8_t> att_in(uint16_t handle, const std::vector<uint8_t>& pdu) {
  std::vector<uint8_t> l2cap = octo::att::build_l2cap(octo::att::kCidAtt, pdu);
  return octo::hci::build_acl(handle, octo::hci::kAclFirstFlushable,
                              l2cap.data(), l2cap.size());
}

// ----------------------------------------------------------- bringing up

// Answer whatever the host asks during init, so that a test which is about
// something else can get to it in two lines. `bd_addr_zero` is the nRF52840's
// actual behaviour and therefore the default.
void bring_up(Fixture& f, bool bd_addr_zero = true, uint16_t acl_payload = 27,
              uint8_t acl_total = 4) {
  for (int round = 0; round < 24 && !f.ready_called; ++round) {
    std::vector<Command> cmds = f.commands();
    if (cmds.empty()) break;
    for (const Command& c : cmds) {
      switch (c.opcode) {
        case octo::hci::kOpReadLocalVersion:
          f.deliver(complete(c.opcode, {octo::hci::kSuccess, 0x0c, 0x00, 0x00,
                                        0x0c, 0x59, 0x00, 0x34, 0x12}));
          break;
        case octo::hci::kOpLeReadBufferSize:
          f.deliver(complete(c.opcode,
                             {octo::hci::kSuccess,
                              static_cast<uint8_t>(acl_payload & 0xff),
                              static_cast<uint8_t>(acl_payload >> 8),
                              acl_total}));
          break;
        case octo::hci::kOpReadBdAddr: {
          std::vector<uint8_t> ret{octo::hci::kSuccess};
          for (int i = 0; i < 6; ++i) {
            ret.push_back(bd_addr_zero ? 0x00 : static_cast<uint8_t>(0xa0 + i));
          }
          f.deliver(complete(c.opcode, ret));
          break;
        }
        case octo::hci::kOpLeRand: {
          std::vector<uint8_t> ret{octo::hci::kSuccess};
          for (int i = 0; i < 8; ++i) ret.push_back(static_cast<uint8_t>(i + 1));
          f.deliver(complete(c.opcode, ret));
          break;
        }
        default:
          f.deliver(complete_ok(c.opcode));
          break;
      }
    }
  }
}

// Answer every command the host has written, and keep answering until it stops
// writing them. One command is in flight at a time, so a two-command sequence
// only puts its second command on the wire once the first has been answered --
// draining once would leave half a sequence outstanding. Returns the opcodes
// answered, in order.
std::vector<uint16_t> answer_all(Fixture& f) {
  std::vector<uint16_t> seen;
  for (int round = 0; round < 16; ++round) {
    std::vector<Command> cmds = f.commands();
    if (cmds.empty()) break;
    for (const Command& c : cmds) {
      seen.push_back(c.opcode);
      // LE Create Connection answers with a Command Status; the connection
      // itself arrives later as its own event.
      if (c.opcode == octo::hci::kOpLeCreateConnection) {
        f.deliver(command_status(c.opcode, octo::hci::kSuccess));
      } else {
        f.deliver(complete_ok(c.opcode));
      }
    }
  }
  return seen;
}

bool saw(const std::vector<uint16_t>& ops, uint16_t opcode) {
  for (uint16_t o : ops) {
    if (o == opcode) return true;
  }
  return false;
}

// A brought-up link with one connection on handle 0x0040.
void bring_up_connected(Fixture& f, uint16_t handle = 0x0040) {
  bring_up(f);
  bool done = false;
  f.link->connect(octo::hci::Address(), 10.0,
                  [&](bool ok, uint16_t, const std::string&) { done = ok; });
  f.settle();
  for (const Command& c : f.commands()) {
    if (c.opcode == octo::hci::kOpLeCreateConnection) {
      f.deliver(command_status(c.opcode, octo::hci::kSuccess));
    }
  }
  f.deliver(connection_complete(octo::hci::kSuccess, handle));
  CHECK(done);
  f.commands();
  f.acl();
}

// ------------------------------------------------------------------ tests

void test_bring_up_installs_a_random_static_address() {
  // The nRF52840 has no public address, so Read BD_ADDR answers with zeros and
  // every attempt to use a "public" address is then rejected with a status
  // that says nothing about addresses. doc/dongle-notes.md records this as the
  // workaround that made scanning work at all; this is what pins it.
  Fixture f;
  bring_up(f);
  CHECK(f.ready_called);
  CHECK(f.ready_ok);
  CHECK_STR(f.ready_err, "");

  octo::hci::Address local = f.link->local_address();
  CHECK_EQ(local.type, static_cast<uint8_t>(octo::hci::kAddrRandom));
  // The top two bits must be 11 or the controller calls the address malformed.
  CHECK_EQ(local.bytes[0] & 0xc0, 0xc0);
}

void test_a_public_address_is_used_as_is() {
  Fixture f;
  bring_up(f, /*bd_addr_zero=*/false);
  CHECK(f.ready_ok);
  CHECK_EQ(f.link->local_address().type,
           static_cast<uint8_t>(octo::hci::kAddrPublic));
  // Nothing random was asked for: the controller already had an identity.
  for (const Command& c : f.commands()) {
    CHECK(c.opcode != octo::hci::kOpLeSetRandomAddress);
  }
}

void test_bring_up_stops_at_the_first_refusal() {
  Fixture f;
  std::vector<Command> cmds = f.commands();
  CHECK_EQ(cmds.size(), static_cast<size_t>(1));
  CHECK_EQ(cmds[0].opcode, static_cast<uint16_t>(octo::hci::kOpReset));

  f.deliver(complete(octo::hci::kOpReset, {octo::hci::kCommandDisallowed}));
  CHECK(f.ready_called);
  CHECK(!f.ready_ok);
  CHECK(!f.ready_err.empty());
  // And it did not carry on regardless.
  CHECK_EQ(f.commands().size(), static_cast<size_t>(0));
}

void test_a_completion_never_runs_inside_the_call_that_made_it() {
  // The rule hcilink.h promises. Without it a caller that fails fast is
  // re-entered from inside its own call, which is exactly the class of bug the
  // old blocking API could not have and the new one could.
  Fixture f;
  bring_up(f);

  bool inside = false;
  bool called = false;
  // Not connected, so this can only fail -- and it must still fail later.
  f.link->att_request(0x0040, octo::att::read_request(1), 5.0,
                      [&](bool ok, const std::vector<uint8_t>&,
                          const std::string&) {
                        called = true;
                        CHECK(!ok);
                        CHECK(inside);
                      });
  CHECK(!called);  // nothing happened during the call itself
  inside = true;
  f.settle();
  CHECK(called);
}

void test_only_one_command_is_in_flight_at_a_time() {
  Fixture f;
  bring_up(f);

  std::vector<int> order;
  f.link->command(octo::hci::kOpLeReadLocalFeatures, {},
                  [&](bool ok, const octo::hci::CommandComplete&,
                      const std::string&) {
                    if (ok) order.push_back(1);
                  });
  f.link->command(octo::hci::kOpReadLocalCommands, {},
                  [&](bool ok, const octo::hci::CommandComplete&,
                      const std::string&) {
                    if (ok) order.push_back(2);
                  });
  f.settle();

  std::vector<Command> cmds = f.commands();
  CHECK_EQ(cmds.size(), static_cast<size_t>(1));
  CHECK_EQ(cmds[0].opcode,
           static_cast<uint16_t>(octo::hci::kOpLeReadLocalFeatures));

  f.deliver(complete_ok(octo::hci::kOpLeReadLocalFeatures));
  cmds = f.commands();
  CHECK_EQ(cmds.size(), static_cast<size_t>(1));
  CHECK_EQ(cmds[0].opcode,
           static_cast<uint16_t>(octo::hci::kOpReadLocalCommands));

  f.deliver(complete_ok(octo::hci::kOpReadLocalCommands));
  CHECK_EQ(order.size(), static_cast<size_t>(2));
  CHECK_EQ(order[0], 1);
  CHECK_EQ(order[1], 2);
}

void test_a_command_that_is_never_answered_times_out_and_frees_the_queue() {
  Link::Options opts;
  opts.command_timeout = 3.0;
  Fixture f(opts);
  bring_up(f);

  bool first_failed = false;
  bool second_ran = false;
  f.link->command(octo::hci::kOpLeReadLocalFeatures, {},
                  [&](bool ok, const octo::hci::CommandComplete&,
                      const std::string& err) {
                    first_failed = !ok && !err.empty();
                  });
  f.link->command(octo::hci::kOpReadLocalCommands, {},
                  [&](bool, const octo::hci::CommandComplete&,
                      const std::string&) { second_ran = true; });
  f.settle();
  f.commands();

  f.loop.advance(2.0);
  CHECK(!first_failed);  // not early

  f.loop.advance(2.0);
  CHECK(first_failed);
  // The queue moved on rather than wedging behind a controller that went quiet.
  std::vector<Command> cmds = f.commands();
  CHECK_EQ(cmds.size(), static_cast<size_t>(1));
  CHECK_EQ(cmds[0].opcode,
           static_cast<uint16_t>(octo::hci::kOpReadLocalCommands));
  CHECK(!second_ran);
}

void test_a_command_status_failure_is_the_whole_answer() {
  Fixture f;
  bring_up(f);
  bool failed = false;
  std::string why;
  f.link->command(octo::hci::kOpLeCreateConnection, {},
                  [&](bool ok, const octo::hci::CommandComplete&,
                      const std::string& err) {
                    failed = !ok;
                    why = err;
                  });
  f.settle();
  f.commands();
  f.deliver(command_status(octo::hci::kOpLeCreateConnection,
                           octo::hci::kCommandDisallowed));
  CHECK(failed);
  CHECK(!why.empty());
}

void test_an_att_response_completes_its_request() {
  Fixture f;
  bring_up_connected(f);

  bool got = false;
  std::vector<uint8_t> value;
  f.link->att_request(0x0040, octo::att::read_request(0x0021), 5.0,
                      [&](bool ok, const std::vector<uint8_t>& rsp,
                          const std::string&) {
                        got = ok;
                        value = rsp;
                      });
  f.settle();
  CHECK_EQ(f.acl().size(), static_cast<size_t>(1));

  f.deliver(att_in(0x0040, {octo::att::kReadResponse, 0xaa, 0xbb}));
  CHECK(got);
  CHECK_EQ(value.size(), static_cast<size_t>(3));
  CHECK_EQ(value[0], static_cast<uint8_t>(octo::att::kReadResponse));
}

void test_a_notification_does_not_complete_an_outstanding_request() {
  // Notifications arrive on the same channel as responses. Letting one satisfy
  // a pending request is what makes a subscribed characteristic look like a
  // stuck read -- and the answer it produces is another attribute's value.
  //
  // Honest note: today this also holds for a second reason, which is that no
  // defined ATT request is numbered one below 0x1b. So removing the explicit
  // guard in dispatch_acl does not make this test fail. It is pinned anyway
  // because it is the behaviour callers depend on, and the test below is the
  // one that has teeth against the matching rule itself.
  Fixture f;
  bring_up_connected(f);

  int notifications = 0;
  f.link->set_att_handler(
      [&](uint16_t, const std::vector<uint8_t>&) { ++notifications; });

  bool completed = false;
  f.link->att_request(0x0040, octo::att::read_request(0x0021), 5.0,
                      [&](bool, const std::vector<uint8_t>&,
                          const std::string&) { completed = true; });
  f.settle();
  f.acl();

  f.deliver(att_in(0x0040, {octo::att::kHandleValueNotification, 0x10, 0x00,
                            0x01, 0x02}));
  CHECK_EQ(notifications, 1);
  CHECK(!completed);

  // The real response still lands.
  f.deliver(att_in(0x0040, {octo::att::kReadResponse, 0x99}));
  CHECK(completed);
  CHECK_EQ(notifications, 1);
}

void test_a_mismatched_response_does_not_complete_a_request() {
  // The rule that actually does the work: a response completes a request only
  // when it is that request's response. A camera that answers late, or a
  // server PDU arriving while a request is outstanding, must not be handed
  // back to the caller as the answer it was waiting for -- it would be another
  // attribute's value, reported as this attribute's.
  Fixture f;
  bring_up_connected(f);

  int stray = 0;
  f.link->set_att_handler(
      [&](uint16_t, const std::vector<uint8_t>&) { ++stray; });

  bool completed = false;
  std::vector<uint8_t> got;
  f.link->att_request(0x0040, octo::att::read_request(0x0021), 5.0,
                      [&](bool, const std::vector<uint8_t>& rsp,
                          const std::string&) {
                        completed = true;
                        got = rsp;
                      });
  f.settle();
  f.acl();

  // A Write Response, while a Read Request is outstanding.
  f.deliver(att_in(0x0040, {octo::att::kWriteResponse}));
  CHECK(!completed);
  CHECK_EQ(stray, 1);

  // An Error Response naming a different request is equally not the answer.
  f.deliver(att_in(0x0040, {octo::att::kErrorResponse,
                            octo::att::kWriteRequest, 0x21, 0x00, 0x03}));
  CHECK(!completed);
  CHECK_EQ(stray, 2);

  // The real one still lands.
  f.deliver(att_in(0x0040, {octo::att::kReadResponse, 0x77}));
  CHECK(completed);
  CHECK_EQ(got.size(), static_cast<size_t>(2));
  CHECK_EQ(got[0], static_cast<uint8_t>(octo::att::kReadResponse));
}

void test_an_error_response_is_an_answer_not_a_failure() {
  // "attribute not found" is the normal way a walk over a handle range ends.
  // Reporting it as a transport failure would make discovery look broken.
  Fixture f;
  bring_up_connected(f);

  bool ok_flag = false;
  std::vector<uint8_t> value;
  f.link->att_request(0x0040, octo::att::read_request(0x0021), 5.0,
                      [&](bool ok, const std::vector<uint8_t>& rsp,
                          const std::string&) {
                        ok_flag = ok;
                        value = rsp;
                      });
  f.settle();
  f.acl();

  f.deliver(att_in(0x0040, {octo::att::kErrorResponse, octo::att::kReadRequest,
                            0x21, 0x00, 0x0a}));
  CHECK(ok_flag);
  CHECK_EQ(value.size(), static_cast<size_t>(5));
  CHECK_EQ(value[0], static_cast<uint8_t>(octo::att::kErrorResponse));
}

void test_att_requests_on_one_connection_are_serialised() {
  // The protocol's rule: one outstanding request per direction. Two in flight
  // and the responses cannot be told apart.
  Fixture f;
  bring_up_connected(f);

  std::vector<int> order;
  f.link->att_request(0x0040, octo::att::read_request(0x0021), 5.0,
                      [&](bool, const std::vector<uint8_t>&,
                          const std::string&) { order.push_back(1); });
  f.link->att_request(0x0040, octo::att::read_request(0x0022), 5.0,
                      [&](bool, const std::vector<uint8_t>&,
                          const std::string&) { order.push_back(2); });
  f.settle();
  CHECK_EQ(f.acl().size(), static_cast<size_t>(1));

  f.deliver(att_in(0x0040, {octo::att::kReadResponse, 0x01}));
  CHECK_EQ(order.size(), static_cast<size_t>(1));
  CHECK_EQ(f.acl().size(), static_cast<size_t>(1));

  f.deliver(att_in(0x0040, {octo::att::kReadResponse, 0x02}));
  CHECK_EQ(order.size(), static_cast<size_t>(2));
  CHECK_EQ(order[1], 2);
}

void test_two_connections_each_get_a_request_in_flight() {
  // The old code held one mutex across every connection, which was stricter
  // than ATT requires and made a slow camera stall a fast one.
  Fixture f;
  bring_up_connected(f, 0x0040);
  bool second = false;
  f.link->connect(octo::hci::Address(), 10.0,
                  [&](bool ok, uint16_t, const std::string&) { second = ok; });
  f.settle();
  for (const Command& c : f.commands()) {
    if (c.opcode == octo::hci::kOpLeCreateConnection) {
      f.deliver(command_status(c.opcode, octo::hci::kSuccess));
    }
  }
  f.deliver(connection_complete(octo::hci::kSuccess, 0x0041));
  CHECK(second);
  f.acl();

  f.link->att_request(0x0040, octo::att::read_request(0x0021), 5.0, nullptr);
  f.link->att_request(0x0041, octo::att::read_request(0x0021), 5.0, nullptr);
  f.settle();

  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> sent = f.acl();
  CHECK_EQ(sent.size(), static_cast<size_t>(2));
  if (sent.size() == 2) {
    CHECK(sent[0].first != sent[1].first);
  }
}

void test_acl_waits_for_a_controller_buffer_and_resumes_on_credit() {
  // The one piece of flow control that fails silently: a packet sent without a
  // credit is not refused, it is dropped, and the peer just stops answering.
  Fixture f;
  // One buffer, and a payload small enough that a modest PDU needs two.
  bring_up(f, true, /*acl_payload=*/27, /*acl_total=*/1);
  f.link->connect(octo::hci::Address(), 10.0, nullptr);
  f.settle();
  for (const Command& c : f.commands()) {
    if (c.opcode == octo::hci::kOpLeCreateConnection) {
      f.deliver(command_status(c.opcode, octo::hci::kSuccess));
    }
  }
  f.deliver(connection_complete(octo::hci::kSuccess, 0x0040));
  f.acl();

  std::vector<uint8_t> big(60, 0x5a);
  std::string err;
  CHECK(f.link->send_att(0x0040, big, &err));
  f.settle();

  // Exactly one fragment went out: the controller has one buffer.
  CHECK_EQ(f.acl().size(), static_cast<size_t>(1));
  f.loop.advance(5.0);
  CHECK_EQ(f.acl().size(), static_cast<size_t>(0));  // and waiting is not a timeout

  f.deliver(num_completed(0x0040, 1));
  CHECK_EQ(f.acl().size(), static_cast<size_t>(1));
}

void test_a_backed_up_queue_is_refused_rather_than_grown() {
  // 256 KB of RAM on the box. An unbounded queue behind a controller that has
  // stopped granting credits is a reboot, not a slow program.
  Fixture f;
  bring_up(f, true, 27, 1);
  f.link->connect(octo::hci::Address(), 10.0, nullptr);
  f.settle();
  for (const Command& c : f.commands()) {
    if (c.opcode == octo::hci::kOpLeCreateConnection) {
      f.deliver(command_status(c.opcode, octo::hci::kSuccess));
    }
  }
  f.deliver(connection_complete(octo::hci::kSuccess, 0x0040));
  f.acl();

  std::vector<uint8_t> pdu(20, 0x11);
  bool refused = false;
  std::string err;
  for (int i = 0; i < 200 && !refused; ++i) {
    if (!f.link->send_att(0x0040, pdu, &err)) refused = true;
    f.settle();
  }
  CHECK(refused);
  CHECK(!err.empty());
  // Refused, not closed: the link is still usable once the peer catches up.
  CHECK(!f.link->closed());
}

void test_a_disconnect_fails_outstanding_requests_at_once() {
  Fixture f;
  bring_up_connected(f);

  bool failed = false;
  std::string why;
  f.link->att_request(0x0040, octo::att::read_request(0x0021), 30.0,
                      [&](bool ok, const std::vector<uint8_t>&,
                          const std::string& err) {
                        failed = !ok;
                        why = err;
                      });
  f.settle();
  f.acl();

  f.deliver(disconnection(0x0040, octo::hci::kRemoteUserTerminated));
  // Immediately, not after the thirty-second timeout the caller asked for.
  CHECK(failed);
  CHECK(!why.empty());
  CHECK(!f.link->connected(0x0040));
}

void test_fragments_for_a_dead_connection_are_dropped() {
  // Handles are reused. A fragment queued for a connection that has gone would
  // otherwise be delivered to whichever connection inherits the number.
  Fixture f;
  bring_up(f, true, 27, 1);
  f.link->connect(octo::hci::Address(), 10.0, nullptr);
  f.settle();
  for (const Command& c : f.commands()) {
    if (c.opcode == octo::hci::kOpLeCreateConnection) {
      f.deliver(command_status(c.opcode, octo::hci::kSuccess));
    }
  }
  f.deliver(connection_complete(octo::hci::kSuccess, 0x0040));
  f.acl();

  std::vector<uint8_t> big(60, 0x5a);
  CHECK(f.link->send_att(0x0040, big, nullptr));
  f.settle();
  CHECK_EQ(f.acl().size(), static_cast<size_t>(1));  // one out, the rest queued

  f.deliver(disconnection(0x0040, octo::hci::kRemoteUserTerminated));
  f.deliver(num_completed(0x0040, 1));
  // The credit came back, but there is nothing left to send it to.
  CHECK_EQ(f.acl().size(), static_cast<size_t>(0));
}

void test_the_port_dying_fails_everything_outstanding() {
  Fixture f;
  bring_up_connected(f);

  bool att_failed = false, cmd_failed = false;
  f.link->att_request(0x0040, octo::att::read_request(0x0021), 30.0,
                      [&](bool ok, const std::vector<uint8_t>&,
                          const std::string&) { att_failed = !ok; });
  f.link->command(octo::hci::kOpLeReadLocalFeatures, {},
                  [&](bool ok, const octo::hci::CommandComplete&,
                      const std::string&) { cmd_failed = !ok; });
  f.settle();

  f.port->unplug();
  f.loop.deliver(f.loop.last_source(), kRead, 0.0);
  f.settle();

  CHECK(f.link->closed());
  CHECK(att_failed);
  CHECK(cmd_failed);
  CHECK(!f.closed_why.empty());
  CHECK(f.port->closed());
}

void test_connecting_stops_scanning_and_puts_it_back_on_failure() {
  // A controller cannot scan and initiate at once, and an initiator left
  // running blocks every later scan with a bare "command disallowed".
  Fixture f;
  bring_up(f);
  f.link->start_scan(false, false, [](const octo::hci::AdvReport&) {}, nullptr);
  f.settle();
  answer_all(f);
  CHECK(f.link->scanning());

  bool failed = false;
  f.link->connect(octo::hci::Address(), 4.0,
                  [&](bool ok, uint16_t, const std::string&) { failed = !ok; });
  f.settle();

  std::vector<uint16_t> ops = answer_all(f);
  CHECK(saw(ops, octo::hci::kOpLeSetScanEnable));
  CHECK(saw(ops, octo::hci::kOpLeCreateConnection));
  CHECK(!f.link->scanning());
  CHECK(!failed);

  // Nobody answers. The attempt must be cancelled, not abandoned.
  f.loop.advance(5.0);
  CHECK(failed);
  std::vector<uint16_t> after = answer_all(f);
  CHECK(saw(after, octo::hci::kOpLeCreateConnectionCancel));
  // And the radio is put back the way it was found.
  CHECK(saw(after, octo::hci::kOpLeSetScanParams));
  CHECK(f.link->scanning());
}

void test_a_second_connect_is_refused_while_one_is_outstanding() {
  Fixture f;
  bring_up(f);
  f.link->connect(octo::hci::Address(), 10.0, nullptr);
  f.settle();

  bool refused = false;
  std::string why;
  f.link->connect(octo::hci::Address(), 10.0,
                  [&](bool ok, uint16_t, const std::string& err) {
                    refused = !ok;
                    why = err;
                  });
  f.settle();
  CHECK(refused);
  CHECK(!why.empty());
}

void test_a_long_term_key_request_is_answered() {
  // Nothing here holds long term keys yet. Ignoring the request instead leaves
  // the peer waiting for an encrypted link that never starts.
  Fixture f;
  bring_up_connected(f);
  std::vector<uint8_t> p{octo::hci::kLeLongTermKeyRequest, 0x40, 0x00};
  for (int i = 0; i < 10; ++i) p.push_back(0);
  f.deliver(event(octo::hci::kEvtLeMeta, p));

  bool answered = false;
  for (const Command& c : f.commands()) {
    if (c.opcode == octo::hci::kOpLeLtkRequestNegReply) answered = true;
  }
  CHECK(answered);
}

void test_encryption_reports_the_controllers_verdict() {
  Fixture f;
  bring_up_connected(f);

  std::array<uint8_t, 16> ltk{};
  std::array<uint8_t, 8> rand{};
  bool done = false, ok_flag = false;
  std::string why;
  f.link->start_encryption(0x0040, ltk, 0, rand, 10.0,
                           [&](bool ok, const std::string& err) {
                             done = true;
                             ok_flag = ok;
                             why = err;
                           });
  f.settle();
  for (const Command& c : f.commands()) {
    if (c.opcode == octo::hci::kOpLeStartEncryption) {
      f.deliver(command_status(c.opcode, octo::hci::kSuccess));
    }
  }
  CHECK(!done);

  // "PIN or key missing" is what a wrong passkey looks like from here.
  f.deliver(event(octo::hci::kEvtEncryptionChange,
                  {octo::hci::kPinOrKeyMissing, 0x40, 0x00, 0x00}));
  CHECK(done);
  CHECK(!ok_flag);
  CHECK(why.find("passkey") != std::string::npos);
  CHECK(!f.link->encrypted(0x0040));
}

void test_encryption_succeeding_marks_the_connection() {
  Fixture f;
  bring_up_connected(f);
  std::array<uint8_t, 16> ltk{};
  std::array<uint8_t, 8> rand{};
  bool ok_flag = false;
  f.link->start_encryption(0x0040, ltk, 0, rand, 10.0,
                           [&](bool ok, const std::string&) { ok_flag = ok; });
  f.settle();
  for (const Command& c : f.commands()) {
    if (c.opcode == octo::hci::kOpLeStartEncryption) {
      f.deliver(command_status(c.opcode, octo::hci::kSuccess));
    }
  }
  f.deliver(event(octo::hci::kEvtEncryptionChange,
                  {octo::hci::kSuccess, 0x40, 0x00, 0x01}));
  CHECK(ok_flag);
  CHECK(f.link->encrypted(0x0040));
}

}  // namespace

int main() {
  test_bring_up_installs_a_random_static_address();
  test_a_public_address_is_used_as_is();
  test_bring_up_stops_at_the_first_refusal();
  test_a_completion_never_runs_inside_the_call_that_made_it();
  test_only_one_command_is_in_flight_at_a_time();
  test_a_command_that_is_never_answered_times_out_and_frees_the_queue();
  test_a_command_status_failure_is_the_whole_answer();
  test_an_att_response_completes_its_request();
  test_a_notification_does_not_complete_an_outstanding_request();
  test_a_mismatched_response_does_not_complete_a_request();
  test_an_error_response_is_an_answer_not_a_failure();
  test_att_requests_on_one_connection_are_serialised();
  test_two_connections_each_get_a_request_in_flight();
  test_acl_waits_for_a_controller_buffer_and_resumes_on_credit();
  test_a_backed_up_queue_is_refused_rather_than_grown();
  test_a_disconnect_fails_outstanding_requests_at_once();
  test_fragments_for_a_dead_connection_are_dropped();
  test_the_port_dying_fails_everything_outstanding();
  test_connecting_stops_scanning_and_puts_it_back_on_failure();
  test_a_second_connect_is_refused_while_one_is_outstanding();
  test_a_long_term_key_request_is_answered();
  test_encryption_reports_the_controllers_verdict();
  test_encryption_succeeding_marks_the_connection();
  return octotest::report("test_hcilink");
}
