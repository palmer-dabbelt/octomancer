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
// Threading. A reader thread owns the port and turns bytes into events. Public
// methods are called from whatever thread the program uses and block until the
// controller answers, which is the same shape camera.h already has and for the
// same reason: the daemon above is a plain loop, and a callback-shaped radio
// would turn it into a state machine for no gain. Callbacks handed to
// set_*_handler run on the reader thread.
#ifndef OCTO_HCILINK_H
#define OCTO_HCILINK_H

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "att.h"
#include "hci.h"
#include "hciport.h"

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

  // Opens the port and brings the controller up: reset, event masks, buffer
  // sizes, and an address to advertise from. Returns nullptr with a reason on
  // failure; hci::no_port_found() distinguishes "no dongle" from "the dongle
  // would not open", and only the first should make a caller fall back to
  // another radio.
  static std::unique_ptr<Link> open(const Options& opts, std::string* err);

  ~Link();
  Link(const Link&) = delete;
  Link& operator=(const Link&) = delete;

  std::string port_name() const;
  Address local_address() const;
  // "nRF52840 at /dev/cu.usbmodem1101, HCI 5.4, address C0:...", for the logs.
  std::string describe() const;

  // The controller's ACL payload size. Anything larger has to be fragmented,
  // which att::fragment does.
  size_t max_acl_payload() const;

  // --------------------------------------------------------------- scanning

  using AdvHandler = std::function<void(const AdvReport&)>;

  // Passive scanning never sends a scan request, so it never provokes a scan
  // response and never announces our presence. That is what a Tentacle wants:
  // the clock is in the advertisement itself.
  bool start_scan(bool active, bool filter_duplicates, AdvHandler on_report,
                  std::string* err);
  bool stop_scan(std::string* err);
  bool scanning() const;

  // ------------------------------------------------------------ advertising

  bool start_advertising(const AdvConfig& cfg, std::string* err);
  bool stop_advertising(std::string* err);
  bool advertising() const;

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

  // Connects outward and waits. Scanning is stopped for the duration: a
  // controller cannot scan and initiate at the same time, and the failure if
  // it is asked to is a bare "command disallowed".
  bool connect(const Address& peer, double timeout, uint16_t* handle,
               std::string* err);
  bool disconnect(uint16_t handle, uint8_t reason = kRemoteUserTerminated);
  std::vector<Conn> connections() const;
  bool connected(uint16_t handle) const;

  // ------------------------------------------------------------------- ATT

  // Every ATT PDU that is not the response to an outstanding request: a
  // notification when we are the client, a request when we are the server.
  using AttHandler =
      std::function<void(uint16_t conn, const std::vector<uint8_t>& pdu)>;
  void set_att_handler(AttHandler on_att);

  bool send_att(uint16_t conn, const std::vector<uint8_t>& pdu,
                std::string* err);

  // Sends a request and waits for its response. ATT permits exactly one
  // outstanding request per direction, which this enforces rather than
  // discovers: two in flight and the responses cannot be told apart.
  //
  // An Error Response is returned in `rsp` as-is, not turned into a failure --
  // "attribute not found" is a normal answer while walking a table, not a
  // fault.
  bool att_request(uint16_t conn, const std::vector<uint8_t>& req,
                   std::vector<uint8_t>* rsp, double timeout, std::string* err);

  // ------------------------------------------------------------------- SMP

  // Pairing PDUs travel on their own L2CAP channel, not on ATT. The exchange
  // itself is smp.h; this is only the pipe and the one command that turns the
  // result into an encrypted link.
  using SmpHandler =
      std::function<void(uint16_t conn, const std::vector<uint8_t>& pdu)>;
  void set_smp_handler(SmpHandler on_smp);
  bool send_smp(uint16_t conn, const std::vector<uint8_t>& pdu,
                std::string* err);

  // Start encryption with a key pairing produced, and wait for the controller
  // to say it took. Legacy pairing's first encryption uses an EDIV and Rand of
  // zero; a stored long term key would supply its own.
  //
  // Failure here is usually the peer disagreeing about the key, which on a
  // camera means the passkey was wrong -- the status is "PIN or key missing"
  // either way, so the message says both.
  bool start_encryption(uint16_t conn, const std::array<uint8_t, 16>& ltk,
                        uint16_t ediv, const std::array<uint8_t, 8>& rand,
                        double timeout, std::string* err);
  bool encrypted(uint16_t conn) const;

  // ------------------------------------------------------------------- raw

  // Escape hatches, for the experiments. A capture worth taking is usually one
  // nobody wrote an API for.
  bool command(uint16_t opcode, const std::vector<uint8_t>& params,
               CommandComplete* out, std::string* err);
  bool send_l2cap(uint16_t conn, uint16_t cid,
                  const std::vector<uint8_t>& payload, std::string* err);
  using L2capHandler = std::function<void(uint16_t conn, uint16_t cid,
                                          const std::vector<uint8_t>& payload)>;
  void set_l2cap_handler(L2capHandler on_frame);

  // Every event, before it is dispatched. For the sniffing modes.
  using EventHandler = std::function<void(const Event&)>;
  void set_event_handler(EventHandler on_event);

 private:
  Link();
  bool init(std::string* err);
  void reader();
  void dispatch_event(const Event& evt);
  void dispatch_acl(const std::vector<uint8_t>& payload);
  bool write_packet(const std::vector<uint8_t>& pkt, std::string* err);
  bool send_acl(uint16_t conn, const std::vector<uint8_t>& frame,
                std::string* err);
  void log(const char* dir, const std::vector<uint8_t>& pkt);

  std::unique_ptr<Port> port_;
  Options opts_;
  std::thread reader_;
  bool running_ = false;

  mutable std::mutex mu_;
  std::condition_variable cv_;

  // One command at a time. The controller allows more, but nothing here needs
  // the throughput, and serialising removes the only way the opcode-to-caller
  // mapping can go wrong.
  std::mutex command_mu_;
  bool cmd_pending_ = false;
  uint16_t cmd_opcode_ = 0;
  bool cmd_done_ = false;
  CommandComplete cmd_result_;

  // ACL flow control. Writing more packets than the controller has buffers
  // for is not rejected; it is dropped, silently, which presents as a peer
  // that stops answering.
  size_t acl_payload_ = 27;
  int acl_credits_ = 1;
  int acl_total_ = 1;

  Address local_;
  uint8_t own_addr_type_ = kAddrPublic;
  std::string version_;

  bool scanning_ = false;
  bool advertising_ = false;

  std::map<uint16_t, Conn> conns_;
  std::map<uint16_t, att::Reassembler> reasm_;

  // Waiting for LE Connection Complete after LE Create Connection.
  bool connect_pending_ = false;
  bool connect_done_ = false;
  ConnectionComplete connect_result_;

  // The single outstanding ATT request, per connection.
  struct AttWait {
    bool waiting = false;
    bool done = false;
    uint8_t req_opcode = 0;
    std::vector<uint8_t> rsp;
  };
  std::map<uint16_t, AttWait> att_waits_;
  std::mutex att_request_mu_;

  std::map<uint16_t, bool> encrypted_;
  bool encrypt_pending_ = false;
  bool encrypt_done_ = false;
  uint8_t encrypt_status_ = 0;
  uint8_t encrypt_enabled_ = 0;

  AdvHandler on_adv_;
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
