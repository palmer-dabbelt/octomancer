// The HCI host: everything stateful that sits between the serial port and a
// program that wants a radio.
//
// A controller is passive in the sense that matters here -- it does what it is
// told and reports what happened -- so all the state lives up here: which
// commands are outstanding, which connections exist, how many ACL buffers the
// controller has left, and how a stream of fragments becomes an ATT PDU. That
// is a fair amount of machinery, and it is the price of the thing macOS would
// not do: putting exact bytes on the air and seeing exactly what comes back.
//
// Nothing here blocks and nothing here has a thread. The port is a source on
// the loop, bytes are turned into events when there are bytes, and everything
// that used to be a return value is now a completion handler. That is not a
// stylistic preference: the Zephyr SDK's libstdc++ is built without
// _GLIBCXX_HAS_GTHREADS in every multilib, so the reader thread and the four
// mutexes this file used to hold cannot exist on the box at all. See loop.h.
//
// The shape that replaces them is a queue per resource, because that is what
// the old locks were standing in for:
//
//   * one HCI command in flight, the rest queued in order;
//   * one ATT request in flight per connection, the rest queued per connection
//     -- the protocol's own rule, and previously enforced across all
//     connections at once by a single mutex, which was stricter than it needed
//     to be;
//   * ACL fragments queued against the controller's buffer credits, which is
//     the one piece of flow control that must not be got wrong: a packet sent
//     without a credit is not refused, it is dropped, and the peer simply
//     stops answering.
//
// Two rules callers can rely on. A completion handler is never invoked before
// the call that registered it has returned -- an immediate failure is deferred
// onto the loop like any other -- so a caller never has to reason about
// re-entering itself. And every handler runs on the loop's thread, which is
// the only thread, so nothing a handler touches needs a lock.
//
// One rule callers have to honour: a handler must not destroy the Link it was
// called from. Post that to the loop instead. Tearing down the object whose
// dispatch is still on the stack is the one hazard the queues cannot absorb.
#ifndef OCTO_HCILINK_H
#define OCTO_HCILINK_H

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "att.h"
#include "hci.h"
#include "hciport.h"
#include "loop.h"

namespace octo {
namespace hci {

// What to put on the air. Held as raw AD bytes rather than as a list of fields
// because being able to say precisely this and nothing else is the entire
// reason the dongle exists -- see doc/zoom-bta1-notes.md, where the open
// question was what CoreBluetooth had actually transmitted.
struct AdvConfig {
  std::vector<uint8_t> adv_data;       // at most 31 bytes
  std::vector<uint8_t> scan_response;  // at most 31 bytes; may be empty
  uint8_t type = kAdvInd;
  double interval_ms = 100.0;
};

class Link {
 public:
  struct Options {
    std::string device;  // empty: use the first dongle found
    // Log every packet in both directions. Verbose, and the only way to
    // answer "what did we actually send" -- which is the question this whole
    // subsystem exists to make answerable.
    bool trace = false;
    double command_timeout = 10.0;
  };

  // ------------------------------------------------------- completion types

  // `ok` is false when the controller refused, when it never answered, or
  // when the link died underneath the request; `err` says which.
  using DoneHandler = std::function<void(bool ok, const std::string& err)>;
  using CommandHandler = std::function<void(bool ok, const CommandComplete& cc,
                                            const std::string& err)>;
  using ConnectHandler =
      std::function<void(bool ok, uint16_t handle, const std::string& err)>;
  // An Error Response arrives in `rsp` with `ok` true, not as a failure --
  // "attribute not found" is a normal answer while walking a table, not a
  // fault. `ok` false means no answer came back at all.
  using ResponseHandler = std::function<void(
      bool ok, const std::vector<uint8_t>& rsp, const std::string& err)>;

  // ------------------------------------------------------------------ open

  // Opens the port and starts bringing the controller up. The link is NOT
  // usable when this returns: reset, event masks, buffer sizes and installing
  // an address are half a dozen round trips, and `on_ready` is how a caller
  // learns they finished. Commands issued before then are queued behind them,
  // which is correct but is not the same as being ready.
  //
  // Returns nullptr with a reason when the port itself will not open, which is
  // knowable straight away. hci::no_port_found() distinguishes "no dongle"
  // from "the dongle would not open", and only the first should make a caller
  // fall back to another radio.
  static std::unique_ptr<Link> open(Loop* loop, const Options& opts,
                                    DoneHandler on_ready, std::string* err);

  // Same, over a port the caller supplies. This is what lets the whole host be
  // driven against a scripted controller in a test -- see tests/test_hcilink.cc
  // -- which is the reason Port was ever a virtual class.
  static std::unique_ptr<Link> attach(Loop* loop, std::unique_ptr<Port> port,
                                      const Options& opts,
                                      DoneHandler on_ready);

  ~Link();
  Link(const Link&) = delete;
  Link& operator=(const Link&) = delete;

  std::string port_name() const;
  Address local_address() const { return local_; }
  // "HCI 5.4, address C0:...", for the logs.
  std::string describe() const;

  // The controller's ACL payload size. Anything larger has to be fragmented,
  // which att::fragment does.
  size_t max_acl_payload() const { return acl_payload_; }

  // True once the port has failed. Every outstanding completion has already
  // been called with an error by the time this is observable.
  bool closed() const { return closed_; }

  // Called once, when the port dies underneath us -- almost always the dongle
  // being unplugged. Distinct from a request failing: the link is gone and
  // nothing further will work.
  using ClosedHandler = std::function<void(const std::string& why)>;
  void set_closed_handler(ClosedHandler on_closed);

  // --------------------------------------------------------------- scanning

  using AdvHandler = std::function<void(const AdvReport&)>;

  // Passive scanning never sends a scan request, so it never provokes a scan
  // response and never announces our presence. That is what a Tentacle wants:
  // the clock is in the advertisement itself.
  //
  // A null `on_report` means "keep whatever is already installed", which is
  // what lets connect() put scanning back the way it found it without carrying
  // the caller's callback around.
  void start_scan(bool active, bool filter_duplicates, AdvHandler on_report,
                  DoneHandler done);
  void stop_scan(DoneHandler done);
  bool scanning() const { return scanning_; }

  // ------------------------------------------------------------ advertising

  void start_advertising(const AdvConfig& cfg, DoneHandler done);
  void stop_advertising(DoneHandler done);
  bool advertising() const { return advertising_; }

  // ------------------------------------------------------------ connections

  struct Conn {
    uint16_t handle = 0;
    Address peer;
    uint8_t role = 0;  // 0 central (we connected out), 1 peripheral
  };

  using ConnectedHandler = std::function<void(const Conn&)>;
  using DisconnectedHandler =
      std::function<void(uint16_t handle, uint8_t reason)>;
  void set_connection_handlers(ConnectedHandler on_connect,
                               DisconnectedHandler on_disconnect);

  // Connects outward. Scanning is stopped for the duration and restored
  // afterwards: a controller cannot scan and initiate at the same time, and
  // the failure if it is asked to is a bare "command disallowed".
  //
  // One at a time. A controller has one initiator, so a second call while one
  // is outstanding fails rather than queueing -- queueing would mean a caller
  // waiting a full timeout for a connection it has since stopped wanting.
  void connect(const Address& peer, double timeout, ConnectHandler done);
  void disconnect(uint16_t handle, uint8_t reason = kRemoteUserTerminated);
  std::vector<Conn> connections() const;
  bool connected(uint16_t handle) const { return conns_.count(handle) != 0; }

  // ------------------------------------------------------------------- ATT

  // Every ATT PDU that is not the response to an outstanding request: a
  // notification when we are the client, a request when we are the server.
  using AttHandler =
      std::function<void(uint16_t conn, const std::vector<uint8_t>& pdu)>;
  void set_att_handler(AttHandler on_att);

  // Queues a PDU for transmission. True means accepted, not delivered: it may
  // still be waiting on a controller buffer. False means it will never go --
  // an unknown connection, or more bytes outstanding than this device is
  // willing to hold.
  bool send_att(uint16_t conn, const std::vector<uint8_t>& pdu,
                std::string* err);

  // Sends a request and calls back with its response. ATT permits exactly one
  // outstanding request per direction, which this enforces rather than
  // discovers: two in flight and the responses cannot be told apart. Further
  // requests on the same connection queue behind it in the order they were
  // made.
  void att_request(uint16_t conn, const std::vector<uint8_t>& req,
                   double timeout, ResponseHandler done);

  // ------------------------------------------------------------------- SMP

  // Pairing PDUs travel on their own L2CAP channel, not on ATT. The exchange
  // itself is smp.h; this is only the pipe and the one command that turns the
  // result into an encrypted link.
  using SmpHandler =
      std::function<void(uint16_t conn, const std::vector<uint8_t>& pdu)>;
  void set_smp_handler(SmpHandler on_smp);
  bool send_smp(uint16_t conn, const std::vector<uint8_t>& pdu,
                std::string* err);

  // Start encryption with a key pairing produced. Legacy pairing's first
  // encryption uses an EDIV and Rand of zero; a stored long term key would
  // supply its own.
  //
  // Failure here is usually the peer disagreeing about the key, which on a
  // camera means the passkey was wrong -- the status is "PIN or key missing"
  // either way, so the message says both.
  void start_encryption(uint16_t conn, const std::array<uint8_t, 16>& ltk,
                        uint16_t ediv, const std::array<uint8_t, 8>& rand,
                        double timeout, DoneHandler done);
  bool encrypted(uint16_t conn) const;

  // ------------------------------------------------------------------- raw

  // Escape hatches, for the experiments. A capture worth taking is usually one
  // nobody wrote an API for.
  void command(uint16_t opcode, const std::vector<uint8_t>& params,
               CommandHandler done);
  bool send_l2cap(uint16_t conn, uint16_t cid,
                  const std::vector<uint8_t>& payload, std::string* err);
  using L2capHandler = std::function<void(uint16_t conn, uint16_t cid,
                                          const std::vector<uint8_t>& payload)>;
  void set_l2cap_handler(L2capHandler on_frame);

  // Every event, before it is dispatched. For the sniffing modes.
  using EventHandler = std::function<void(const Event&)>;
  void set_event_handler(EventHandler on_event);

  // How many ACL fragments may sit waiting for a controller buffer before
  // send_att refuses. A cap rather than an unbounded queue because this code
  // has to run on a device with 256 KB of RAM, where "the peer stopped reading
  // and we kept queueing" is an out-of-memory reboot rather than a slow
  // program.
  static constexpr size_t kMaxQueuedFragments = 64;

 private:
  explicit Link(Loop* loop);

  // ------------------------------------------------------------- internals

  struct QueuedCommand {
    uint16_t opcode = 0;
    std::vector<uint8_t> params;
    CommandHandler done;
  };

  struct QueuedFragment {
    uint16_t conn = 0;
    uint8_t pb = 0;
    std::vector<uint8_t> data;
  };

  struct QueuedRequest {
    std::vector<uint8_t> req;
    double timeout = 0.0;
    ResponseHandler done;
  };

  // One ATT channel per connection: the request in flight, and whatever is
  // waiting behind it.
  struct AttChannel {
    bool in_flight = false;
    uint8_t req_opcode = 0;
    ResponseHandler done;
    TimerId timer = kNoTimer;
    std::deque<QueuedRequest> queue;
  };

  void on_readable(int interest);
  void on_port_error(const std::string& why);
  void fail(const std::string& why);

  // Run `fn` on the next turn of the loop. Every synchronous failure goes
  // through here, so that a completion handler never runs inside the call that
  // registered it.
  void defer(std::function<void()> fn);

  void init_step(int step);
  void init_address();
  void finish_init(bool ok, const std::string& err);

  // Issue a fixed list of commands in order, stopping at the first failure.
  // Turning on scanning is two commands and turning on advertising is four,
  // and none of them looks at the result of the one before -- so writing them
  // as a list is the difference between a flat description of what happens and
  // four levels of nested lambda saying the same thing.
  //
  // The position is held in a shared struct rather than in a lambda that
  // captures itself. A self-referential std::function is the obvious way to
  // write this and it leaks: the closure holds the shared_ptr that owns the
  // closure, so the count never reaches zero. One small leak per scan, on a
  // device meant to run for days.
  struct Sequence {
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> steps;
    size_t index = 0;
    DoneHandler done;
  };
  void command_sequence(
      std::vector<std::pair<uint16_t, std::vector<uint8_t>>> steps,
      DoneHandler done);
  void run_sequence(std::shared_ptr<Sequence> seq);

  void pump_commands();
  void finish_command(bool ok, const CommandComplete& cc,
                      const std::string& err);
  void pump_acl();
  void pump_att(uint16_t conn);
  void finish_att(uint16_t conn, bool ok, const std::vector<uint8_t>& rsp,
                  const std::string& err);
  void drop_connection(uint16_t handle, const std::string& why);
  void finish_connect(bool ok, uint16_t handle, const std::string& err);
  void finish_encryption(bool ok, const std::string& err);

  void dispatch_event(const Event& evt);
  void dispatch_acl(const std::vector<uint8_t>& payload);
  bool write_packet(const std::vector<uint8_t>& pkt, std::string* err);
  bool queue_acl(uint16_t conn, const std::vector<uint8_t>& frame,
                 std::string* err);
  void log(const char* dir, const std::vector<uint8_t>& pkt);

  Loop* loop_ = nullptr;
  // False once the destructor has run. Deferred work captures a copy and
  // checks it: the loop outlives the link, and a link torn down between
  // posting a continuation and its firing must not take the loop with it.
  std::shared_ptr<bool> alive_;
  SourceId source_ = kNoSource;
  std::unique_ptr<Port> port_;
  Options opts_;
  bool closed_ = false;
  std::vector<uint8_t> rx_;

  DoneHandler on_ready_;
  ClosedHandler on_closed_;

  std::deque<QueuedCommand> cmd_queue_;
  bool cmd_in_flight_ = false;
  uint16_t cmd_opcode_ = 0;
  CommandHandler cmd_done_;
  TimerId cmd_timer_ = kNoTimer;

  // ACL flow control. Writing more packets than the controller has buffers
  // for is not rejected; it is dropped, silently, which presents as a peer
  // that stops answering.
  size_t acl_payload_ = 27;
  int acl_credits_ = 1;
  int acl_total_ = 1;
  std::deque<QueuedFragment> acl_queue_;

  Address local_;
  uint8_t own_addr_type_ = kAddrPublic;
  std::string version_;

  bool scanning_ = false;
  bool advertising_ = false;
  AdvHandler on_adv_;

  std::map<uint16_t, Conn> conns_;
  std::map<uint16_t, att::Reassembler> reasm_;
  std::map<uint16_t, AttChannel> att_;
  std::map<uint16_t, bool> encrypted_;

  bool connect_pending_ = false;
  bool connect_restore_scan_ = false;
  Address connect_peer_;
  ConnectHandler connect_done_;
  TimerId connect_timer_ = kNoTimer;

  bool encrypt_pending_ = false;
  DoneHandler encrypt_done_;
  TimerId encrypt_timer_ = kNoTimer;

  SmpHandler on_smp_;
  ConnectedHandler on_connect_;
  DisconnectedHandler on_disconnect_;
  AttHandler on_att_;
  L2capHandler on_l2cap_;
  EventHandler on_event_;
};

}  // namespace hci
}  // namespace octo

#endif  // OCTO_HCILINK_H
