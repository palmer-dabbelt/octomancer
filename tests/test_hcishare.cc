// One controller, several jobs, against a controller that is a script.
//
// What is pinned here is the arbitration -- who gets the scan, with which
// parameters, and what happens to it around a connection. That last one is the
// reason this file exists rather than a paragraph in doc/: the failure it
// prevents is silent. Two things opened the dongle, each saw the other's
// replies as corruption, and it presented as a radio that had powered itself
// off. Nothing in that failure mentions the port, and it cost an evening.
//
// The other half of what is pinned is the destruction order, which is the
// hazard this shape introduces in exchange. A User can outlive the SharedLink,
// a User can be destroyed inside another User's handler, and a fan-out already
// under way must survive both.

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "harness.h"
#include "hci.h"
#include "hcishare.h"
#include "loopfake.h"

using octo::FakeLoop;
using octo::Handle;
using octo::kRead;
using octo::hci::Link;
using octo::hci::Port;
using octo::hci::SharedLink;

namespace {

// ------------------------------------------------------------- the fixture

// The same two byte vectors tests/test_hcilink.cc uses. Copied rather than
// shared: a header holding one class used by two tests is a dependency between
// them, and the whole point of a fake port is that it is four methods.
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

  std::vector<uint8_t> tx_;

 private:
  std::vector<uint8_t> rx_;
  bool broken_ = false;
  bool closed_ = false;
};

struct Command {
  uint16_t opcode = 0;
  std::vector<uint8_t> params;
};

std::vector<uint8_t> event(uint8_t code, const std::vector<uint8_t>& params) {
  std::vector<uint8_t> out{octo::hci::kPacketEvent, code,
                           static_cast<uint8_t>(params.size())};
  out.insert(out.end(), params.begin(), params.end());
  return out;
}

std::vector<uint8_t> complete(uint16_t opcode, const std::vector<uint8_t>& ret) {
  std::vector<uint8_t> p{1, static_cast<uint8_t>(opcode & 0xff),
                         static_cast<uint8_t>(opcode >> 8)};
  p.insert(p.end(), ret.begin(), ret.end());
  return event(octo::hci::kEvtCommandComplete, p);
}

std::vector<uint8_t> complete_ok(uint16_t opcode) {
  return complete(opcode, {octo::hci::kSuccess});
}

std::vector<uint8_t> complete_fail(uint16_t opcode) {
  return complete(opcode, {0x0c});  // command disallowed
}

std::vector<uint8_t> command_status(uint16_t opcode, uint8_t status) {
  return event(octo::hci::kEvtCommandStatus,
               {status, 1, static_cast<uint8_t>(opcode & 0xff),
                static_cast<uint8_t>(opcode >> 8)});
}

std::vector<uint8_t> connection_complete(uint8_t status, uint16_t handle) {
  std::vector<uint8_t> p{octo::hci::kLeConnectionComplete,
                         status,
                         static_cast<uint8_t>(handle & 0xff),
                         static_cast<uint8_t>(handle >> 8),
                         0,
                         0};
  for (int i = 0; i < 6; ++i) p.push_back(static_cast<uint8_t>(0x10 + i));
  p.insert(p.end(), {0x18, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x00});
  return event(octo::hci::kEvtLeMeta, p);
}

std::vector<uint8_t> disconnection(uint16_t handle, uint8_t reason) {
  return event(octo::hci::kEvtDisconnectionComplete,
               {octo::hci::kSuccess, static_cast<uint8_t>(handle & 0xff),
                static_cast<uint8_t>(handle >> 8), reason});
}

// One advertisement, from a device whose address ends in `tag`.
std::vector<uint8_t> advertisement(uint8_t tag, int rssi) {
  std::vector<uint8_t> p{octo::hci::kLeAdvertisingReport, 1, 0x00, 0x00};
  for (int i = 0; i < 5; ++i) p.push_back(static_cast<uint8_t>(0x20 + i));
  p.push_back(tag);
  const std::vector<uint8_t> ad{0x02, 0x01, 0x06};
  p.push_back(static_cast<uint8_t>(ad.size()));
  p.insert(p.end(), ad.begin(), ad.end());
  p.push_back(static_cast<uint8_t>(rssi));
  return event(octo::hci::kEvtLeMeta, p);
}

struct Fixture {
  FakeLoop loop;
  FakePort* port = nullptr;
  std::unique_ptr<SharedLink> radio;

  Fixture() {
    std::unique_ptr<FakePort> owned(new FakePort());
    port = owned.get();
    radio = SharedLink::attach(&loop, std::move(owned), Link::Options());
  }

  void settle() { loop.advance(0.0005); }

  void deliver(const std::vector<uint8_t>& bytes) {
    port->feed(bytes);
    loop.deliver(loop.last_source(), kRead, 0.0);
    settle();
  }

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

  // Answer every command written, and keep answering until the host stops
  // writing them. One command is in flight at a time, so a two-command
  // sequence only puts its second on the wire once the first is answered.
  std::vector<uint16_t> answer_all() {
    std::vector<uint16_t> seen;
    for (int round = 0; round < 24; ++round) {
      std::vector<Command> cmds = commands();
      if (cmds.empty()) break;
      for (const Command& c : cmds) {
        seen.push_back(c.opcode);
        switch (c.opcode) {
          case octo::hci::kOpReadLocalVersion:
            deliver(complete(c.opcode, {octo::hci::kSuccess, 0x0c, 0x00, 0x00,
                                        0x0c, 0x59, 0x00, 0x34, 0x12}));
            break;
          case octo::hci::kOpLeReadBufferSize:
            deliver(complete(c.opcode, {octo::hci::kSuccess, 27, 0, 4}));
            break;
          case octo::hci::kOpReadBdAddr: {
            std::vector<uint8_t> ret{octo::hci::kSuccess};
            for (int i = 0; i < 6; ++i) ret.push_back(0x00);
            deliver(complete(c.opcode, ret));
            break;
          }
          case octo::hci::kOpLeRand: {
            std::vector<uint8_t> ret{octo::hci::kSuccess};
            for (int i = 0; i < 8; ++i) {
              ret.push_back(static_cast<uint8_t>(i + 1));
            }
            deliver(complete(c.opcode, ret));
            break;
          }
          case octo::hci::kOpLeCreateConnection:
            deliver(command_status(c.opcode, octo::hci::kSuccess));
            break;
          default:
            deliver(complete_ok(c.opcode));
            break;
        }
      }
    }
    return seen;
  }

  void bring_up() { answer_all(); }

  // Whether the last LE Set Scan Parameters asked for an active scan. The
  // first parameter byte is the scan type: 0 passive, 1 active.
  bool last_scan_was_active(const std::vector<Command>& cmds) const {
    bool active = false;
    for (const Command& c : cmds) {
      if (c.opcode == octo::hci::kOpLeSetScanParams && !c.params.empty()) {
        active = c.params[0] == 1;
      }
    }
    return active;
  }

  static int count(const std::vector<uint16_t>& seen, uint16_t opcode) {
    int n = 0;
    for (uint16_t o : seen) {
      if (o == opcode) ++n;
    }
    return n;
  }
};

// A user plus the things it saw, so a test does not write the same five
// lambdas each time.
struct Watcher {
  std::unique_ptr<SharedLink::User> user;
  int ready_calls = 0;
  bool ready_ok = false;
  std::string closed_why;
  std::vector<int> rssis;
  int scan_done = 0;
  bool scan_ok = false;
  int connected = 0;
  int disconnected = 0;

  Watcher(SharedLink* radio, const std::string& name)
      : user(radio->add_user(name)) {
    user->when_ready([this](bool ok, const std::string&) {
      ++ready_calls;
      ready_ok = ok;
    });
    user->set_closed_handler(
        [this](const std::string& why) { closed_why = why; });
    user->set_connection_handlers(
        [this](const Link::Conn&) { ++connected; },
        [this](uint16_t, uint8_t) { ++disconnected; });
  }

  void listen(bool active) {
    user->start_scan(
        active, [this](const octo::hci::AdvReport& r) { rssis.push_back(r.rssi); },
        [this](bool ok, const std::string&) {
          ++scan_done;
          scan_ok = ok;
        });
  }
};

// ------------------------------------------------------------ the properties

// The controller coming up reaches a user that subscribed before it did.
void ready_reaches_an_early_user() {
  Fixture f;
  Watcher a(f.radio.get(), "early");
  CHECK_EQ(a.ready_calls, 0);
  f.bring_up();
  CHECK_EQ(a.ready_calls, 1);
  CHECK(a.ready_ok);
  CHECK(f.radio->ready());
}

// And a user that subscribed afterwards is told too -- on the next turn of the
// loop, never inside the call that asked. That rule is the whole reason
// hcilink.h defers its own immediate failures, and a shared layer that broke
// it would hand every caller back the re-entrancy they were promised was gone.
void ready_reaches_a_late_user_but_not_inline() {
  Fixture f;
  f.bring_up();
  Watcher b(f.radio.get(), "late");
  CHECK_EQ(b.ready_calls, 0);  // not inside when_ready
  f.settle();
  CHECK_EQ(b.ready_calls, 1);
  CHECK(b.ready_ok);
}

// One listener means one passive scan.
void one_user_scans_passive() {
  Fixture f;
  Watcher a(f.radio.get(), "tentacles");
  f.bring_up();
  a.listen(false);
  f.answer_all();
  f.settle();
  CHECK(f.radio->scanning());
  CHECK(!f.radio->scanning_active());
  CHECK_EQ(a.scan_done, 1);
  CHECK(a.scan_ok);
}

// A second listener wanting the same thing does not restart the radio, and is
// still told yes.
void a_second_passive_user_costs_nothing() {
  Fixture f;
  Watcher a(f.radio.get(), "tentacles");
  f.bring_up();
  a.listen(false);
  f.answer_all();
  f.settle();

  Watcher b(f.radio.get(), "another");
  f.settle();
  b.listen(false);
  std::vector<uint16_t> after = f.answer_all();
  f.settle();

  CHECK_EQ(Fixture::count(after, octo::hci::kOpLeSetScanEnable), 0);
  CHECK_EQ(Fixture::count(after, octo::hci::kOpLeSetScanParams), 0);
  CHECK_EQ(b.scan_done, 1);
  CHECK(b.scan_ok);
  CHECK(f.radio->scanning());
}

// A user wanting an active scan while another wants passive gets one, because
// active is a superset -- the passive user still sees everything it would
// have.
void active_wins_over_passive() {
  Fixture f;
  Watcher a(f.radio.get(), "tentacles");
  f.bring_up();
  a.listen(false);
  f.answer_all();
  f.settle();
  CHECK(!f.radio->scanning_active());

  Watcher b(f.radio.get(), "camera");
  f.settle();
  b.listen(true);
  std::vector<uint16_t> after = f.answer_all();
  f.settle();

  // Stopped and started again: the parameters cannot be changed in place.
  CHECK(Fixture::count(after, octo::hci::kOpLeSetScanEnable) >= 2);
  CHECK(f.radio->scanning());
  CHECK(f.radio->scanning_active());
  CHECK_EQ(b.scan_done, 1);
  CHECK(b.scan_ok);
}

// Everybody scanning hears the same advertisement; a user that stopped hears
// nothing.
void adverts_fan_out_to_scanning_users_only() {
  Fixture f;
  Watcher a(f.radio.get(), "a");
  Watcher b(f.radio.get(), "b");
  Watcher c(f.radio.get(), "c");
  f.bring_up();
  a.listen(false);
  b.listen(false);
  f.answer_all();
  f.settle();

  f.deliver(advertisement(0x01, -60));
  CHECK_EQ(a.rssis.size(), static_cast<size_t>(1));
  CHECK_EQ(b.rssis.size(), static_cast<size_t>(1));
  CHECK_EQ(c.rssis.size(), static_cast<size_t>(0));
  CHECK_EQ(a.rssis[0], -60);

  b.user->stop_scan(nullptr);
  f.answer_all();
  f.settle();
  f.deliver(advertisement(0x02, -50));
  CHECK_EQ(a.rssis.size(), static_cast<size_t>(2));
  CHECK_EQ(b.rssis.size(), static_cast<size_t>(1));
  CHECK(f.radio->scanning());  // a still wants it
}

// The radio stops when the last user stops wanting it.
void the_last_user_stops_the_radio() {
  Fixture f;
  Watcher a(f.radio.get(), "a");
  Watcher b(f.radio.get(), "b");
  f.bring_up();
  a.listen(false);
  b.listen(false);
  f.answer_all();
  f.settle();
  CHECK(f.radio->scanning());

  a.user->stop_scan(nullptr);
  f.answer_all();
  f.settle();
  CHECK(f.radio->scanning());

  b.user->stop_scan(nullptr);
  f.answer_all();
  f.settle();
  CHECK(!f.radio->scanning());
}

// Destroying a user releases its share, which is the only way to unsubscribe.
void destroying_a_user_releases_the_radio() {
  Fixture f;
  Watcher a(f.radio.get(), "a");
  {
    Watcher b(f.radio.get(), "b");
    f.bring_up();
    a.listen(false);
    b.listen(false);
    f.answer_all();
    f.settle();
    CHECK_EQ(f.radio->user_count(), static_cast<size_t>(2));
  }
  f.answer_all();
  f.settle();
  CHECK_EQ(f.radio->user_count(), static_cast<size_t>(1));
  CHECK(f.radio->scanning());  // a still wants it

  a.user->stop_scan(nullptr);
  f.answer_all();
  f.settle();
  CHECK(!f.radio->scanning());
}

// And destroying the user that wanted an active scan puts the radio back to
// passive rather than leaving it announcing itself for nobody.
void destroying_the_active_user_drops_back_to_passive() {
  Fixture f;
  Watcher a(f.radio.get(), "tentacles");
  f.bring_up();
  a.listen(false);
  f.answer_all();
  f.settle();

  {
    Watcher b(f.radio.get(), "camera");
    f.settle();
    b.listen(true);
    f.answer_all();
    f.settle();
    CHECK(f.radio->scanning_active());
  }
  f.answer_all();
  f.settle();
  CHECK(f.radio->scanning());
  CHECK(!f.radio->scanning_active());
}

// The one this whole file is for: a connection that *succeeds* leaves the
// scan running.
//
// Link::connect stops the scan -- a controller cannot scan and initiate at
// once -- and by itself restores it only when the attempt failed. That is
// wrong for a daemon whose reference clock is a broadcast: the Tentacle
// reading is needed *during* the connection, not merely before it.
void a_successful_connection_leaves_the_scan_running() {
  Fixture f;
  Watcher a(f.radio.get(), "tentacles");
  Watcher b(f.radio.get(), "camera");
  f.bring_up();
  a.listen(false);
  f.answer_all();
  f.settle();
  CHECK(f.radio->scanning());

  octo::hci::Address peer;
  peer.type = octo::hci::kAddrPublic;
  for (int i = 0; i < 6; ++i) peer.bytes[i] = static_cast<uint8_t>(0x10 + i);

  bool connected = false;
  b.user->connect(peer, 5.0, [&connected](bool ok, uint16_t, const std::string&) {
    connected = ok;
  });
  f.answer_all();
  f.settle();
  // Initiating: the controller is not scanning.
  CHECK(!f.radio->scanning());

  f.deliver(connection_complete(octo::hci::kSuccess, 0x0040));
  f.answer_all();
  f.settle();

  CHECK(connected);
  CHECK(f.radio->scanning());
  CHECK_EQ(a.connected, 1);
  CHECK_EQ(b.connected, 1);

  // And the reference clock is genuinely audible again, not merely flagged on.
  f.deliver(advertisement(0x03, -55));
  CHECK_EQ(a.rssis.size(), static_cast<size_t>(1));
}

// A connection that fails also leaves the scan as the users asked for it --
// including active, which Link's own restore does not preserve. That is what
// the force flag in hcishare.cc is for, and without it the camera's scan comes
// back silently downgraded to passive.
void a_failed_connection_restores_the_right_parameters() {
  Fixture f;
  Watcher a(f.radio.get(), "camera");
  f.bring_up();
  a.listen(true);
  f.answer_all();
  f.settle();
  CHECK(f.radio->scanning_active());

  octo::hci::Address peer;
  peer.type = octo::hci::kAddrPublic;
  for (int i = 0; i < 6; ++i) peer.bytes[i] = static_cast<uint8_t>(0x10 + i);

  bool done = false;
  bool ok_seen = true;
  a.user->connect(peer, 5.0,
                  [&done, &ok_seen](bool ok, uint16_t, const std::string&) {
                    done = true;
                    ok_seen = ok;
                  });
  f.answer_all();
  f.settle();
  f.deliver(connection_complete(0x02, 0));  // unknown connection identifier
  f.answer_all();
  f.settle();

  CHECK(done);
  CHECK(!ok_seen);
  CHECK(f.radio->scanning());
  CHECK(f.radio->scanning_active());
}

// Disconnection fans out the same way connection does.
void disconnection_fans_out() {
  Fixture f;
  Watcher a(f.radio.get(), "a");
  Watcher b(f.radio.get(), "b");
  f.bring_up();

  octo::hci::Address peer;
  peer.type = octo::hci::kAddrPublic;
  for (int i = 0; i < 6; ++i) peer.bytes[i] = static_cast<uint8_t>(0x10 + i);
  b.user->connect(peer, 5.0, nullptr);
  f.answer_all();
  f.deliver(connection_complete(octo::hci::kSuccess, 0x0040));
  f.answer_all();
  f.settle();

  f.deliver(disconnection(0x0040, 0x13));
  f.settle();
  CHECK_EQ(a.disconnected, 1);
  CHECK_EQ(b.disconnected, 1);
}

// The dongle being unplugged is told to everybody, once.
void the_dongle_going_away_reaches_everybody() {
  Fixture f;
  Watcher a(f.radio.get(), "a");
  Watcher b(f.radio.get(), "b");
  f.bring_up();
  a.listen(false);
  f.answer_all();
  f.settle();

  f.port->unplug();
  f.loop.deliver(f.loop.last_source(), kRead, 0.0);
  f.settle();

  CHECK(!a.closed_why.empty());
  CHECK(!b.closed_why.empty());
  CHECK(f.radio->failed());
}

// A scan asked for on a radio that then dies is answered, rather than left
// waiting for a controller that will never reply.
void a_pending_scan_fails_when_the_radio_dies() {
  Fixture f;
  Watcher a(f.radio.get(), "a");
  f.bring_up();
  a.listen(false);
  // Do not answer the scan commands; take the dongle away instead.
  f.port->unplug();
  f.loop.deliver(f.loop.last_source(), kRead, 0.0);
  f.settle();
  CHECK_EQ(a.scan_done, 1);
  CHECK(!a.scan_ok);
}

// A controller that refuses to scan says so to the user that asked, and does
// not leave the shared state claiming a scan is running.
void a_refused_scan_is_reported() {
  Fixture f;
  Watcher a(f.radio.get(), "a");
  f.bring_up();
  a.listen(false);
  for (int round = 0; round < 6; ++round) {
    std::vector<Command> cmds = f.commands();
    if (cmds.empty()) break;
    for (const Command& c : cmds) f.deliver(complete_fail(c.opcode));
  }
  f.settle();
  CHECK_EQ(a.scan_done, 1);
  CHECK(!a.scan_ok);
  CHECK(!f.radio->scanning());
}

// A user destroyed inside another user's advertisement handler does not
// corrupt the fan-out in progress. The sync daemon does exactly this: it
// releases the camera from inside a handler when a cycle ends.
void a_user_may_be_destroyed_inside_a_handler() {
  Fixture f;
  Watcher a(f.radio.get(), "a");
  std::unique_ptr<Watcher> victim(new Watcher(f.radio.get(), "victim"));
  Watcher c(f.radio.get(), "c");
  f.bring_up();
  a.listen(false);
  victim->listen(false);
  c.listen(false);
  f.answer_all();
  f.settle();

  Watcher* raw = victim.get();
  a.user->start_scan(false,
                     [&victim, raw](const octo::hci::AdvReport&) {
                       if (victim.get() == raw) victim.reset();
                     },
                     nullptr);
  f.answer_all();
  f.settle();

  f.deliver(advertisement(0x04, -70));
  f.settle();
  CHECK(victim == nullptr);
  CHECK_EQ(c.rssis.size(), static_cast<size_t>(1));  // still delivered
  CHECK_EQ(f.radio->user_count(), static_cast<size_t>(2));
}

// A user may outlive the radio. The daemon destroys its members in whatever
// order it declared them, and this must be a no-op rather than a crash.
void a_user_may_outlive_the_radio() {
  FakeLoop loop;
  std::unique_ptr<SharedLink::User> user;
  int scan_done = 0;
  bool scan_ok = true;
  {
    std::unique_ptr<FakePort> owned(new FakePort());
    std::unique_ptr<SharedLink> radio =
        SharedLink::attach(&loop, std::move(owned), Link::Options());
    user = radio->add_user("outlives");
    CHECK(!user->orphaned());
  }
  CHECK(user->orphaned());
  CHECK(user->link() == nullptr);

  user->start_scan(false, nullptr, [&scan_done, &scan_ok](bool ok,
                                                          const std::string&) {
    ++scan_done;
    scan_ok = ok;
  });
  loop.advance(0.001);
  CHECK_EQ(scan_done, 1);
  CHECK(!scan_ok);

  int connect_done = 0;
  user->connect(octo::hci::Address(), 1.0,
                [&connect_done](bool, uint16_t, const std::string&) {
                  ++connect_done;
                });
  loop.advance(0.001);
  CHECK_EQ(connect_done, 1);
  user.reset();  // and destroying it afterwards is fine too
}

// Two users asking at once are both answered by the one reconciliation their
// requests share, rather than one of them waiting forever.
void simultaneous_requests_are_both_answered() {
  Fixture f;
  Watcher a(f.radio.get(), "a");
  Watcher b(f.radio.get(), "b");
  f.bring_up();
  a.listen(false);
  b.listen(false);  // before the first has been answered by the controller
  f.answer_all();
  f.settle();
  CHECK_EQ(a.scan_done, 1);
  CHECK_EQ(b.scan_done, 1);
  CHECK(a.scan_ok);
  CHECK(b.scan_ok);
  CHECK(f.radio->scanning());
}

// A scan asked for before the controller is up is not lost; it happens once
// the controller answers.
void a_scan_asked_for_while_opening_still_happens() {
  Fixture f;
  Watcher a(f.radio.get(), "a");
  a.listen(false);
  CHECK_EQ(a.scan_done, 0);
  CHECK(!f.radio->scanning());
  f.bring_up();
  f.answer_all();
  f.settle();
  CHECK_EQ(a.scan_done, 1);
  CHECK(a.scan_ok);
  CHECK(f.radio->scanning());
}

}  // namespace

int main() {
  ready_reaches_an_early_user();
  ready_reaches_a_late_user_but_not_inline();
  one_user_scans_passive();
  a_second_passive_user_costs_nothing();
  active_wins_over_passive();
  adverts_fan_out_to_scanning_users_only();
  the_last_user_stops_the_radio();
  destroying_a_user_releases_the_radio();
  destroying_the_active_user_drops_back_to_passive();
  a_successful_connection_leaves_the_scan_running();
  a_failed_connection_restores_the_right_parameters();
  disconnection_fans_out();
  the_dongle_going_away_reaches_everybody();
  a_pending_scan_fails_when_the_radio_dies();
  a_refused_scan_is_reported();
  a_user_may_be_destroyed_inside_a_handler();
  a_user_may_outlive_the_radio();
  simultaneous_requests_are_both_answered();
  a_scan_asked_for_while_opening_still_happens();
  return octotest::report("test_hcishare");
}
