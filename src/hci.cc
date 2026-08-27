#include "hci.h"

#include <cstdio>
#include <cstring>

namespace octo {
namespace hci {
namespace {

// A place to render the hex fallbacks the *_name() functions return. Made
// thread-local because the scanner's reader thread and the caller's thread
// both log, and a shared buffer would let one overwrite the other's name
// mid-printf.
char* scratch() {
  static thread_local char buf[32];
  return buf;
}

void put16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xff));
  out->push_back(static_cast<uint8_t>(v >> 8));
}

uint16_t get16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

void put_addr(std::vector<uint8_t>* out, const Address& addr) {
  // On the wire an address is least significant byte first, the reverse of
  // how it is written down.
  for (size_t i = 0; i < 6; ++i) out->push_back(addr.bytes[5 - i]);
}

void get_addr(const uint8_t* p, uint8_t type, Address* out) {
  for (size_t i = 0; i < 6; ++i) out->bytes[i] = p[5 - i];
  out->type = type;
}

int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 0000xxxx-0000-1000-8000-00805F9B34FB, big-endian, as Uuid holds it.
const uint8_t kBaseUuid[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                               0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb};

bool matches_base_tail(const std::array<uint8_t, 16>& b) {
  return std::memcmp(b.data() + 4, kBaseUuid + 4, 12) == 0;
}

uint16_t clamp_u16(double v, uint16_t lo, uint16_t hi) {
  if (!(v > 0)) return lo;  // also catches NaN
  if (v <= static_cast<double>(lo)) return lo;
  if (v >= static_cast<double>(hi)) return hi;
  return static_cast<uint16_t>(v);
}

}  // namespace

// --------------------------------------------------------------- naming

const char* status_name(uint8_t status) {
  switch (status) {
    case kSuccess: return "success";
    case kUnknownConnection: return "unknown connection identifier";
    case kAuthenticationFailure: return "authentication failure";
    case kPinOrKeyMissing: return "PIN or key missing";
    case kMemoryCapacityExceeded: return "memory capacity exceeded";
    case kConnectionTimeout: return "connection timeout";
    case kCommandDisallowed: return "command disallowed";
    case kRemoteUserTerminated: return "remote user terminated the connection";
    case kLocalHostTerminated: return "local host terminated the connection";
    case kUnsupportedFeature: return "unsupported feature or parameter";
    case kInvalidParameters: return "invalid HCI command parameters";
    case kUnspecifiedError: return "unspecified error";
    case kUnsupportedLmpParameter: return "unsupported LMP parameter";
    case kInstantPassed: return "instant passed";
    case kControllerBusy: return "controller busy";
    case kConnectionFailedToEstablish: return "connection failed to establish";
    default: break;
  }
  std::snprintf(scratch(), 32, "status 0x%02x", status);
  return scratch();
}

const char* opcode_name(uint16_t opcode) {
  switch (opcode) {
    case kOpDisconnect: return "Disconnect";
    case kOpReset: return "Reset";
    case kOpSetEventMask: return "Set Event Mask";
    case kOpReadLocalVersion: return "Read Local Version";
    case kOpReadLocalCommands: return "Read Local Supported Commands";
    case kOpReadBdAddr: return "Read BD_ADDR";
    case kOpLeSetEventMask: return "LE Set Event Mask";
    case kOpLeReadBufferSize: return "LE Read Buffer Size";
    case kOpLeReadLocalFeatures: return "LE Read Local Features";
    case kOpLeSetRandomAddress: return "LE Set Random Address";
    case kOpLeSetAdvParams: return "LE Set Advertising Parameters";
    case kOpLeReadAdvChannelTxPower: return "LE Read Advertising Channel TX Power";
    case kOpLeSetAdvData: return "LE Set Advertising Data";
    case kOpLeSetScanResponseData: return "LE Set Scan Response Data";
    case kOpLeSetAdvEnable: return "LE Set Advertise Enable";
    case kOpLeSetScanParams: return "LE Set Scan Parameters";
    case kOpLeSetScanEnable: return "LE Set Scan Enable";
    case kOpLeCreateConnection: return "LE Create Connection";
    case kOpLeCreateConnectionCancel: return "LE Create Connection Cancel";
    case kOpLeConnectionUpdate: return "LE Connection Update";
    case kOpLeEncrypt: return "LE Encrypt";
    case kOpLeRand: return "LE Rand";
    case kOpLeStartEncryption: return "LE Start Encryption";
    case kOpLeLtkRequestReply: return "LE Long Term Key Request Reply";
    case kOpLeLtkRequestNegReply: return "LE Long Term Key Request Negative Reply";
    case kOpLeSetDataLength: return "LE Set Data Length";
    case kOpLeReadLocalP256: return "LE Read Local P-256 Public Key";
    case kOpLeGenerateDhKey: return "LE Generate DHKey";
    default: break;
  }
  std::snprintf(scratch(), 32, "opcode 0x%04x", opcode);
  return scratch();
}

const char* event_name(uint8_t code, uint8_t subevent) {
  if (code == kEvtLeMeta) {
    switch (subevent) {
      case kLeConnectionComplete: return "LE Connection Complete";
      case kLeAdvertisingReport: return "LE Advertising Report";
      case kLeConnectionUpdateComplete: return "LE Connection Update Complete";
      case kLeReadRemoteFeaturesComplete: return "LE Read Remote Features Complete";
      case kLeLongTermKeyRequest: return "LE Long Term Key Request";
      case kLeDataLengthChange: return "LE Data Length Change";
      case kLeReadLocalP256Complete: return "LE Read Local P-256 Public Key Complete";
      case kLeGenerateDhKeyComplete: return "LE Generate DHKey Complete";
      case kLeEnhancedConnectionComplete: return "LE Enhanced Connection Complete";
      case kLePhyUpdateComplete: return "LE PHY Update Complete";
      default: break;
    }
    std::snprintf(scratch(), 32, "LE subevent 0x%02x", subevent);
    return scratch();
  }
  switch (code) {
    case kEvtDisconnectionComplete: return "Disconnection Complete";
    case kEvtEncryptionChange: return "Encryption Change";
    case kEvtCommandComplete: return "Command Complete";
    case kEvtCommandStatus: return "Command Status";
    case kEvtHardwareError: return "Hardware Error";
    case kEvtNumCompletedPackets: return "Number Of Completed Packets";
    case kEvtEncryptionKeyRefresh: return "Encryption Key Refresh Complete";
    default: break;
  }
  std::snprintf(scratch(), 32, "event 0x%02x", code);
  return scratch();
}

// ------------------------------------------------------------- addresses

std::string address_to_string(const Address& addr) {
  char buf[18];
  std::snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X", addr.bytes[0],
                addr.bytes[1], addr.bytes[2], addr.bytes[3], addr.bytes[4],
                addr.bytes[5]);
  return std::string(buf);
}

bool address_from_string(const std::string& text, Address* out) {
  if (!out) return false;
  Address addr;
  size_t nibble = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == ':' || c == '-') continue;
    int v = hex_val(c);
    if (v < 0) return false;
    if (nibble >= 12) return false;
    uint8_t& b = addr.bytes[nibble / 2];
    if (nibble % 2 == 0) {
      b = static_cast<uint8_t>(v << 4);
    } else {
      b = static_cast<uint8_t>(b | v);
    }
    ++nibble;
  }
  if (nibble != 12) return false;
  addr.type = out->type;  // caller keeps whatever type it had already decided
  *out = addr;
  return true;
}

RandomKind random_kind(const Address& addr) {
  if (addr.type != kAddrRandom && addr.type != kAddrRandomIdentity) {
    return RandomKind::kNotRandom;
  }
  switch (addr.bytes[0] >> 6) {
    case 0x03: return RandomKind::kStatic;
    case 0x01: return RandomKind::kResolvablePrivate;
    case 0x00: return RandomKind::kNonResolvable;
    default: break;
  }
  return RandomKind::kNonResolvable;
}

bool address_is_stable(const Address& addr) {
  switch (random_kind(addr)) {
    case RandomKind::kNotRandom: return true;   // public: assigned, permanent
    case RandomKind::kStatic: return true;      // fixed until the device reboots
    default: return false;
  }
}

// ------------------------------------------------------------------ UUIDs

bool Uuid::is_16bit() const {
  return bytes[0] == 0 && bytes[1] == 0 && matches_base_tail(bytes);
}

bool Uuid::is_32bit() const {
  return matches_base_tail(bytes) && !(bytes[0] == 0 && bytes[1] == 0);
}

uint16_t Uuid::short16() const {
  return static_cast<uint16_t>((bytes[2] << 8) | bytes[3]);
}

Uuid uuid_from_16(uint16_t value) {
  Uuid u;
  std::memcpy(u.bytes.data(), kBaseUuid, 16);
  u.bytes[2] = static_cast<uint8_t>(value >> 8);
  u.bytes[3] = static_cast<uint8_t>(value & 0xff);
  return u;
}

Uuid uuid_from_32(uint32_t value) {
  Uuid u;
  std::memcpy(u.bytes.data(), kBaseUuid, 16);
  u.bytes[0] = static_cast<uint8_t>(value >> 24);
  u.bytes[1] = static_cast<uint8_t>((value >> 16) & 0xff);
  u.bytes[2] = static_cast<uint8_t>((value >> 8) & 0xff);
  u.bytes[3] = static_cast<uint8_t>(value & 0xff);
  return u;
}

bool uuid_from_string(const std::string& text, Uuid* out) {
  if (!out) return false;
  std::string hex;
  hex.reserve(32);
  for (char c : text) {
    if (c == '-') continue;
    if (hex_val(c) < 0) return false;
    hex.push_back(c);
  }
  if (hex.size() == 4) {
    *out = uuid_from_16(static_cast<uint16_t>(std::stoul(hex, nullptr, 16)));
    return true;
  }
  if (hex.size() == 8) {
    *out = uuid_from_32(static_cast<uint32_t>(std::stoul(hex, nullptr, 16)));
    return true;
  }
  if (hex.size() != 32) return false;
  Uuid u;
  for (size_t i = 0; i < 16; ++i) {
    u.bytes[i] = static_cast<uint8_t>((hex_val(hex[i * 2]) << 4) |
                                      hex_val(hex[i * 2 + 1]));
  }
  *out = u;
  return true;
}

Uuid uuid_const(const std::string& text) {
  Uuid u;
  // A malformed constant is a bug in this program, not in the environment, so
  // it returns the all-zero UUID rather than aborting a daemon at start-up.
  // Nothing matches all-zero, which makes the failure loud in the logs and
  // harmless on the air.
  if (!uuid_from_string(text, &u)) return Uuid();
  return u;
}

std::string uuid_to_string(const Uuid& uuid) {
  char buf[40];
  if (uuid.is_16bit()) {
    std::snprintf(buf, sizeof buf, "%04X", uuid.short16());
    return std::string(buf);
  }
  const uint8_t* b = uuid.bytes.data();
  std::snprintf(buf, sizeof buf,
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                "%02x%02x%02x%02x%02x%02x",
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                b[10], b[11], b[12], b[13], b[14], b[15]);
  return std::string(buf);
}

std::vector<uint8_t> uuid_to_le(const Uuid& uuid) {
  if (uuid.is_16bit()) {
    uint16_t v = uuid.short16();
    return {static_cast<uint8_t>(v & 0xff), static_cast<uint8_t>(v >> 8)};
  }
  std::vector<uint8_t> out(16);
  for (size_t i = 0; i < 16; ++i) out[i] = uuid.bytes[15 - i];
  return out;
}

bool uuid_from_le(const uint8_t* data, size_t len, Uuid* out) {
  if (!data || !out) return false;
  if (len == 2) {
    *out = uuid_from_16(get16(data));
    return true;
  }
  if (len == 4) {
    uint32_t v = static_cast<uint32_t>(data[0]) |
                 (static_cast<uint32_t>(data[1]) << 8) |
                 (static_cast<uint32_t>(data[2]) << 16) |
                 (static_cast<uint32_t>(data[3]) << 24);
    *out = uuid_from_32(v);
    return true;
  }
  if (len == 16) {
    Uuid u;
    for (size_t i = 0; i < 16; ++i) u.bytes[i] = data[15 - i];
    *out = u;
    return true;
  }
  return false;
}

// --------------------------------------------------------------- commands

std::vector<uint8_t> build_command(uint16_t opcode,
                                   const std::vector<uint8_t>& params) {
  std::vector<uint8_t> out;
  out.reserve(4 + params.size());
  out.push_back(kPacketCommand);
  put16(&out, opcode);
  out.push_back(static_cast<uint8_t>(params.size()));
  out.insert(out.end(), params.begin(), params.end());
  return out;
}

std::vector<uint8_t> le_set_adv_params(uint16_t interval_min_units,
                                       uint16_t interval_max_units,
                                       uint8_t adv_type, uint8_t own_addr_type,
                                       const Address& peer, uint8_t channel_map,
                                       uint8_t filter_policy) {
  std::vector<uint8_t> p;
  put16(&p, interval_min_units);
  put16(&p, interval_max_units);
  p.push_back(adv_type);
  p.push_back(own_addr_type);
  p.push_back(peer.type);
  put_addr(&p, peer);
  p.push_back(channel_map);
  p.push_back(filter_policy);
  return p;
}

bool le_set_adv_data(const std::vector<uint8_t>& data,
                     std::vector<uint8_t>* out) {
  if (!out || data.size() > 31) return false;
  out->clear();
  out->reserve(32);
  out->push_back(static_cast<uint8_t>(data.size()));
  out->insert(out->end(), data.begin(), data.end());
  out->resize(32, 0);  // the command takes a fixed 31-byte block
  return true;
}

bool le_set_scan_response_data(const std::vector<uint8_t>& data,
                               std::vector<uint8_t>* out) {
  return le_set_adv_data(data, out);  // identical shape
}

std::vector<uint8_t> le_set_adv_enable(bool enable) {
  return {static_cast<uint8_t>(enable ? 1 : 0)};
}

std::vector<uint8_t> le_set_scan_params(bool active, uint16_t interval_units,
                                        uint16_t window_units,
                                        uint8_t own_addr_type,
                                        uint8_t filter_policy) {
  std::vector<uint8_t> p;
  p.push_back(static_cast<uint8_t>(active ? 1 : 0));
  put16(&p, interval_units);
  put16(&p, window_units);
  p.push_back(own_addr_type);
  p.push_back(filter_policy);
  return p;
}

std::vector<uint8_t> le_set_scan_enable(bool enable, bool filter_duplicates) {
  return {static_cast<uint8_t>(enable ? 1 : 0),
          static_cast<uint8_t>(filter_duplicates ? 1 : 0)};
}

std::vector<uint8_t> le_create_connection(
    const Address& peer, uint16_t scan_interval_units, uint16_t scan_window_units,
    uint16_t conn_interval_min_units, uint16_t conn_interval_max_units,
    uint16_t latency, uint16_t supervision_timeout_units,
    uint8_t own_addr_type) {
  std::vector<uint8_t> p;
  put16(&p, scan_interval_units);
  put16(&p, scan_window_units);
  p.push_back(0x00);  // no filter list: we name the peer explicitly
  p.push_back(peer.type);
  put_addr(&p, peer);
  p.push_back(own_addr_type);
  put16(&p, conn_interval_min_units);
  put16(&p, conn_interval_max_units);
  put16(&p, latency);
  put16(&p, supervision_timeout_units);
  put16(&p, 0);  // minimum CE length
  put16(&p, 0);  // maximum CE length
  return p;
}

std::vector<uint8_t> disconnect_params(uint16_t handle, uint8_t reason) {
  std::vector<uint8_t> p;
  put16(&p, handle);
  p.push_back(reason);
  return p;
}

uint16_t ms_to_adv_units(double ms) {
  // 0.625 ms units, and the specification's floor for connectable advertising
  // is 20 ms. Asking for less earns an "invalid parameters" with no hint.
  return clamp_u16(ms / 0.625, 0x0020, 0x4000);
}

uint16_t ms_to_scan_units(double ms) {
  return clamp_u16(ms / 0.625, 0x0004, 0x4000);
}

uint16_t ms_to_conn_units(double ms) {
  // Connection intervals are 1.25 ms units, 7.5 ms to 4 s.
  return clamp_u16(ms / 1.25, 0x0006, 0x0c80);
}

uint16_t ms_to_supervision_units(double ms) {
  // Supervision timeout is in 10 ms units, 100 ms to 32 s.
  return clamp_u16(ms / 10.0, 0x000a, 0x0c80);
}

// --------------------------------------------------------------- parsing

size_t parse_stream(const uint8_t* data, size_t len, std::vector<Packet>* out) {
  if (!data || !out) return 0;
  size_t off = 0;
  while (off < len) {
    uint8_t type = data[off];
    size_t avail = len - off - 1;
    const uint8_t* body = data + off + 1;
    size_t need = 0;
    switch (type) {
      case kPacketEvent:
        if (avail < 2) return off;
        need = 2 + body[1];
        break;
      case kPacketAclData:
        if (avail < 4) return off;
        need = 4 + get16(body + 2);
        break;
      case kPacketCommand:
        if (avail < 3) return off;
        need = 3 + body[2];
        break;
      case kPacketIsoData:
        if (avail < 4) return off;
        need = 4 + (get16(body + 2) & 0x3fff);
        break;
      case kPacketScoData:
        if (avail < 3) return off;
        need = 3 + body[2];
        break;
      default:
        // A byte that begins no known packet means the stream is out of step.
        // Drop exactly one byte so the next candidate is examined, rather than
        // discarding the buffer and losing a real packet behind the garbage.
        ++off;
        continue;
    }
    if (avail < need) return off;  // a partial packet: wait for more
    Packet pkt;
    pkt.type = type;
    pkt.payload.assign(body, body + need);
    out->push_back(std::move(pkt));
    off += 1 + need;
  }
  return off;
}

bool parse_event(const std::vector<uint8_t>& payload, Event* out) {
  if (!out || payload.size() < 2) return false;
  uint8_t plen = payload[1];
  if (payload.size() < static_cast<size_t>(2) + plen) return false;
  out->code = payload[0];
  out->params.assign(payload.begin() + 2, payload.begin() + 2 + plen);
  out->subevent = (out->code == kEvtLeMeta && !out->params.empty())
                      ? out->params[0]
                      : 0;
  return true;
}

bool parse_command_complete(const Event& evt, CommandComplete* out) {
  if (!out || evt.code != kEvtCommandComplete || evt.params.size() < 3) {
    return false;
  }
  out->num_packets = evt.params[0];
  out->opcode = get16(evt.params.data() + 1);
  out->params.assign(evt.params.begin() + 3, evt.params.end());
  // Almost every command returns status first. The exceptions -- LE Rand and
  // the P-256 pair -- return status first as well, so the only command whose
  // first byte is not a status is one with no return parameters at all.
  out->status = out->params.empty() ? static_cast<uint8_t>(kSuccess)
                                    : out->params[0];
  return true;
}

bool parse_command_status(const Event& evt, CommandStatus* out) {
  if (!out || evt.code != kEvtCommandStatus || evt.params.size() < 4) {
    return false;
  }
  out->status = evt.params[0];
  out->num_packets = evt.params[1];
  out->opcode = get16(evt.params.data() + 2);
  return true;
}

bool parse_connection_complete(const Event& evt, ConnectionComplete* out) {
  if (!out || evt.code != kEvtLeMeta) return false;
  const std::vector<uint8_t>& p = evt.params;
  if (evt.subevent == kLeConnectionComplete) {
    if (p.size() < 19) return false;
    out->status = p[1];
    out->handle = static_cast<uint16_t>(get16(p.data() + 2) & 0x0fff);
    out->role = p[4];
    get_addr(p.data() + 6, p[5], &out->peer);
    out->interval = get16(p.data() + 12);
    out->latency = get16(p.data() + 14);
    out->timeout = get16(p.data() + 16);
    return true;
  }
  if (evt.subevent == kLeEnhancedConnectionComplete) {
    // Same fields, with the local and peer resolvable private addresses
    // inserted after the peer address. A controller with privacy support
    // sends only this form, so a host that parses just the plain one sits
    // waiting for a connection it has already made.
    if (p.size() < 31) return false;
    out->status = p[1];
    out->handle = static_cast<uint16_t>(get16(p.data() + 2) & 0x0fff);
    out->role = p[4];
    get_addr(p.data() + 6, p[5], &out->peer);
    out->interval = get16(p.data() + 24);
    out->latency = get16(p.data() + 26);
    out->timeout = get16(p.data() + 28);
    return true;
  }
  return false;
}

bool parse_disconnection_complete(const Event& evt, DisconnectionComplete* out) {
  if (!out || evt.code != kEvtDisconnectionComplete || evt.params.size() < 4) {
    return false;
  }
  out->status = evt.params[0];
  out->handle = static_cast<uint16_t>(get16(evt.params.data() + 1) & 0x0fff);
  out->reason = evt.params[3];
  return true;
}

bool parse_encryption_change(const Event& evt, EncryptionChange* out) {
  if (!out || evt.code != kEvtEncryptionChange || evt.params.size() < 4) {
    return false;
  }
  out->status = evt.params[0];
  out->handle = static_cast<uint16_t>(get16(evt.params.data() + 1) & 0x0fff);
  out->enabled = evt.params[3];
  return true;
}

bool parse_ltk_request(const Event& evt, LongTermKeyRequest* out) {
  if (!out || evt.code != kEvtLeMeta ||
      evt.subevent != kLeLongTermKeyRequest || evt.params.size() < 13) {
    return false;
  }
  out->handle = static_cast<uint16_t>(get16(evt.params.data() + 1) & 0x0fff);
  std::memcpy(out->rand.data(), evt.params.data() + 3, 8);
  out->ediv = get16(evt.params.data() + 11);
  return true;
}

bool parse_adv_reports(const Event& evt, std::vector<AdvReport>* out) {
  if (!out || evt.code != kEvtLeMeta ||
      evt.subevent != kLeAdvertisingReport || evt.params.size() < 2) {
    return false;
  }
  const std::vector<uint8_t>& p = evt.params;
  uint8_t count = p[1];
  size_t off = 2;
  for (uint8_t i = 0; i < count; ++i) {
    // event_type, addr_type, addr[6], data_len -- nine bytes before the data,
    // then one signed byte of RSSI after it.
    if (off + 9 > p.size()) return false;
    AdvReport r;
    r.event_type = p[off];
    get_addr(p.data() + off + 2, p[off + 1], &r.addr);
    uint8_t dlen = p[off + 8];
    if (off + 9 + dlen + 1 > p.size()) return false;
    r.data.assign(p.begin() + off + 9, p.begin() + off + 9 + dlen);
    r.rssi = static_cast<int8_t>(p[off + 9 + dlen]);
    out->push_back(std::move(r));
    off += 10 + dlen;
  }
  return true;
}

// ------------------------------------------------------------- ACL framing

std::vector<uint8_t> build_acl(uint16_t handle, uint8_t pb_flag,
                               const uint8_t* data, size_t len) {
  std::vector<uint8_t> out;
  out.reserve(5 + len);
  out.push_back(kPacketAclData);
  uint16_t hf = static_cast<uint16_t>((handle & 0x0fff) |
                                      ((pb_flag & 0x03) << 12));
  put16(&out, hf);
  put16(&out, static_cast<uint16_t>(len));
  if (data && len) out.insert(out.end(), data, data + len);
  return out;
}

bool parse_acl(const std::vector<uint8_t>& payload, AclHeader* hdr,
               std::vector<uint8_t>* data) {
  if (!hdr || !data || payload.size() < 4) return false;
  uint16_t hf = get16(payload.data());
  hdr->handle = static_cast<uint16_t>(hf & 0x0fff);
  hdr->pb_flag = static_cast<uint8_t>((hf >> 12) & 0x03);
  hdr->bc_flag = static_cast<uint8_t>((hf >> 14) & 0x03);
  uint16_t dlen = get16(payload.data() + 2);
  if (payload.size() < static_cast<size_t>(4) + dlen) return false;
  data->assign(payload.begin() + 4, payload.begin() + 4 + dlen);
  return true;
}

// ------------------------------------------------- advertising data (AD)

std::vector<AdStructure> parse_ad(const uint8_t* data, size_t len) {
  std::vector<AdStructure> out;
  size_t off = 0;
  while (off < len) {
    uint8_t l = data[off];
    // Zero length is the padding an advertisement is filled out with, and it
    // means there is nothing further to read.
    if (l == 0) break;
    if (off + 1 + l > len) break;  // truncated: stop rather than guess
    AdStructure s;
    s.type = data[off + 1];
    s.value.assign(data + off + 2, data + off + 1 + l);
    out.push_back(std::move(s));
    off += 1 + l;
  }
  return out;
}

std::vector<AdStructure> parse_ad(const std::vector<uint8_t>& data) {
  return parse_ad(data.data(), data.size());
}

AdInfo summarise_ad(const std::vector<AdStructure>& structures) {
  AdInfo info;
  for (const AdStructure& s : structures) {
    switch (s.type) {
      case kAdFlags:
        if (!s.value.empty()) {
          info.has_flags = true;
          info.flags = s.value[0];
        }
        break;
      case kAdShortName:
      case kAdCompleteName:
        // A complete name wins over a shortened one, but a shortened name is
        // better than none: some devices only ever send the short form.
        if (info.name.empty() || s.type == kAdCompleteName) {
          info.name.assign(s.value.begin(), s.value.end());
        }
        break;
      case kAdTxPower:
        if (!s.value.empty()) {
          info.has_tx_power = true;
          info.tx_power = static_cast<int8_t>(s.value[0]);
        }
        break;
      case kAdIncomplete16:
      case kAdComplete16:
        for (size_t i = 0; i + 2 <= s.value.size(); i += 2) {
          Uuid u;
          if (uuid_from_le(s.value.data() + i, 2, &u)) info.services.push_back(u);
        }
        break;
      case kAdIncomplete32:
      case kAdComplete32:
        for (size_t i = 0; i + 4 <= s.value.size(); i += 4) {
          Uuid u;
          if (uuid_from_le(s.value.data() + i, 4, &u)) info.services.push_back(u);
        }
        break;
      case kAdIncomplete128:
      case kAdComplete128:
        for (size_t i = 0; i + 16 <= s.value.size(); i += 16) {
          Uuid u;
          if (uuid_from_le(s.value.data() + i, 16, &u)) {
            info.services.push_back(u);
          }
        }
        break;
      case kAdServiceData16:
      case kAdServiceData32:
      case kAdServiceData128: {
        size_t idlen = s.type == kAdServiceData16   ? 2
                       : s.type == kAdServiceData32 ? 4
                                                    : 16;
        if (s.value.size() < idlen) break;
        Uuid u;
        if (!uuid_from_le(s.value.data(), idlen, &u)) break;
        info.service_data.emplace_back(
            u, std::vector<uint8_t>(s.value.begin() + idlen, s.value.end()));
        break;
      }
      case kAdManufacturer:
        info.manufacturer = s.value;
        break;
      default:
        break;
    }
  }
  return info;
}

bool append_ad(std::vector<uint8_t>* out, uint8_t type, const uint8_t* value,
               size_t len, size_t budget) {
  if (!out) return false;
  if (len > 254) return false;
  size_t need = 2 + len;
  if (out->size() + need > budget) return false;
  out->push_back(static_cast<uint8_t>(1 + len));
  out->push_back(type);
  if (value && len) out->insert(out->end(), value, value + len);
  return true;
}

bool append_ad(std::vector<uint8_t>* out, uint8_t type,
               const std::vector<uint8_t>& value, size_t budget) {
  return append_ad(out, type, value.data(), value.size(), budget);
}

bool append_ad_flags(std::vector<uint8_t>* out, uint8_t flags, size_t budget) {
  return append_ad(out, kAdFlags, &flags, 1, budget);
}

bool append_ad_name(std::vector<uint8_t>* out, const std::string& name,
                    bool complete, size_t budget) {
  // An empty name is not the same as no name: a zero-length complete-name
  // structure is a device asserting it has none. Callers that want the field
  // gone simply do not call this, which is the distinction that mattered when
  // macOS kept spending the budget on a name we did not want.
  return append_ad(out, complete ? kAdCompleteName : kAdShortName,
                   reinterpret_cast<const uint8_t*>(name.data()), name.size(),
                   budget);
}

bool append_ad_service(std::vector<uint8_t>* out, const Uuid& uuid,
                       bool complete, size_t budget) {
  std::vector<uint8_t> le = uuid_to_le(uuid);
  uint8_t type;
  if (le.size() == 2) {
    type = complete ? kAdComplete16 : kAdIncomplete16;
  } else {
    type = complete ? kAdComplete128 : kAdIncomplete128;
  }
  return append_ad(out, type, le, budget);
}

bool append_ad_service_data(std::vector<uint8_t>* out, const Uuid& uuid,
                            const std::vector<uint8_t>& data, size_t budget) {
  std::vector<uint8_t> le = uuid_to_le(uuid);
  std::vector<uint8_t> value = le;
  value.insert(value.end(), data.begin(), data.end());
  uint8_t type = le.size() == 2 ? kAdServiceData16 : kAdServiceData128;
  return append_ad(out, type, value, budget);
}

std::string to_hex(const uint8_t* data, size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0x0f]);
  }
  return out;
}

std::string to_hex(const std::vector<uint8_t>& data) {
  return to_hex(data.data(), data.size());
}

std::vector<std::string> describe_ad(const std::vector<uint8_t>& data) {
  std::vector<std::string> lines;
  for (const AdStructure& s : parse_ad(data)) {
    std::string line;
    char buf[64];
    switch (s.type) {
      case kAdFlags: {
        uint8_t f = s.value.empty() ? 0 : s.value[0];
        std::snprintf(buf, sizeof buf, "flags 0x%02x", f);
        line = buf;
        if (f & kFlagLimitedDiscoverable) line += " limited-discoverable";
        if (f & kFlagGeneralDiscoverable) line += " general-discoverable";
        if (f & kFlagBrEdrNotSupported) line += " le-only";
        break;
      }
      case kAdShortName:
      case kAdCompleteName:
        line = (s.type == kAdCompleteName ? "name " : "short name ") +
               std::string(s.value.begin(), s.value.end());
        break;
      case kAdTxPower:
        std::snprintf(buf, sizeof buf, "tx power %d dBm",
                      s.value.empty() ? 0 : static_cast<int8_t>(s.value[0]));
        line = buf;
        break;
      case kAdIncomplete16:
      case kAdComplete16:
      case kAdIncomplete32:
      case kAdComplete32:
      case kAdIncomplete128:
      case kAdComplete128: {
        size_t w = (s.type == kAdIncomplete16 || s.type == kAdComplete16)   ? 2
                   : (s.type == kAdIncomplete32 || s.type == kAdComplete32) ? 4
                                                                            : 16;
        line = "services";
        for (size_t i = 0; i + w <= s.value.size(); i += w) {
          Uuid u;
          if (uuid_from_le(s.value.data() + i, w, &u)) {
            line += " " + uuid_to_string(u);
          }
        }
        break;
      }
      case kAdServiceData16:
      case kAdServiceData32:
      case kAdServiceData128: {
        size_t w = s.type == kAdServiceData16   ? 2
                   : s.type == kAdServiceData32 ? 4
                                                : 16;
        if (s.value.size() < w) {
          line = "service data (truncated)";
          break;
        }
        Uuid u;
        uuid_from_le(s.value.data(), w, &u);
        line = "service data " + uuid_to_string(u) + " = " +
               to_hex(s.value.data() + w, s.value.size() - w);
        break;
      }
      case kAdManufacturer: {
        if (s.value.size() >= 2) {
          std::snprintf(buf, sizeof buf, "manufacturer 0x%04x = ",
                        static_cast<unsigned>(s.value[0] | (s.value[1] << 8)));
          line = std::string(buf) +
                 to_hex(s.value.data() + 2, s.value.size() - 2);
        } else {
          line = "manufacturer (truncated)";
        }
        break;
      }
      default:
        std::snprintf(buf, sizeof buf, "type 0x%02x = ", s.type);
        line = std::string(buf) + to_hex(s.value);
        break;
    }
    lines.push_back(line);
  }
  return lines;
}

}  // namespace hci
}  // namespace octo
