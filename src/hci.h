// The Bluetooth Host Controller Interface, as spoken to an nRF52840 dongle.
//
// This is the other radio. CoreBluetooth is an API that decides a great deal
// on our behalf -- what an advertisement may contain, which of its bytes we
// are allowed to see, whether a 128-bit UUID appears on the air at all -- and
// doc/zoom-bta1-notes.md is a record of a week spent losing to exactly those
// decisions. HCI is the layer underneath all of that: a controller does what
// the host tells it, and the host is this file.
//
// The dongle runs a stock Zephyr `hci_uart` image, so there is no firmware in
// this project to maintain. It presents a USB CDC serial port carrying H4
// framing, and every decision worth making is made up here in portable C++
// that tests/test_hci.cc can check on a machine with no dongle in it. That is
// the same split scanner.h already draws around CoreBluetooth, for the same
// reason: the parts that can be tested should be the parts that are hard.
//
// Byte order: HCI is little-endian everywhere, including inside the 128-bit
// UUIDs it carries, which are therefore the reverse of how a UUID is written
// down. See uuid_from_string / uuid_to_string, which are the only sanctioned
// way to cross that boundary.
//
// References are to the Bluetooth Core Specification 5.4, Vol 4 Part E (HCI)
// and Vol 3 Part C section 11 (the advertising data format).
#ifndef OCTO_HCI_H
#define OCTO_HCI_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace octo {
namespace hci {

// ------------------------------------------------------------- H4 framing

// The one byte a UART or CDC transport puts in front of each packet to say
// which of the four streams it belongs to (Vol 4 Part A section 2).
enum PacketType : uint8_t {
  kPacketCommand = 0x01,
  kPacketAclData = 0x02,
  kPacketScoData = 0x03,
  kPacketEvent = 0x04,
  kPacketIsoData = 0x05,
};

// ---------------------------------------------------------------- opcodes

// An opcode is OGF<<10 | OCF. They are spelled out as combined constants
// because that is how they appear on the wire and in every capture, and
// reassembling them from two halves at each use site only adds a place to get
// the shift wrong.
enum Opcode : uint16_t {
  kOpDisconnect = 0x0406,

  kOpReset = 0x0c03,
  kOpSetEventMask = 0x0c01,

  kOpReadLocalVersion = 0x1001,
  kOpReadLocalCommands = 0x1002,
  kOpReadBdAddr = 0x1009,

  kOpLeSetEventMask = 0x2001,
  kOpLeReadBufferSize = 0x2002,
  kOpLeReadLocalFeatures = 0x2003,
  kOpLeSetRandomAddress = 0x2005,
  kOpLeSetAdvParams = 0x2006,
  kOpLeReadAdvChannelTxPower = 0x2007,
  kOpLeSetAdvData = 0x2008,
  kOpLeSetScanResponseData = 0x2009,
  kOpLeSetAdvEnable = 0x200a,
  kOpLeSetScanParams = 0x200b,
  kOpLeSetScanEnable = 0x200c,
  kOpLeCreateConnection = 0x200d,
  kOpLeCreateConnectionCancel = 0x200e,
  kOpLeConnectionUpdate = 0x2013,
  kOpLeEncrypt = 0x2017,
  kOpLeRand = 0x2018,
  kOpLeStartEncryption = 0x2019,
  kOpLeLtkRequestReply = 0x201a,
  kOpLeLtkRequestNegReply = 0x201b,
  kOpLeSetDataLength = 0x2022,
  kOpLeReadLocalP256 = 0x2025,
  kOpLeGenerateDhKey = 0x2026,
};

// ----------------------------------------------------------------- events

enum EventCode : uint8_t {
  kEvtDisconnectionComplete = 0x05,
  kEvtEncryptionChange = 0x08,
  kEvtCommandComplete = 0x0e,
  kEvtCommandStatus = 0x0f,
  kEvtHardwareError = 0x10,
  kEvtNumCompletedPackets = 0x13,
  kEvtEncryptionKeyRefresh = 0x30,
  kEvtLeMeta = 0x3e,
};

enum LeSubevent : uint8_t {
  kLeConnectionComplete = 0x01,
  kLeAdvertisingReport = 0x02,
  kLeConnectionUpdateComplete = 0x03,
  kLeReadRemoteFeaturesComplete = 0x04,
  kLeLongTermKeyRequest = 0x05,
  kLeDataLengthChange = 0x07,
  kLeReadLocalP256Complete = 0x08,
  kLeGenerateDhKeyComplete = 0x09,
  kLeEnhancedConnectionComplete = 0x0a,
  kLePhyUpdateComplete = 0x0c,
};

// Status 0x00 is success; everything else is a reason. Only the codes this
// program can actually provoke are named, and status_name() falls back to hex
// for the rest rather than pretending to a completeness it does not have.
enum Status : uint8_t {
  kSuccess = 0x00,
  kUnknownConnection = 0x02,
  kAuthenticationFailure = 0x05,
  kPinOrKeyMissing = 0x06,
  kMemoryCapacityExceeded = 0x07,
  kConnectionTimeout = 0x08,
  kCommandDisallowed = 0x0c,
  kRemoteUserTerminated = 0x13,
  kLocalHostTerminated = 0x16,
  kUnsupportedFeature = 0x11,
  kInvalidParameters = 0x12,
  kUnspecifiedError = 0x1f,
  kUnsupportedLmpParameter = 0x20,
  kInstantPassed = 0x28,
  kControllerBusy = 0x3a,
  kConnectionFailedToEstablish = 0x3e,
};

const char* status_name(uint8_t status);
const char* opcode_name(uint16_t opcode);
const char* event_name(uint8_t code, uint8_t subevent);

// ------------------------------------------------------------- addresses

// An address is six bytes plus the one bit that says what those bytes mean.
// The bit is not decoration: the same six bytes as a public address and as a
// random address are different devices, and a connection request that gets it
// wrong is simply never answered.
enum AddressType : uint8_t {
  kAddrPublic = 0x00,
  kAddrRandom = 0x01,
  kAddrPublicIdentity = 0x02,
  kAddrRandomIdentity = 0x03,
};

struct Address {
  std::array<uint8_t, 6> bytes{};
  uint8_t type = kAddrPublic;

  bool operator==(const Address& o) const {
    return bytes == o.bytes && type == o.type;
  }
  bool operator<(const Address& o) const {
    if (bytes != o.bytes) return bytes < o.bytes;
    return type < o.type;
  }
};

// "C0:1A:2B:3C:4D:5E" -- most significant byte first, the way an address is
// printed everywhere, which is the reverse of how it travels on the wire.
std::string address_to_string(const Address& addr);
bool address_from_string(const std::string& text, Address* out);

// A random address carries its own kind in the top two bits of the last byte
// (Vol 6 Part B section 1.3.2). A resolvable private address rotates roughly
// every fifteen minutes, which is what makes a device set diff worthless --
// see the note in doc/zoom-bta1-notes.md about the scan that proved nothing.
enum class RandomKind { kNotRandom, kStatic, kResolvablePrivate, kNonResolvable };
RandomKind random_kind(const Address& addr);

// True when this address will still name the same device in an hour. Anything
// else should not be written to a config file or a camera database.
bool address_is_stable(const Address& addr);

// ------------------------------------------------------------------ UUIDs

// Held big-endian -- the order a UUID is written and read by a human. Every
// conversion to and from the wire happens in this file, so no caller has to
// remember which way round the bytes go.
struct Uuid {
  std::array<uint8_t, 16> bytes{};

  bool operator==(const Uuid& o) const { return bytes == o.bytes; }
  bool operator!=(const Uuid& o) const { return !(*this == o); }
  bool operator<(const Uuid& o) const { return bytes < o.bytes; }

  // True when this is one of the Bluetooth SIG's short UUIDs, which travel in
  // two or four bytes rather than sixteen.
  bool is_16bit() const;
  bool is_32bit() const;
  uint16_t short16() const;  // meaningless unless is_16bit()
};

// The base every short UUID expands against: 0000xxxx-0000-1000-8000-00805F9B34FB.
Uuid uuid_from_16(uint16_t value);
Uuid uuid_from_32(uint32_t value);

// Accepts the canonical 36-character form with or without dashes, and the
// short "180A" and "FDAC" forms, which expand against the base above.
bool uuid_from_string(const std::string& text, Uuid* out);
Uuid uuid_const(const std::string& text);  // for constants known good

// Prints short UUIDs short and long ones long, matching how each is normally
// written and how CoreBluetooth reports them, so a log line from either radio
// reads the same.
std::string uuid_to_string(const Uuid& uuid);

// Wire order is little-endian: the reverse of `bytes`. Both directions are
// spelled out because reversing in place at the call site is the single most
// common way to end up hunting a device that does not exist.
std::vector<uint8_t> uuid_to_le(const Uuid& uuid);
bool uuid_from_le(const uint8_t* data, size_t len, Uuid* out);  // len 2, 4 or 16

// --------------------------------------------------------------- commands

// A command as it goes to the controller, H4 byte included, ready to write.
std::vector<uint8_t> build_command(uint16_t opcode,
                                   const std::vector<uint8_t>& params = {});

// The parameter blocks, built separately from the framing so a test can check
// the bytes of an advertisement without a controller in the loop -- which is
// the whole point of the exercise, given that what macOS put on the air is
// still an open question.

// Advertising type (Vol 4 Part E 7.8.5). Connectable undirected is the one a
// device that wants to be talked to uses.
enum AdvType : uint8_t {
  kAdvInd = 0x00,          // connectable, scannable, undirected
  kAdvDirectIndHigh = 0x01,
  kAdvScanInd = 0x02,      // scannable, not connectable
  kAdvNonconnInd = 0x03,   // neither: a beacon
  kAdvDirectIndLow = 0x04,
};

// Intervals are in units of 0.625 ms and the controller enforces a floor of
// 20 ms for connectable advertising.
std::vector<uint8_t> le_set_adv_params(uint16_t interval_min_units,
                                       uint16_t interval_max_units,
                                       uint8_t adv_type, uint8_t own_addr_type,
                                       const Address& peer,
                                       uint8_t channel_map = 0x07,
                                       uint8_t filter_policy = 0x00);

// Both of these pad to the fixed 31-byte block the command expects. Data
// longer than 31 bytes is rejected rather than truncated: a silently shortened
// advertisement is a device that is on the air saying the wrong thing, which
// is far harder to diagnose than one that never started.
bool le_set_adv_data(const std::vector<uint8_t>& data,
                     std::vector<uint8_t>* out);
bool le_set_scan_response_data(const std::vector<uint8_t>& data,
                               std::vector<uint8_t>* out);

std::vector<uint8_t> le_set_adv_enable(bool enable);

// Scan window and interval are also in 0.625 ms units. Passive scanning does
// not send scan requests, so it never provokes a scan response -- which is
// what a Tentacle wants, since it puts everything in the advertisement.
std::vector<uint8_t> le_set_scan_params(bool active, uint16_t interval_units,
                                        uint16_t window_units,
                                        uint8_t own_addr_type = kAddrPublic,
                                        uint8_t filter_policy = 0x00);
std::vector<uint8_t> le_set_scan_enable(bool enable, bool filter_duplicates);

std::vector<uint8_t> le_create_connection(const Address& peer,
                                          uint16_t scan_interval_units,
                                          uint16_t scan_window_units,
                                          uint16_t conn_interval_min_units,
                                          uint16_t conn_interval_max_units,
                                          uint16_t latency,
                                          uint16_t supervision_timeout_units,
                                          uint8_t own_addr_type = kAddrPublic);

std::vector<uint8_t> disconnect_params(uint16_t handle, uint8_t reason);

// Milliseconds to the units each field is actually specified in, clamped to
// the legal range so a caller cannot ask for something the controller will
// reject with a bare "invalid parameters".
uint16_t ms_to_adv_units(double ms);
uint16_t ms_to_scan_units(double ms);
uint16_t ms_to_conn_units(double ms);
uint16_t ms_to_supervision_units(double ms);

// ------------------------------------------------------------ event parsing

struct Event {
  uint8_t code = 0;
  uint8_t subevent = 0;  // meaningful only when code == kEvtLeMeta
  std::vector<uint8_t> params;  // after the length byte; LE meta keeps its
                                // subevent byte at params[0]
};

// Split a byte stream from the transport into whole packets. Returns how many
// bytes were consumed; anything left over is a partial packet and must be kept
// for the next read. A malformed leading byte consumes one byte so a desynced
// stream resynchronises rather than wedging.
struct Packet {
  uint8_t type = 0;
  std::vector<uint8_t> payload;  // everything after the H4 type byte
};
size_t parse_stream(const uint8_t* data, size_t len, std::vector<Packet>* out);

bool parse_event(const std::vector<uint8_t>& payload, Event* out);

struct CommandComplete {
  uint8_t num_packets = 0;
  uint16_t opcode = 0;
  uint8_t status = 0;          // params[0] for nearly every command
  std::vector<uint8_t> params; // return parameters after the opcode
};
bool parse_command_complete(const Event& evt, CommandComplete* out);

struct CommandStatus {
  uint8_t status = 0;
  uint8_t num_packets = 0;
  uint16_t opcode = 0;
};
bool parse_command_status(const Event& evt, CommandStatus* out);

struct ConnectionComplete {
  uint8_t status = 0;
  uint16_t handle = 0;
  uint8_t role = 0;  // 0 central, 1 peripheral
  Address peer;
  uint16_t interval = 0;
  uint16_t latency = 0;
  uint16_t timeout = 0;
};
// Handles both LE Connection Complete and its Enhanced form, which adds the
// local and peer resolvable addresses in the middle. A controller that
// supports privacy sends only the enhanced one, so a host that parses just the
// first will connect and then never notice it has.
bool parse_connection_complete(const Event& evt, ConnectionComplete* out);

struct DisconnectionComplete {
  uint8_t status = 0;
  uint16_t handle = 0;
  uint8_t reason = 0;
};
bool parse_disconnection_complete(const Event& evt, DisconnectionComplete* out);

struct EncryptionChange {
  uint8_t status = 0;
  uint16_t handle = 0;
  uint8_t enabled = 0;
};
bool parse_encryption_change(const Event& evt, EncryptionChange* out);

struct LongTermKeyRequest {
  uint16_t handle = 0;
  std::array<uint8_t, 8> rand{};
  uint16_t ediv = 0;
};
bool parse_ltk_request(const Event& evt, LongTermKeyRequest* out);

// One entry of an LE Advertising Report. A single event can carry several.
struct AdvReport {
  uint8_t event_type = 0;
  Address addr;
  std::vector<uint8_t> data;  // the raw AD structures, undecoded
  int rssi = 0;
};
enum AdvEventType : uint8_t {
  kReportAdvInd = 0x00,
  kReportAdvDirectInd = 0x01,
  kReportAdvScanInd = 0x02,
  kReportAdvNonconnInd = 0x03,
  kReportScanResponse = 0x04,
};
bool parse_adv_reports(const Event& evt, std::vector<AdvReport>* out);

// ------------------------------------------------------------- ACL framing

struct AclHeader {
  uint16_t handle = 0;
  uint8_t pb_flag = 0;  // 0b10 first fragment of a host packet, 0b01 continuing
  uint8_t bc_flag = 0;
};
enum AclPbFlag : uint8_t {
  kAclContinuing = 0x01,
  kAclFirstFlushable = 0x02,
};

std::vector<uint8_t> build_acl(uint16_t handle, uint8_t pb_flag,
                               const uint8_t* data, size_t len);
bool parse_acl(const std::vector<uint8_t>& payload, AclHeader* hdr,
               std::vector<uint8_t>* data);

// ------------------------------------------------- advertising data (AD)

// Vol 3 Part C section 11: a sequence of length-prefixed structures, each one
// byte of type followed by its value.
enum AdType : uint8_t {
  kAdFlags = 0x01,
  kAdIncomplete16 = 0x02,
  kAdComplete16 = 0x03,
  kAdIncomplete32 = 0x04,
  kAdComplete32 = 0x05,
  kAdIncomplete128 = 0x06,
  kAdComplete128 = 0x07,
  kAdShortName = 0x08,
  kAdCompleteName = 0x09,
  kAdTxPower = 0x0a,
  kAdServiceData16 = 0x16,
  kAdAppearance = 0x19,
  kAdServiceData32 = 0x20,
  kAdServiceData128 = 0x21,
  kAdManufacturer = 0xff,
};

enum AdFlags : uint8_t {
  kFlagLimitedDiscoverable = 0x01,
  kFlagGeneralDiscoverable = 0x02,
  kFlagBrEdrNotSupported = 0x04,
};

struct AdStructure {
  uint8_t type = 0;
  std::vector<uint8_t> value;
};

// Stops at the first structure that runs off the end rather than guessing,
// and treats a zero length as the end-of-data padding it is.
std::vector<AdStructure> parse_ad(const uint8_t* data, size_t len);
std::vector<AdStructure> parse_ad(const std::vector<uint8_t>& data);

// What a caller actually wants out of an advertisement. Kept as a struct
// rather than repeated lookups because almost every user needs several fields
// and each one is a linear walk.
struct AdInfo {
  std::string name;
  bool has_flags = false;
  uint8_t flags = 0;
  bool has_tx_power = false;
  int tx_power = 0;
  std::vector<Uuid> services;
  // Service data, keyed by the UUID it was published under. The Tentacle
  // payload this whole project began with is the FDAC entry here.
  std::vector<std::pair<Uuid, std::vector<uint8_t>>> service_data;
  std::vector<uint8_t> manufacturer;  // company ID still in the first two bytes
};
AdInfo summarise_ad(const std::vector<AdStructure>& structures);

// Building the other direction. Every one of these refuses to overflow the
// 31-byte budget, and `append_ad` reports whether the structure fit, because
// the interesting failure is not a crash but an advertisement that is missing
// the very field the experiment turns on.
bool append_ad(std::vector<uint8_t>* out, uint8_t type,
               const uint8_t* value, size_t len, size_t budget = 31);
bool append_ad(std::vector<uint8_t>* out, uint8_t type,
               const std::vector<uint8_t>& value, size_t budget = 31);
bool append_ad_flags(std::vector<uint8_t>* out, uint8_t flags,
                     size_t budget = 31);
bool append_ad_name(std::vector<uint8_t>* out, const std::string& name,
                    bool complete = true, size_t budget = 31);
bool append_ad_service(std::vector<uint8_t>* out, const Uuid& uuid,
                       bool complete = true, size_t budget = 31);
bool append_ad_service_data(std::vector<uint8_t>* out, const Uuid& uuid,
                            const std::vector<uint8_t>& data,
                            size_t budget = 31);

std::string to_hex(const uint8_t* data, size_t len);
std::string to_hex(const std::vector<uint8_t>& data);
// Render AD structures one per line, named, for the logs. This is the tool
// that would have answered in a minute what a week of guessing at
// CoreBluetooth could not: what is actually on the air.
std::vector<std::string> describe_ad(const std::vector<uint8_t>& data);

}  // namespace hci
}  // namespace octo

#endif  // OCTO_HCI_H
