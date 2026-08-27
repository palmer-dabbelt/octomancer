// L2CAP framing and the Attribute Protocol, in both directions.
//
// Both directions is the point. CoreBluetooth gives a program one role at a
// time and a different API for each -- CBCentralManager to talk to a camera,
// CBPeripheralManager to be talked to by a Zoom -- and the peripheral half is
// the one that turned out to be unusable for this project. Underneath, ATT is
// a single symmetric protocol over one L2CAP channel, so a client and a server
// are the same codec read in opposite directions. That is what this file is.
//
// The server half exists because of the Zoom BTA-1. In timecode mode the F6
// scans and connects outward, so anything that wants to feed it timecode has
// to be a peripheral hosting the profile in doc/zoom-bta1-notes.md. The client
// half exists because a Blackmagic camera is a peripheral and octomancer-sync
// has to write its clock.
//
// Everything here is pure byte arithmetic over buffers. No radio, no sockets,
// no threads -- so tests/test_att.cc can drive a whole attribute table through
// a discovery sequence on a machine with no Bluetooth at all.
//
// References are to the Bluetooth Core Specification 5.4, Vol 3 Part F (ATT)
// and Vol 3 Part G (GATT).
#ifndef OCTO_ATT_H
#define OCTO_ATT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hci.h"

namespace octo {
namespace att {

using hci::Uuid;

// ------------------------------------------------------------------ L2CAP

// The fixed channel identifiers LE uses. There is no channel setup on LE for
// any of these: they exist as soon as the connection does.
enum ChannelId : uint16_t {
  kCidAtt = 0x0004,
  kCidLeSignaling = 0x0005,
  kCidSmp = 0x0006,
};

// len(2) + cid(2) + payload. The length counts the payload only.
std::vector<uint8_t> build_l2cap(uint16_t cid,
                                 const std::vector<uint8_t>& payload);
bool parse_l2cap(const uint8_t* data, size_t len, uint16_t* cid,
                 std::vector<uint8_t>* payload);

// An L2CAP frame can be split across several ACL fragments, and on LE it very
// often is: the default ACL payload is 27 bytes and a service discovery
// response is routinely longer. This reassembles one connection's stream.
//
// Kept as an object with state rather than a free function because the
// boundary between "this fragment completes a frame" and "there is more
// coming" is exactly the state, and a caller that has to track it separately
// will eventually get it wrong under a retransmission.
class Reassembler {
 public:
  // Feed one ACL fragment. Completed frames are appended to `out`. Returns
  // false when the stream is malformed, at which point the buffer is dropped:
  // continuing from a bad length would misinterpret every frame after it.
  bool push(uint8_t pb_flag, const uint8_t* data, size_t len,
            std::vector<std::pair<uint16_t, std::vector<uint8_t>>>* out);
  void reset();
  bool in_progress() const { return expecting_ > 0; }

 private:
  std::vector<uint8_t> buf_;
  size_t expecting_ = 0;  // total frame bytes wanted, header included
};

// Split an outgoing frame into ACL-sized fragments. The first carries the
// "first flushable" flag and the rest carry "continuing"; a controller that
// receives the wrong flags treats a fragment as a new frame and the far end
// sees garbage.
std::vector<std::vector<uint8_t>> fragment(const std::vector<uint8_t>& frame,
                                           size_t max_acl_payload);

// -------------------------------------------------------------- ATT opcodes

enum Opcode : uint8_t {
  kErrorResponse = 0x01,
  kExchangeMtuRequest = 0x02,
  kExchangeMtuResponse = 0x03,
  kFindInformationRequest = 0x04,
  kFindInformationResponse = 0x05,
  kFindByTypeValueRequest = 0x06,
  kFindByTypeValueResponse = 0x07,
  kReadByTypeRequest = 0x08,
  kReadByTypeResponse = 0x09,
  kReadRequest = 0x0a,
  kReadResponse = 0x0b,
  kReadBlobRequest = 0x0c,
  kReadBlobResponse = 0x0d,
  kReadMultipleRequest = 0x0e,
  kReadMultipleResponse = 0x0f,
  kReadByGroupTypeRequest = 0x10,
  kReadByGroupTypeResponse = 0x11,
  kWriteRequest = 0x12,
  kWriteResponse = 0x13,
  kWriteCommand = 0x52,
  kPrepareWriteRequest = 0x16,
  kPrepareWriteResponse = 0x17,
  kExecuteWriteRequest = 0x18,
  kExecuteWriteResponse = 0x19,
  kHandleValueNotification = 0x1b,
  kHandleValueIndication = 0x1d,
  kHandleValueConfirmation = 0x1e,
};

enum Error : uint8_t {
  kInvalidHandle = 0x01,
  kReadNotPermitted = 0x02,
  kWriteNotPermitted = 0x03,
  kInvalidPdu = 0x04,
  kInsufficientAuthentication = 0x05,
  kRequestNotSupported = 0x06,
  kInvalidOffset = 0x07,
  kInsufficientAuthorization = 0x08,
  kPrepareQueueFull = 0x09,
  kAttributeNotFound = 0x0a,
  kAttributeNotLong = 0x0b,
  kInsufficientKeySize = 0x0c,
  kInvalidAttributeValueLength = 0x0d,
  kUnlikelyError = 0x0e,
  kInsufficientEncryption = 0x0f,
  kUnsupportedGroupType = 0x10,
  kInsufficientResources = 0x11,
};

const char* error_name(uint8_t code);
const char* opcode_name(uint8_t opcode);

// The default ATT MTU on LE, and the only size a peer is obliged to accept
// before an exchange has happened.
inline constexpr uint16_t kDefaultMtu = 23;

// ---------------------------------------------------------- GATT constants

// The declarations that give an attribute table its structure. These are
// ordinary attributes whose type happens to be one of these UUIDs -- there is
// no separate "service" object anywhere in the protocol.
inline constexpr uint16_t kUuidPrimaryService = 0x2800;
inline constexpr uint16_t kUuidSecondaryService = 0x2801;
inline constexpr uint16_t kUuidInclude = 0x2802;
inline constexpr uint16_t kUuidCharacteristic = 0x2803;
inline constexpr uint16_t kUuidCharExtendedProperties = 0x2900;
inline constexpr uint16_t kUuidCharUserDescription = 0x2901;
inline constexpr uint16_t kUuidClientCharConfig = 0x2902;  // the CCCD

// Device Information, which is what the Zoom adapter answered with and how
// its firmware revision was matched to the carved image.
inline constexpr uint16_t kUuidDeviceInformation = 0x180a;
inline constexpr uint16_t kUuidManufacturerName = 0x2a29;
inline constexpr uint16_t kUuidModelNumber = 0x2a24;
inline constexpr uint16_t kUuidFirmwareRevision = 0x2a26;
inline constexpr uint16_t kUuidSoftwareRevision = 0x2a28;

enum CharProperty : uint8_t {
  kPropBroadcast = 0x01,
  kPropRead = 0x02,
  kPropWriteWithoutResponse = 0x04,
  kPropWrite = 0x08,
  kPropNotify = 0x10,
  kPropIndicate = 0x20,
  kPropSignedWrite = 0x40,
  kPropExtended = 0x80,
};

std::string properties_to_string(uint8_t props);

// Writing this to a CCCD is what starts notifications. It is the step that is
// easy to forget and produces a connection that works and never says anything.
inline constexpr uint16_t kCccNotify = 0x0001;
inline constexpr uint16_t kCccIndicate = 0x0002;

// -------------------------------------------------------- client: requests

std::vector<uint8_t> exchange_mtu_request(uint16_t mtu);
std::vector<uint8_t> find_information_request(uint16_t start, uint16_t end);
std::vector<uint8_t> read_by_group_type_request(uint16_t start, uint16_t end,
                                                const Uuid& type);
std::vector<uint8_t> read_by_type_request(uint16_t start, uint16_t end,
                                          const Uuid& type);
std::vector<uint8_t> read_request(uint16_t handle);
std::vector<uint8_t> read_blob_request(uint16_t handle, uint16_t offset);
std::vector<uint8_t> write_request(uint16_t handle,
                                   const std::vector<uint8_t>& value);
std::vector<uint8_t> write_command(uint16_t handle,
                                   const std::vector<uint8_t>& value);
std::vector<uint8_t> handle_value_confirmation();

// ------------------------------------------------------- client: responses

struct ErrorResponse {
  uint8_t request_opcode = 0;
  uint16_t handle = 0;
  uint8_t error = 0;
};
bool parse_error_response(const std::vector<uint8_t>& pdu, ErrorResponse* out);

bool parse_exchange_mtu_response(const std::vector<uint8_t>& pdu,
                                 uint16_t* mtu);

// One service as reported by Read By Group Type over 0x2800.
struct ServiceRange {
  uint16_t start = 0;
  uint16_t end = 0;
  Uuid uuid;
};
bool parse_read_by_group_type_response(const std::vector<uint8_t>& pdu,
                                       std::vector<ServiceRange>* out);

// One characteristic as reported by Read By Type over 0x2803. `value_handle`
// is the handle to actually read and write; `handle` is the declaration's own,
// which is only useful for working out where the next one starts.
struct CharDecl {
  uint16_t handle = 0;
  uint8_t properties = 0;
  uint16_t value_handle = 0;
  Uuid uuid;
};
bool parse_read_by_type_response_chars(const std::vector<uint8_t>& pdu,
                                       std::vector<CharDecl>* out);

// The generic form, for reading any type across a range.
struct TypedValue {
  uint16_t handle = 0;
  std::vector<uint8_t> value;
};
bool parse_read_by_type_response(const std::vector<uint8_t>& pdu,
                                 std::vector<TypedValue>* out);

struct HandleUuid {
  uint16_t handle = 0;
  Uuid uuid;
};
bool parse_find_information_response(const std::vector<uint8_t>& pdu,
                                     std::vector<HandleUuid>* out);

bool parse_read_response(const std::vector<uint8_t>& pdu,
                         std::vector<uint8_t>* value);

// Notifications and indications differ only in whether the sender wants a
// confirmation back. Both land here.
struct Notification {
  uint16_t handle = 0;
  std::vector<uint8_t> value;
  bool wants_confirmation = false;
};
bool parse_notification(const std::vector<uint8_t>& pdu, Notification* out);

// ------------------------------------------------------------ server side

// One row of the attribute table. The table *is* the server: a GATT service is
// nothing more than a run of these with the right types in the right order,
// which is why building one by hand is reasonable and a framework is not
// needed to host three characteristics.
struct Attribute {
  uint16_t handle = 0;
  Uuid type;
  std::vector<uint8_t> value;
  bool readable = true;
  bool writable = false;
  // A characteristic whose value the server generates on demand rather than
  // storing. Used for anything live -- a timecode read has to be the time now,
  // not the time when the table was built.
  std::function<std::vector<uint8_t>()> read_fn;
};

// Builds a well-formed attribute table and hands back the handles that matter.
// Handles are assigned in order from 1, which is what every client assumes
// even though nothing requires it.
class ServerBuilder {
 public:
  // Returns the service's declaration handle.
  uint16_t add_primary_service(const Uuid& uuid);

  // Returns the *value* handle -- the one a client reads, writes, and receives
  // notifications from. The declaration handle is never what a caller wants.
  //
  // A characteristic with notify or indicate among its properties
  // automatically gets a CCCD after it, because a notifying characteristic
  // without one is a service no client can ever subscribe to.
  uint16_t add_characteristic(const Uuid& uuid, uint8_t properties,
                              const std::vector<uint8_t>& value = {},
                              const std::string& description = "");

  // A characteristic whose value is computed at read time.
  uint16_t add_dynamic_characteristic(
      const Uuid& uuid, uint8_t properties,
      std::function<std::vector<uint8_t>()> read_fn,
      const std::string& description = "");

  std::vector<Attribute> take() { return std::move(attrs_); }

 private:
  uint16_t next_ = 1;
  std::vector<Attribute> attrs_;
};

// Answers ATT requests out of an attribute table.
//
// Deliberately not a class with a socket in it: `handle_request` takes a PDU
// and returns a PDU. Whatever moves the bytes -- a dongle, a test, a replay of
// a capture -- is somebody else's problem, and that is what makes the Zoom
// experiment reproducible without the Zoom.
class Server {
 public:
  explicit Server(std::vector<Attribute> attrs) : attrs_(std::move(attrs)) {}

  // Returns the response PDU, or an empty vector for a request that takes no
  // response (a Write Command). Never returns a malformed PDU: anything it
  // cannot answer becomes a proper Error Response, because a peer that gets
  // silence retries until it gives up and reports nothing useful.
  std::vector<uint8_t> handle_request(const std::vector<uint8_t>& pdu);

  uint16_t mtu() const { return mtu_; }

  // Called for every write the peer makes, including to a CCCD. This is the
  // whole experiment on the Zoom side: what the F6 writes, and when.
  using WriteHandler =
      std::function<void(uint16_t handle, const std::vector<uint8_t>& value)>;
  void set_write_handler(WriteHandler fn) { on_write_ = std::move(fn); }

  // True once the peer has written the notify bit to the CCCD that follows
  // `value_handle`. A server that pushes notifications before this is talking
  // to nobody.
  bool subscribed(uint16_t value_handle) const;

  // Build a notification for a characteristic, truncated to the negotiated
  // MTU. Returns empty if nothing is subscribed, so a caller can push
  // unconditionally without spamming an unsubscribed link.
  std::vector<uint8_t> notification(uint16_t value_handle,
                                    const std::vector<uint8_t>& value) const;

  const std::vector<Attribute>& attributes() const { return attrs_; }

 private:
  std::vector<uint8_t> error(uint8_t req, uint16_t handle, uint8_t code) const;
  const Attribute* find(uint16_t handle) const;

  std::vector<Attribute> attrs_;
  uint16_t mtu_ = kDefaultMtu;
  WriteHandler on_write_;
};

}  // namespace att
}  // namespace octo

#endif  // OCTO_ATT_H
