#include "att.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace octo {
namespace att {
namespace {

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

void append_uuid_le(std::vector<uint8_t>* out, const Uuid& uuid) {
  std::vector<uint8_t> le = hci::uuid_to_le(uuid);
  out->insert(out->end(), le.begin(), le.end());
}

bool is_service_decl(const Uuid& type) {
  return type == hci::uuid_from_16(kUuidPrimaryService) ||
         type == hci::uuid_from_16(kUuidSecondaryService);
}

}  // namespace

// ------------------------------------------------------------------ names

const char* error_name(uint8_t code) {
  switch (code) {
    case kInvalidHandle: return "invalid handle";
    case kReadNotPermitted: return "read not permitted";
    case kWriteNotPermitted: return "write not permitted";
    case kInvalidPdu: return "invalid PDU";
    case kInsufficientAuthentication: return "insufficient authentication";
    case kRequestNotSupported: return "request not supported";
    case kInvalidOffset: return "invalid offset";
    case kInsufficientAuthorization: return "insufficient authorization";
    case kPrepareQueueFull: return "prepare queue full";
    case kAttributeNotFound: return "attribute not found";
    case kAttributeNotLong: return "attribute not long";
    case kInsufficientKeySize: return "insufficient encryption key size";
    case kInvalidAttributeValueLength: return "invalid attribute value length";
    case kUnlikelyError: return "unlikely error";
    case kInsufficientEncryption: return "insufficient encryption";
    case kUnsupportedGroupType: return "unsupported group type";
    case kInsufficientResources: return "insufficient resources";
    default: break;
  }
  std::snprintf(scratch(), 32, "ATT error 0x%02x", code);
  return scratch();
}

const char* opcode_name(uint8_t opcode) {
  switch (opcode) {
    case kErrorResponse: return "Error Response";
    case kExchangeMtuRequest: return "Exchange MTU Request";
    case kExchangeMtuResponse: return "Exchange MTU Response";
    case kFindInformationRequest: return "Find Information Request";
    case kFindInformationResponse: return "Find Information Response";
    case kFindByTypeValueRequest: return "Find By Type Value Request";
    case kFindByTypeValueResponse: return "Find By Type Value Response";
    case kReadByTypeRequest: return "Read By Type Request";
    case kReadByTypeResponse: return "Read By Type Response";
    case kReadRequest: return "Read Request";
    case kReadResponse: return "Read Response";
    case kReadBlobRequest: return "Read Blob Request";
    case kReadBlobResponse: return "Read Blob Response";
    case kReadMultipleRequest: return "Read Multiple Request";
    case kReadMultipleResponse: return "Read Multiple Response";
    case kReadByGroupTypeRequest: return "Read By Group Type Request";
    case kReadByGroupTypeResponse: return "Read By Group Type Response";
    case kWriteRequest: return "Write Request";
    case kWriteResponse: return "Write Response";
    case kWriteCommand: return "Write Command";
    case kPrepareWriteRequest: return "Prepare Write Request";
    case kPrepareWriteResponse: return "Prepare Write Response";
    case kExecuteWriteRequest: return "Execute Write Request";
    case kExecuteWriteResponse: return "Execute Write Response";
    case kHandleValueNotification: return "Handle Value Notification";
    case kHandleValueIndication: return "Handle Value Indication";
    case kHandleValueConfirmation: return "Handle Value Confirmation";
    default: break;
  }
  std::snprintf(scratch(), 32, "ATT opcode 0x%02x", opcode);
  return scratch();
}

std::string properties_to_string(uint8_t props) {
  std::string out;
  auto add = [&out](const char* s) {
    if (!out.empty()) out += ",";
    out += s;
  };
  if (props & kPropBroadcast) add("broadcast");
  if (props & kPropRead) add("read");
  if (props & kPropWriteWithoutResponse) add("write-nr");
  if (props & kPropWrite) add("write");
  if (props & kPropNotify) add("notify");
  if (props & kPropIndicate) add("indicate");
  if (props & kPropSignedWrite) add("signed-write");
  if (props & kPropExtended) add("extended");
  return out;
}

// ------------------------------------------------------------------ L2CAP

std::vector<uint8_t> build_l2cap(uint16_t cid,
                                 const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> out;
  out.reserve(4 + payload.size());
  put16(&out, static_cast<uint16_t>(payload.size()));
  put16(&out, cid);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

bool parse_l2cap(const uint8_t* data, size_t len, uint16_t* cid,
                 std::vector<uint8_t>* payload) {
  if (!data || !cid || !payload || len < 4) return false;
  uint16_t plen = get16(data);
  if (len < static_cast<size_t>(4) + plen) return false;
  *cid = get16(data + 2);
  payload->assign(data + 4, data + 4 + plen);
  return true;
}

void Reassembler::reset() {
  buf_.clear();
  expecting_ = 0;
}

bool Reassembler::push(
    uint8_t pb_flag, const uint8_t* data, size_t len,
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>>* out) {
  if (!out) return false;
  if (pb_flag != hci::kAclContinuing) {
    // A first fragment abandons whatever was half-assembled. That is not
    // tidiness: a controller that drops a continuation gives no notice, so the
    // only signal that the previous frame will never finish is the next one
    // starting.
    buf_.clear();
    expecting_ = 0;
  } else if (buf_.empty()) {
    // A continuation with nothing to continue. Discard it rather than treat
    // its middle as an L2CAP header, which would invent a length.
    return true;
  }
  buf_.insert(buf_.end(), data, data + len);

  for (;;) {
    if (buf_.size() < 4) {
      expecting_ = 0;
      return true;
    }
    expecting_ = 4 + static_cast<size_t>(get16(buf_.data()));
    if (buf_.size() < expecting_) return true;
    uint16_t cid = get16(buf_.data() + 2);
    out->emplace_back(cid, std::vector<uint8_t>(buf_.begin() + 4,
                                                buf_.begin() + expecting_));
    buf_.erase(buf_.begin(), buf_.begin() + expecting_);
    expecting_ = 0;
    if (buf_.empty()) return true;
  }
}

std::vector<std::vector<uint8_t>> fragment(const std::vector<uint8_t>& frame,
                                           size_t max_acl_payload) {
  std::vector<std::vector<uint8_t>> out;
  if (max_acl_payload == 0) return out;
  size_t off = 0;
  while (off < frame.size()) {
    size_t n = std::min(max_acl_payload, frame.size() - off);
    out.emplace_back(frame.begin() + off, frame.begin() + off + n);
    off += n;
  }
  return out;
}

// -------------------------------------------------------- client: requests

std::vector<uint8_t> exchange_mtu_request(uint16_t mtu) {
  std::vector<uint8_t> p{kExchangeMtuRequest};
  put16(&p, mtu);
  return p;
}

std::vector<uint8_t> find_information_request(uint16_t start, uint16_t end) {
  std::vector<uint8_t> p{kFindInformationRequest};
  put16(&p, start);
  put16(&p, end);
  return p;
}

std::vector<uint8_t> read_by_group_type_request(uint16_t start, uint16_t end,
                                                const Uuid& type) {
  std::vector<uint8_t> p{kReadByGroupTypeRequest};
  put16(&p, start);
  put16(&p, end);
  append_uuid_le(&p, type);
  return p;
}

std::vector<uint8_t> read_by_type_request(uint16_t start, uint16_t end,
                                          const Uuid& type) {
  std::vector<uint8_t> p{kReadByTypeRequest};
  put16(&p, start);
  put16(&p, end);
  append_uuid_le(&p, type);
  return p;
}

std::vector<uint8_t> read_request(uint16_t handle) {
  std::vector<uint8_t> p{kReadRequest};
  put16(&p, handle);
  return p;
}

std::vector<uint8_t> read_blob_request(uint16_t handle, uint16_t offset) {
  std::vector<uint8_t> p{kReadBlobRequest};
  put16(&p, handle);
  put16(&p, offset);
  return p;
}

std::vector<uint8_t> write_request(uint16_t handle,
                                   const std::vector<uint8_t>& value) {
  std::vector<uint8_t> p{kWriteRequest};
  put16(&p, handle);
  p.insert(p.end(), value.begin(), value.end());
  return p;
}

std::vector<uint8_t> write_command(uint16_t handle,
                                   const std::vector<uint8_t>& value) {
  std::vector<uint8_t> p{kWriteCommand};
  put16(&p, handle);
  p.insert(p.end(), value.begin(), value.end());
  return p;
}

std::vector<uint8_t> handle_value_confirmation() {
  return {kHandleValueConfirmation};
}

// ------------------------------------------------------- client: responses

bool parse_error_response(const std::vector<uint8_t>& pdu, ErrorResponse* out) {
  if (!out || pdu.size() < 5 || pdu[0] != kErrorResponse) return false;
  out->request_opcode = pdu[1];
  out->handle = get16(pdu.data() + 2);
  out->error = pdu[4];
  return true;
}

bool parse_exchange_mtu_response(const std::vector<uint8_t>& pdu,
                                 uint16_t* mtu) {
  if (!mtu || pdu.size() < 3 || pdu[0] != kExchangeMtuResponse) return false;
  *mtu = get16(pdu.data() + 1);
  return true;
}

bool parse_read_by_group_type_response(const std::vector<uint8_t>& pdu,
                                       std::vector<ServiceRange>* out) {
  if (!out || pdu.size() < 2 || pdu[0] != kReadByGroupTypeResponse) return false;
  size_t entry = pdu[1];
  // start(2) + end(2) + UUID(2 or 16). Any other width means the peer and this
  // parser disagree about the protocol, and guessing would produce services
  // that do not exist.
  if (entry != 6 && entry != 20) return false;
  size_t uuid_len = entry - 4;
  for (size_t off = 2; off + entry <= pdu.size(); off += entry) {
    ServiceRange s;
    s.start = get16(pdu.data() + off);
    s.end = get16(pdu.data() + off + 2);
    if (!hci::uuid_from_le(pdu.data() + off + 4, uuid_len, &s.uuid)) return false;
    out->push_back(s);
  }
  return true;
}

bool parse_read_by_type_response(const std::vector<uint8_t>& pdu,
                                 std::vector<TypedValue>* out) {
  if (!out || pdu.size() < 2 || pdu[0] != kReadByTypeResponse) return false;
  size_t entry = pdu[1];
  if (entry < 3) return false;
  for (size_t off = 2; off + entry <= pdu.size(); off += entry) {
    TypedValue v;
    v.handle = get16(pdu.data() + off);
    v.value.assign(pdu.begin() + off + 2, pdu.begin() + off + entry);
    out->push_back(std::move(v));
  }
  return true;
}

bool parse_read_by_type_response_chars(const std::vector<uint8_t>& pdu,
                                       std::vector<CharDecl>* out) {
  std::vector<TypedValue> raw;
  if (!out || !parse_read_by_type_response(pdu, &raw)) return false;
  for (const TypedValue& v : raw) {
    // properties(1) + value handle(2) + UUID(2 or 16)
    if (v.value.size() != 5 && v.value.size() != 19) return false;
    CharDecl c;
    c.handle = v.handle;
    c.properties = v.value[0];
    c.value_handle = get16(v.value.data() + 1);
    if (!hci::uuid_from_le(v.value.data() + 3, v.value.size() - 3, &c.uuid)) {
      return false;
    }
    out->push_back(c);
  }
  return true;
}

bool parse_find_information_response(const std::vector<uint8_t>& pdu,
                                     std::vector<HandleUuid>* out) {
  if (!out || pdu.size() < 2 || pdu[0] != kFindInformationResponse) return false;
  size_t uuid_len = pdu[1] == 0x01 ? 2 : pdu[1] == 0x02 ? 16 : 0;
  if (uuid_len == 0) return false;
  size_t entry = 2 + uuid_len;
  for (size_t off = 2; off + entry <= pdu.size(); off += entry) {
    HandleUuid h;
    h.handle = get16(pdu.data() + off);
    if (!hci::uuid_from_le(pdu.data() + off + 2, uuid_len, &h.uuid)) return false;
    out->push_back(h);
  }
  return true;
}

bool parse_read_response(const std::vector<uint8_t>& pdu,
                         std::vector<uint8_t>* value) {
  if (!value || pdu.empty()) return false;
  if (pdu[0] != kReadResponse && pdu[0] != kReadBlobResponse) return false;
  value->assign(pdu.begin() + 1, pdu.end());
  return true;
}

bool parse_notification(const std::vector<uint8_t>& pdu, Notification* out) {
  if (!out || pdu.size() < 3) return false;
  if (pdu[0] != kHandleValueNotification && pdu[0] != kHandleValueIndication) {
    return false;
  }
  out->handle = get16(pdu.data() + 1);
  out->value.assign(pdu.begin() + 3, pdu.end());
  out->wants_confirmation = pdu[0] == kHandleValueIndication;
  return true;
}

// ------------------------------------------------------- server: building

uint16_t ServerBuilder::add_primary_service(const Uuid& uuid) {
  Attribute a;
  a.handle = next_++;
  a.type = hci::uuid_from_16(kUuidPrimaryService);
  a.value = hci::uuid_to_le(uuid);
  attrs_.push_back(std::move(a));
  return attrs_.back().handle;
}

uint16_t ServerBuilder::add_characteristic(const Uuid& uuid, uint8_t properties,
                                           const std::vector<uint8_t>& value,
                                           const std::string& description) {
  // Delegated so the declaration/value/description/CCCD layout has exactly one
  // implementation. Two copies of it would drift, and the symptom of a drifted
  // attribute table is a client that discovers a service and then reads the
  // wrong handle out of it.
  uint16_t value_handle =
      add_dynamic_characteristic(uuid, properties, nullptr, description);
  for (Attribute& a : attrs_) {
    if (a.handle == value_handle) {
      a.value = value;
      break;
    }
  }
  return value_handle;
}

uint16_t ServerBuilder::add_dynamic_characteristic(
    const Uuid& uuid, uint8_t properties,
    std::function<std::vector<uint8_t>()> read_fn,
    const std::string& description) {
  uint16_t decl_handle = next_++;
  uint16_t value_handle = next_++;

  Attribute decl;
  decl.handle = decl_handle;
  decl.type = hci::uuid_from_16(kUuidCharacteristic);
  decl.value.push_back(properties);
  decl.value.push_back(static_cast<uint8_t>(value_handle & 0xff));
  decl.value.push_back(static_cast<uint8_t>(value_handle >> 8));
  append_uuid_le(&decl.value, uuid);
  attrs_.push_back(std::move(decl));

  Attribute val;
  val.handle = value_handle;
  val.type = uuid;
  val.readable = (properties & kPropRead) != 0;
  val.writable =
      (properties & (kPropWrite | kPropWriteWithoutResponse)) != 0;
  val.read_fn = std::move(read_fn);
  attrs_.push_back(std::move(val));

  if (!description.empty()) {
    Attribute d;
    d.handle = next_++;
    d.type = hci::uuid_from_16(kUuidCharUserDescription);
    d.value.assign(description.begin(), description.end());
    attrs_.push_back(std::move(d));
  }

  // A notifying characteristic without a CCCD is one no client can subscribe
  // to, so the descriptor is not optional here even though the specification
  // leaves it to the profile.
  if (properties & (kPropNotify | kPropIndicate)) {
    Attribute cccd;
    cccd.handle = next_++;
    cccd.type = hci::uuid_from_16(kUuidClientCharConfig);
    cccd.value = {0x00, 0x00};
    cccd.readable = true;
    cccd.writable = true;
    attrs_.push_back(std::move(cccd));
  }
  return value_handle;
}

// ------------------------------------------------------- server: answering

const Attribute* Server::find(uint16_t handle) const {
  for (const Attribute& a : attrs_) {
    if (a.handle == handle) return &a;
  }
  return nullptr;
}

std::vector<uint8_t> Server::error(uint8_t req, uint16_t handle,
                                   uint8_t code) const {
  std::vector<uint8_t> p{kErrorResponse, req};
  put16(&p, handle);
  p.push_back(code);
  return p;
}

bool Server::subscribed(uint16_t value_handle) const {
  // The CCCD is the first client-characteristic-configuration descriptor after
  // the value, and before the next characteristic declaration.
  bool seen = false;
  for (const Attribute& a : attrs_) {
    if (a.handle == value_handle) {
      seen = true;
      continue;
    }
    if (!seen) continue;
    if (a.type == hci::uuid_from_16(kUuidCharacteristic) ||
        is_service_decl(a.type)) {
      return false;  // ran into the next characteristic: there was no CCCD
    }
    if (a.type == hci::uuid_from_16(kUuidClientCharConfig)) {
      return a.value.size() >= 2 && (a.value[0] & 0x03) != 0;
    }
  }
  return false;
}

std::vector<uint8_t> Server::notification(
    uint16_t value_handle, const std::vector<uint8_t>& value) const {
  if (!subscribed(value_handle)) return {};
  std::vector<uint8_t> p{kHandleValueNotification};
  put16(&p, value_handle);
  // A notification that does not fit the MTU is truncated by the sender, not
  // rejected: there is no way to ask for it to be read back.
  size_t room = mtu_ > 3 ? static_cast<size_t>(mtu_) - 3 : 0;
  size_t n = std::min(room, value.size());
  p.insert(p.end(), value.begin(), value.begin() + n);
  return p;
}

std::vector<uint8_t> Server::handle_request(const std::vector<uint8_t>& pdu) {
  if (pdu.empty()) return {};
  uint8_t op = pdu[0];

  switch (op) {
    case kExchangeMtuRequest: {
      if (pdu.size() < 3) return error(op, 0, kInvalidPdu);
      uint16_t peer = get16(pdu.data() + 1);
      // The agreed MTU is the smaller of the two proposals, and it takes
      // effect for both directions at once.
      uint16_t mine = 247;  // what a Zephyr controller comfortably carries
      mtu_ = std::min(peer, mine);
      if (mtu_ < kDefaultMtu) mtu_ = kDefaultMtu;
      std::vector<uint8_t> p{kExchangeMtuResponse};
      put16(&p, mine);
      return p;
    }

    case kFindInformationRequest: {
      if (pdu.size() < 5) return error(op, 0, kInvalidPdu);
      uint16_t start = get16(pdu.data() + 1);
      uint16_t end = get16(pdu.data() + 3);
      if (start == 0 || start > end) return error(op, start, kInvalidHandle);
      std::vector<uint8_t> p{kFindInformationResponse, 0x00};
      bool want_16 = false;
      bool chose = false;
      for (const Attribute& a : attrs_) {
        if (a.handle < start || a.handle > end) continue;
        bool is16 = a.type.is_16bit();
        // One response carries one UUID width. The first matching attribute
        // decides which, and the rest wait for the next request.
        if (!chose) {
          chose = true;
          want_16 = is16;
          p[1] = is16 ? 0x01 : 0x02;
        } else if (is16 != want_16) {
          break;
        }
        if (p.size() + 2 + (is16 ? 2u : 16u) > mtu_) break;
        put16(&p, a.handle);
        append_uuid_le(&p, a.type);
      }
      if (!chose) return error(op, start, kAttributeNotFound);
      return p;
    }

    case kFindByTypeValueRequest: {
      // start(2) end(2) type(2) value(...). Used by clients that discover a
      // service by UUID instead of enumerating everything.
      if (pdu.size() < 7) return error(op, 0, kInvalidPdu);
      uint16_t start = get16(pdu.data() + 1);
      uint16_t end = get16(pdu.data() + 3);
      Uuid type = hci::uuid_from_16(get16(pdu.data() + 5));
      std::vector<uint8_t> want(pdu.begin() + 7, pdu.end());
      if (start == 0 || start > end) return error(op, start, kInvalidHandle);
      std::vector<uint8_t> p{kFindByTypeValueResponse};
      for (size_t i = 0; i < attrs_.size(); ++i) {
        const Attribute& a = attrs_[i];
        if (a.handle < start || a.handle > end) continue;
        if (!(a.type == type) || a.value != want) continue;
        uint16_t group_end = a.handle;
        for (size_t j = i + 1; j < attrs_.size(); ++j) {
          if (is_service_decl(attrs_[j].type)) break;
          group_end = attrs_[j].handle;
        }
        if (p.size() + 4 > mtu_) break;
        put16(&p, a.handle);
        put16(&p, group_end);
      }
      if (p.size() == 1) return error(op, start, kAttributeNotFound);
      return p;
    }

    case kReadByGroupTypeRequest: {
      if (pdu.size() < 7) return error(op, 0, kInvalidPdu);
      uint16_t start = get16(pdu.data() + 1);
      uint16_t end = get16(pdu.data() + 3);
      Uuid type;
      if (!hci::uuid_from_le(pdu.data() + 5, pdu.size() - 5, &type)) {
        return error(op, start, kInvalidPdu);
      }
      if (start == 0 || start > end) return error(op, start, kInvalidHandle);
      // Only the service declarations are groups. Anything else is a client
      // asking a question this table cannot answer, and saying so is better
      // than returning an empty list it would read as "no services".
      if (!is_service_decl(type)) {
        return error(op, start, kUnsupportedGroupType);
      }
      std::vector<uint8_t> p{kReadByGroupTypeResponse, 0x00};
      size_t entry = 0;
      for (size_t i = 0; i < attrs_.size(); ++i) {
        const Attribute& a = attrs_[i];
        if (a.handle < start || a.handle > end) continue;
        if (!(a.type == type)) continue;
        size_t this_entry = 4 + a.value.size();
        if (entry == 0) {
          entry = this_entry;
          p[1] = static_cast<uint8_t>(entry);
        } else if (this_entry != entry) {
          break;  // one response, one entry width
        }
        // The group runs to the last attribute before the next service.
        uint16_t group_end = a.handle;
        for (size_t j = i + 1; j < attrs_.size(); ++j) {
          if (is_service_decl(attrs_[j].type)) break;
          group_end = attrs_[j].handle;
        }
        if (p.size() + entry > mtu_) break;
        put16(&p, a.handle);
        put16(&p, group_end);
        p.insert(p.end(), a.value.begin(), a.value.end());
      }
      if (entry == 0) return error(op, start, kAttributeNotFound);
      return p;
    }

    case kReadByTypeRequest: {
      if (pdu.size() < 7) return error(op, 0, kInvalidPdu);
      uint16_t start = get16(pdu.data() + 1);
      uint16_t end = get16(pdu.data() + 3);
      Uuid type;
      if (!hci::uuid_from_le(pdu.data() + 5, pdu.size() - 5, &type)) {
        return error(op, start, kInvalidPdu);
      }
      if (start == 0 || start > end) return error(op, start, kInvalidHandle);
      std::vector<uint8_t> p{kReadByTypeResponse, 0x00};
      size_t entry = 0;
      for (const Attribute& a : attrs_) {
        if (a.handle < start || a.handle > end) continue;
        if (!(a.type == type)) continue;
        if (!a.readable) return error(op, a.handle, kReadNotPermitted);
        std::vector<uint8_t> value = a.read_fn ? a.read_fn() : a.value;
        size_t this_entry = 2 + value.size();
        if (entry == 0) {
          entry = this_entry;
          p[1] = static_cast<uint8_t>(entry);
        } else if (this_entry != entry) {
          break;
        }
        if (p.size() + entry > mtu_) break;
        put16(&p, a.handle);
        p.insert(p.end(), value.begin(), value.end());
      }
      if (entry == 0) return error(op, start, kAttributeNotFound);
      return p;
    }

    case kReadRequest: {
      if (pdu.size() < 3) return error(op, 0, kInvalidPdu);
      uint16_t handle = get16(pdu.data() + 1);
      const Attribute* a = find(handle);
      if (!a) return error(op, handle, kInvalidHandle);
      if (!a->readable) return error(op, handle, kReadNotPermitted);
      std::vector<uint8_t> value = a->read_fn ? a->read_fn() : a->value;
      std::vector<uint8_t> p{kReadResponse};
      size_t room = mtu_ > 1 ? static_cast<size_t>(mtu_) - 1 : 0;
      size_t n = std::min(room, value.size());
      p.insert(p.end(), value.begin(), value.begin() + n);
      return p;
    }

    case kReadBlobRequest: {
      if (pdu.size() < 5) return error(op, 0, kInvalidPdu);
      uint16_t handle = get16(pdu.data() + 1);
      uint16_t offset = get16(pdu.data() + 3);
      const Attribute* a = find(handle);
      if (!a) return error(op, handle, kInvalidHandle);
      if (!a->readable) return error(op, handle, kReadNotPermitted);
      std::vector<uint8_t> value = a->read_fn ? a->read_fn() : a->value;
      if (offset > value.size()) return error(op, handle, kInvalidOffset);
      std::vector<uint8_t> p{kReadBlobResponse};
      size_t room = mtu_ > 1 ? static_cast<size_t>(mtu_) - 1 : 0;
      size_t n = std::min(room, value.size() - offset);
      p.insert(p.end(), value.begin() + offset, value.begin() + offset + n);
      return p;
    }

    case kWriteRequest:
    case kWriteCommand: {
      if (pdu.size() < 3) {
        return op == kWriteCommand ? std::vector<uint8_t>()
                                   : error(op, 0, kInvalidPdu);
      }
      uint16_t handle = get16(pdu.data() + 1);
      std::vector<uint8_t> value(pdu.begin() + 3, pdu.end());
      Attribute* target = nullptr;
      for (Attribute& a : attrs_) {
        if (a.handle == handle) {
          target = &a;
          break;
        }
      }
      // A Write Command takes no response at all, including no error -- so a
      // write to a bad handle is silently dropped, which is the specification
      // and also the reason a mis-numbered handle is so hard to notice.
      if (!target) {
        return op == kWriteCommand ? std::vector<uint8_t>()
                                   : error(op, handle, kInvalidHandle);
      }
      if (!target->writable) {
        return op == kWriteCommand ? std::vector<uint8_t>()
                                   : error(op, handle, kWriteNotPermitted);
      }
      target->value = value;
      if (on_write_) on_write_(handle, value);
      return op == kWriteCommand ? std::vector<uint8_t>()
                                 : std::vector<uint8_t>{kWriteResponse};
    }

    case kHandleValueConfirmation:
      return {};  // nothing to say back

    default:
      // Everything else -- prepared writes, read multiple, signed writes --
      // is answered honestly rather than ignored. A peer that gets silence
      // stalls for its full ATT timeout, thirty seconds, and then gives up
      // without saying why.
      return error(op, 0, kRequestNotSupported);
  }
}

}  // namespace att
}  // namespace octo
