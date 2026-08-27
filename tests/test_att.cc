// Drive a whole GATT server through a real discovery sequence, with no radio.
//
// The table built here is the Zoom BTA-1 profile from doc/zoom-bta1-notes.md:
// one custom service, three characteristics, the properties confirmed against
// the hardware. Running a client over it end to end is the closest thing to a
// rehearsal available before the dongle arrives -- and if the F6 later refuses
// to talk to us, this file is what rules out the attribute table as the cause.
//
// The client and server halves are both ours, which would be circular if the
// expectations were only "what we produce". They are not: the byte layouts
// below are the ones in the specification, spelled out literally, so a change
// that breaks interoperability breaks this test too.
#include "../src/att.h"
#include "harness.h"

#include <string>
#include <vector>

using namespace octo;
using namespace octo::att;

namespace {

std::vector<uint8_t> from_hex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return out;
}

// The profile, exactly as read out of the adapter.
const char kZoomService[] = "5e981594-cd7d-4201-86b9-560cf375abae";
const char kZoomTx[] = "4076b47f-130b-407c-a2f4-d52945cb84b5";  // server -> client
const char kZoomRx[] = "cbeb8809-028a-4195-b4a5-762ff6e500a9";  // client -> server
const char kZoomFlow[] = "f0262f5f-3eba-4719-a265-31f126a9c66c";

struct Profile {
  Server server;
  uint16_t tx = 0;
  uint16_t rx = 0;
  uint16_t flow = 0;
};

Profile build_zoom_profile() {
  ServerBuilder b;
  b.add_primary_service(hci::uuid_const(kZoomService));
  uint16_t tx = b.add_characteristic(hci::uuid_const(kZoomTx),
                                     kPropNotify | kPropRead, {},
                                     "Server TX Data");
  uint16_t rx = b.add_characteristic(hci::uuid_const(kZoomRx),
                                     kPropWrite | kPropWriteWithoutResponse, {},
                                     "Server RX Data");
  // Flow Control reads back 0x01 on the real adapter.
  uint16_t flow = b.add_characteristic(
      hci::uuid_const(kZoomFlow),
      kPropNotify | kPropWrite | kPropWriteWithoutResponse | kPropRead, {0x01},
      "Flow Control");

  b.add_primary_service(hci::uuid_from_16(kUuidDeviceInformation));
  b.add_characteristic(hci::uuid_from_16(kUuidManufacturerName), kPropRead,
                       {'Z', 'O', 'O', 'M'});
  b.add_characteristic(hci::uuid_from_16(kUuidModelNumber), kPropRead,
                       {'F', '6'});

  Profile p{Server(b.take()), tx, rx, flow};
  return p;
}

void test_l2cap_framing() {
  std::vector<uint8_t> frame = build_l2cap(kCidAtt, from_hex("0a0300"));
  CHECK_STR(hci::to_hex(frame), "0300" "0400" "0a0300");

  uint16_t cid = 0;
  std::vector<uint8_t> payload;
  CHECK(parse_l2cap(frame.data(), frame.size(), &cid, &payload));
  CHECK_EQ(cid, static_cast<uint16_t>(kCidAtt));
  CHECK_STR(hci::to_hex(payload), "0a0300");

  // A header promising more than it carries is refused, not padded.
  CHECK(!parse_l2cap(frame.data(), 5, &cid, &payload));
}

void test_reassembly() {
  // A 40-byte ATT payload does not fit the 27-byte default ACL payload, so it
  // arrives in two fragments. This is the ordinary case on LE, not an edge
  // case: any service discovery response is longer than 27 bytes.
  std::vector<uint8_t> big(40, 0xab);
  big[0] = kReadResponse;
  std::vector<uint8_t> frame = build_l2cap(kCidAtt, big);
  std::vector<std::vector<uint8_t>> frags = fragment(frame, 27);
  CHECK_EQ(frags.size(), static_cast<size_t>(2));
  CHECK_EQ(frags[0].size(), static_cast<size_t>(27));
  CHECK_EQ(frags[1].size(), static_cast<size_t>(17));

  Reassembler r;
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> done;
  CHECK(r.push(hci::kAclFirstFlushable, frags[0].data(), frags[0].size(), &done));
  CHECK_EQ(done.size(), static_cast<size_t>(0));  // not yet complete
  CHECK(r.in_progress());
  CHECK(r.push(hci::kAclContinuing, frags[1].data(), frags[1].size(), &done));
  CHECK_EQ(done.size(), static_cast<size_t>(1));
  CHECK_EQ(done[0].first, static_cast<uint16_t>(kCidAtt));
  CHECK(done[0].second == big);

  // A continuation arriving with nothing to continue must not be read as a
  // header. Inventing a length there desynchronises everything after it.
  Reassembler orphan;
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> none;
  CHECK(orphan.push(hci::kAclContinuing, frags[1].data(), frags[1].size(),
                    &none));
  CHECK_EQ(none.size(), static_cast<size_t>(0));

  // A new first fragment abandons a half-assembled frame rather than splicing
  // the two together into one plausible-looking wrong PDU.
  Reassembler restart;
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> out2;
  restart.push(hci::kAclFirstFlushable, frags[0].data(), frags[0].size(), &out2);
  std::vector<uint8_t> small = build_l2cap(kCidAtt, from_hex("13"));
  restart.push(hci::kAclFirstFlushable, small.data(), small.size(), &out2);
  CHECK_EQ(out2.size(), static_cast<size_t>(1));
  CHECK_STR(hci::to_hex(out2[0].second), "13");

  // Two whole frames in one fragment both come out.
  Reassembler pair;
  std::vector<uint8_t> both = build_l2cap(kCidAtt, from_hex("13"));
  std::vector<uint8_t> second = build_l2cap(kCidAtt, from_hex("1e"));
  both.insert(both.end(), second.begin(), second.end());
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> out3;
  CHECK(pair.push(hci::kAclFirstFlushable, both.data(), both.size(), &out3));
  CHECK_EQ(out3.size(), static_cast<size_t>(2));
}

void test_request_encoding() {
  CHECK_STR(hci::to_hex(exchange_mtu_request(247)), "02" "f700");
  CHECK_STR(hci::to_hex(read_request(0x0003)), "0a" "0300");
  CHECK_STR(hci::to_hex(find_information_request(1, 0xffff)),
            "04" "0100" "ffff");
  CHECK_STR(hci::to_hex(read_by_group_type_request(
                1, 0xffff, hci::uuid_from_16(kUuidPrimaryService))),
            "10" "0100" "ffff" "0028");
  CHECK_STR(hci::to_hex(write_request(0x0005, from_hex("0100"))),
            "12" "0500" "0100");
  CHECK_STR(hci::to_hex(write_command(0x0005, from_hex("ff"))),
            "52" "0500" "ff");

  // A 128-bit type goes on the wire reversed, like every other UUID.
  std::vector<uint8_t> req =
      read_by_type_request(1, 0xffff, hci::uuid_const(kZoomTx));
  CHECK_STR(hci::to_hex(req),
            "08" "0100" "ffff" "b584cb4529d5f4a27c400b137fb47640");
}

void test_response_parsing() {
  ErrorResponse err;
  CHECK(parse_error_response(from_hex("01" "0a" "0300" "02"), &err));
  CHECK_EQ(err.request_opcode, static_cast<uint8_t>(kReadRequest));
  CHECK_EQ(err.handle, static_cast<uint16_t>(3));
  CHECK_EQ(err.error, static_cast<uint8_t>(kReadNotPermitted));
  CHECK_STR(error_name(err.error), "read not permitted");

  uint16_t mtu = 0;
  CHECK(parse_exchange_mtu_response(from_hex("03" "f700"), &mtu));
  CHECK_EQ(mtu, static_cast<uint16_t>(247));

  // A characteristic declaration: properties, value handle, then the UUID.
  std::vector<CharDecl> chars;
  CHECK(parse_read_by_type_response_chars(
      from_hex("09" "07" "0200" "12" "0300" "0a2a" "0500" "02" "0600" "292a"),
      &chars));
  CHECK_EQ(chars.size(), static_cast<size_t>(2));
  CHECK_EQ(chars[0].value_handle, static_cast<uint16_t>(3));
  CHECK_EQ(chars[0].properties, static_cast<uint8_t>(0x12));
  CHECK_STR(properties_to_string(chars[0].properties), "read,notify");
  CHECK_EQ(chars[1].value_handle, static_cast<uint16_t>(6));

  Notification n;
  CHECK(parse_notification(from_hex("1b" "0300" "deadbeef"), &n));
  CHECK_EQ(n.handle, static_cast<uint16_t>(3));
  CHECK_STR(hci::to_hex(n.value), "deadbeef");
  CHECK(!n.wants_confirmation);
  CHECK(parse_notification(from_hex("1d" "0300" "01"), &n));
  CHECK(n.wants_confirmation);

  // An entry width the parser does not recognise is refused rather than
  // decoded into services that were never advertised.
  std::vector<ServiceRange> svcs;
  CHECK(!parse_read_by_group_type_response(from_hex("11" "05" "0100" "0500" "00"),
                                           &svcs));
}

// Walk the server the way a real client does, and check that what comes back
// is the profile that went in.
void test_server_discovery() {
  Profile p = build_zoom_profile();

  // 1. Negotiate an MTU. Without this every response is capped at 23 bytes and
  //    a 128-bit UUID barely fits one entry.
  std::vector<uint8_t> rsp = p.server.handle_request(exchange_mtu_request(247));
  uint16_t mtu = 0;
  CHECK(parse_exchange_mtu_response(rsp, &mtu));
  CHECK_EQ(p.server.mtu(), static_cast<uint16_t>(247));

  // 2. Enumerate the primary services.
  rsp = p.server.handle_request(read_by_group_type_request(
      1, 0xffff, hci::uuid_from_16(kUuidPrimaryService)));
  std::vector<ServiceRange> svcs;
  CHECK(parse_read_by_group_type_response(rsp, &svcs));
  CHECK_EQ(svcs.size(), static_cast<size_t>(1));  // one width per response
  CHECK_STR(hci::uuid_to_string(svcs[0].uuid), kZoomService);
  CHECK_EQ(svcs[0].start, static_cast<uint16_t>(1));

  // The custom service's group must cover all three characteristics and stop
  // before Device Information starts. Getting this end handle wrong is how a
  // client ends up discovering two services' worth of characteristics under
  // one of them.
  CHECK(svcs[0].end >= p.flow);
  CHECK(svcs[0].end < svcs[0].start + 14);

  // The 16-bit Device Information service comes back on the next request,
  // because one response carries one UUID width.
  rsp = p.server.handle_request(read_by_group_type_request(
      static_cast<uint16_t>(svcs[0].end + 1), 0xffff,
      hci::uuid_from_16(kUuidPrimaryService)));
  std::vector<ServiceRange> more;
  CHECK(parse_read_by_group_type_response(rsp, &more));
  CHECK_EQ(more.size(), static_cast<size_t>(1));
  CHECK_STR(hci::uuid_to_string(more[0].uuid), "180A");

  // 3. Enumerate the characteristics of the custom service.
  rsp = p.server.handle_request(read_by_type_request(
      svcs[0].start, svcs[0].end, hci::uuid_from_16(kUuidCharacteristic)));
  std::vector<CharDecl> chars;
  CHECK(parse_read_by_type_response_chars(rsp, &chars));
  CHECK_EQ(chars.size(), static_cast<size_t>(3));
  CHECK_STR(hci::uuid_to_string(chars[0].uuid), kZoomTx);
  CHECK_EQ(chars[0].value_handle, p.tx);
  CHECK_STR(properties_to_string(chars[0].properties), "read,notify");
  CHECK_STR(hci::uuid_to_string(chars[1].uuid), kZoomRx);
  CHECK_EQ(chars[1].value_handle, p.rx);
  CHECK_STR(properties_to_string(chars[1].properties), "write-nr,write");
  CHECK_STR(hci::uuid_to_string(chars[2].uuid), kZoomFlow);
  CHECK_EQ(chars[2].value_handle, p.flow);

  // 4. Read Flow Control, which the real adapter answers with 0x01.
  rsp = p.server.handle_request(read_request(p.flow));
  std::vector<uint8_t> value;
  CHECK(parse_read_response(rsp, &value));
  CHECK_STR(hci::to_hex(value), "01");

  // 5. Find the descriptors, which is how a client locates a CCCD.
  //
  // One response carries one UUID width, so a client asks again from just past
  // the last handle it got until the range is covered. Doing this in a single
  // request is the mistake that makes a CCCD invisible: the characteristic
  // value is a 128-bit type, the descriptors after it are 16-bit, and the
  // response stops at the change.
  uint16_t cccd_handle = 0;
  uint16_t cursor = p.tx;
  const uint16_t last = static_cast<uint16_t>(p.rx - 1);
  int rounds = 0;
  while (cursor <= last && rounds++ < 8) {
    rsp = p.server.handle_request(find_information_request(cursor, last));
    std::vector<HandleUuid> descs;
    if (!parse_find_information_response(rsp, &descs) || descs.empty()) break;
    for (const HandleUuid& d : descs) {
      if (d.uuid == hci::uuid_from_16(kUuidClientCharConfig)) {
        cccd_handle = d.handle;
      }
      cursor = static_cast<uint16_t>(d.handle + 1);
    }
  }
  CHECK(cccd_handle != 0);

  // 6. Subscribe, and only then does a notification have anywhere to go.
  CHECK(!p.server.subscribed(p.tx));
  CHECK(p.server.notification(p.tx, from_hex("cafe")).empty());
  rsp = p.server.handle_request(write_request(cccd_handle, from_hex("0100")));
  CHECK_EQ(rsp.size(), static_cast<size_t>(1));
  CHECK_EQ(rsp[0], static_cast<uint8_t>(kWriteResponse));
  CHECK(p.server.subscribed(p.tx));
  CHECK_STR(hci::to_hex(p.server.notification(p.tx, from_hex("cafe"))),
            "1b" "0300" "cafe");

  // Subscribing to one characteristic does not subscribe the others.
  CHECK(!p.server.subscribed(p.flow));
}

void test_server_writes_and_errors() {
  Profile p = build_zoom_profile();
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> writes;
  p.server.set_write_handler(
      [&writes](uint16_t h, const std::vector<uint8_t>& v) {
        writes.emplace_back(h, v);
      });

  // A write without response gets no response, and is still reported. This is
  // the whole Zoom experiment: whatever the F6 sends, we see.
  std::vector<uint8_t> rsp =
      p.server.handle_request(write_command(p.rx, from_hex("deadbeef")));
  CHECK(rsp.empty());
  CHECK_EQ(writes.size(), static_cast<size_t>(1));
  CHECK_EQ(writes[0].first, p.rx);
  CHECK_STR(hci::to_hex(writes[0].second), "deadbeef");

  // Writing to a read-only characteristic is refused with the right error...
  rsp = p.server.handle_request(write_request(p.tx, from_hex("00")));
  ErrorResponse err;
  CHECK(parse_error_response(rsp, &err));
  CHECK_EQ(err.error, static_cast<uint8_t>(kWriteNotPermitted));

  // ...but the same write as a command is silently dropped, because a Write
  // Command has no response of any kind, errors included.
  rsp = p.server.handle_request(write_command(p.tx, from_hex("00")));
  CHECK(rsp.empty());

  // A handle that does not exist.
  rsp = p.server.handle_request(read_request(0x7fff));
  CHECK(parse_error_response(rsp, &err));
  CHECK_EQ(err.error, static_cast<uint8_t>(kInvalidHandle));

  // A group type that is not a service. Answering "none found" here would
  // read as "this device has no services".
  rsp = p.server.handle_request(
      read_by_group_type_request(1, 0xffff, hci::uuid_from_16(0x2803)));
  CHECK(parse_error_response(rsp, &err));
  CHECK_EQ(err.error, static_cast<uint8_t>(kUnsupportedGroupType));

  // An unimplemented request is answered rather than ignored: silence costs
  // the peer its full thirty-second ATT timeout.
  rsp = p.server.handle_request(from_hex("16" "0300" "0000" "ff"));
  CHECK(parse_error_response(rsp, &err));
  CHECK_EQ(err.error, static_cast<uint8_t>(kRequestNotSupported));

  // A truncated request does not read past the end of the buffer.
  rsp = p.server.handle_request(from_hex("0a"));
  CHECK(parse_error_response(rsp, &err));
  CHECK_EQ(err.error, static_cast<uint8_t>(kInvalidPdu));
  CHECK(p.server.handle_request(std::vector<uint8_t>()).empty());
}

void test_dynamic_characteristic() {
  // Anything live has to be generated at read time. A timecode read must be
  // the time now, not the time the table was built -- which is the entire
  // point of hosting a clock rather than a value.
  int reads = 0;
  ServerBuilder b;
  b.add_primary_service(hci::uuid_const(kZoomService));
  uint16_t h = b.add_dynamic_characteristic(
      hci::uuid_const(kZoomTx), kPropRead | kPropNotify,
      [&reads]() -> std::vector<uint8_t> {
        ++reads;
        return {static_cast<uint8_t>(reads)};
      });
  Server s(b.take());

  std::vector<uint8_t> value;
  CHECK(parse_read_response(s.handle_request(read_request(h)), &value));
  CHECK_STR(hci::to_hex(value), "01");
  CHECK(parse_read_response(s.handle_request(read_request(h)), &value));
  CHECK_STR(hci::to_hex(value), "02");
  CHECK_EQ(reads, 2);
}

void test_mtu_truncation() {
  // Before an MTU exchange the limit is 23 bytes, so a read response carries
  // at most 22. A server that ignores this overruns the peer's buffer.
  ServerBuilder b;
  b.add_primary_service(hci::uuid_const(kZoomService));
  uint16_t h = b.add_characteristic(hci::uuid_const(kZoomTx), kPropRead,
                                    std::vector<uint8_t>(64, 0x5a));
  Server s(b.take());
  CHECK_EQ(s.mtu(), kDefaultMtu);

  std::vector<uint8_t> value;
  CHECK(parse_read_response(s.handle_request(read_request(h)), &value));
  CHECK_EQ(value.size(), static_cast<size_t>(22));

  // The rest is fetched with Read Blob, which is what a long characteristic
  // is for.
  std::vector<uint8_t> rest;
  CHECK(parse_read_response(s.handle_request(read_blob_request(h, 22)), &rest));
  CHECK_EQ(rest.size(), static_cast<size_t>(22));

  // An offset past the end is an error, not an empty read.
  ErrorResponse err;
  CHECK(parse_error_response(s.handle_request(read_blob_request(h, 200)), &err));
  CHECK_EQ(err.error, static_cast<uint8_t>(kInvalidOffset));
}

}  // namespace

int main() {
  test_l2cap_framing();
  test_reassembly();
  test_request_encoding();
  test_response_parsing();
  test_server_discovery();
  test_server_writes_and_errors();
  test_dynamic_characteristic();
  test_mtu_truncation();
  return octotest::report("test_att");
}
