// Pin the HCI codec to the specification, and to the captures this project
// already has.
//
// The dongle is not here yet, and that is exactly the argument for this file:
// every byte the host will put on the air is decided by portable code, so the
// question "did we build the advertisement correctly" can be settled on a
// machine with no radio in it. The week documented in doc/zoom-bta1-notes.md
// was spent unable to ask that question at all, because CoreBluetooth would
// not say what it had transmitted.
//
// The Zoom expectations below are not invented: they are the profile read out
// of Zoom's own F6SYSTEM.BIN and then confirmed against the adapter over the
// air, including the advertising template found verbatim in the firmware.
#include "../src/hci.h"
#include "harness.h"

#include <string>
#include <vector>

using namespace octo::hci;

namespace {

std::vector<uint8_t> from_hex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return out;
}

void test_uuids() {
  // A short UUID expands against the base and comes back short.
  Uuid fdac = uuid_from_16(0xfdac);
  CHECK(fdac.is_16bit());
  CHECK_EQ(fdac.short16(), 0xfdac);
  CHECK_STR(uuid_to_string(fdac), "FDAC");

  Uuid parsed;
  CHECK(uuid_from_string("FDAC", &parsed));
  CHECK(parsed == fdac);
  CHECK(uuid_from_string("0000fdac-0000-1000-8000-00805f9b34fb", &parsed));
  CHECK(parsed == fdac);

  // The Zoom BTA-1 service, as written down.
  const std::string kZoom = "5e981594-cd7d-4201-86b9-560cf375abae";
  Uuid zoom;
  CHECK(uuid_from_string(kZoom, &zoom));
  CHECK(!zoom.is_16bit());
  CHECK_STR(uuid_to_string(zoom), kZoom);

  // ...and as it appears in the firmware's advertising template, which is
  // byte-reversed. Getting this backwards is the single most effective way to
  // spend an evening hunting a device that is sitting right there.
  CHECK_STR(to_hex(uuid_to_le(zoom)), "aeab75f30c56b98601427dcd9415985e");

  Uuid back;
  std::vector<uint8_t> le = uuid_to_le(zoom);
  CHECK(uuid_from_le(le.data(), le.size(), &back));
  CHECK(back == zoom);

  // Dashes are optional, case is not significant, and nonsense is refused
  // rather than silently producing a UUID nothing will ever match.
  CHECK(uuid_from_string("5E981594CD7D420186B9560CF375ABAE", &parsed));
  CHECK(parsed == zoom);
  CHECK(!uuid_from_string("not-a-uuid", &parsed));
  CHECK(uuid_from_string("5e98", &parsed));   // 4 hex digits: a 16-bit UUID
  CHECK(!uuid_from_string("5e9815", &parsed));         // 6: neither shape
}

void test_addresses() {
  Address a;
  CHECK(address_from_string("C0:1A:2B:3C:4D:5E", &a));
  CHECK_STR(address_to_string(a), "C0:1A:2B:3C:4D:5E");
  CHECK_EQ(a.bytes[0], 0xc0);
  CHECK_EQ(a.bytes[5], 0x5e);

  CHECK(!address_from_string("C0:1A:2B:3C:4D", &a));
  CHECK(!address_from_string("C0:1A:2B:3C:4D:5E:6F", &a));
  CHECK(!address_from_string("ZZ:1A:2B:3C:4D:5E", &a));

  // The top two bits of the most significant byte say what kind of random
  // address this is. A resolvable private address rotates, which is what made
  // the on/off device-set diff in doc/zoom-bta1-notes.md prove nothing.
  Address rpa;
  address_from_string("40:11:22:33:44:55", &rpa);
  rpa.type = kAddrRandom;
  CHECK(random_kind(rpa) == RandomKind::kResolvablePrivate);
  CHECK(!address_is_stable(rpa));

  Address stat;
  address_from_string("C0:11:22:33:44:55", &stat);
  stat.type = kAddrRandom;
  CHECK(random_kind(stat) == RandomKind::kStatic);
  CHECK(address_is_stable(stat));

  Address pub;
  address_from_string("40:11:22:33:44:55", &pub);
  pub.type = kAddrPublic;
  CHECK(random_kind(pub) == RandomKind::kNotRandom);
  CHECK(address_is_stable(pub));
}

void test_command_framing() {
  // Reset is the canonical smallest command: 01 03 0c 00.
  CHECK_STR(to_hex(build_command(kOpReset)), "01030c00");

  std::vector<uint8_t> enable = build_command(kOpLeSetScanEnable,
                                              le_set_scan_enable(true, false));
  CHECK_STR(to_hex(enable), "010c20020100");

  // An address goes onto the wire least significant byte first.
  Address peer;
  address_from_string("C0:1A:2B:3C:4D:5E", &peer);
  peer.type = kAddrRandom;
  std::vector<uint8_t> params = le_create_connection(
      peer, ms_to_scan_units(60), ms_to_scan_units(30), ms_to_conn_units(30),
      ms_to_conn_units(50), 0, ms_to_supervision_units(4000));
  // scan interval/window, filter policy, peer type, then the reversed address.
  CHECK_STR(to_hex(params).substr(0, 8), "60003000");
  CHECK_STR(to_hex(params).substr(8, 16), "0001" "5e4d3c2b1ac0");
}

void test_unit_conversion() {
  // 0.625 ms units for advertising, with the specification's 20 ms floor for
  // the connectable case enforced rather than passed through to a bare
  // "invalid parameters" from the controller.
  CHECK_EQ(ms_to_adv_units(100.0), 160);
  CHECK_EQ(ms_to_adv_units(20.0), 32);
  CHECK_EQ(ms_to_adv_units(1.0), 32);        // clamped up to the floor
  CHECK_EQ(ms_to_adv_units(0.0), 32);
  CHECK_EQ(ms_to_adv_units(1e9), 0x4000);    // clamped down to the ceiling

  CHECK_EQ(ms_to_conn_units(30.0), 24);      // 1.25 ms units
  CHECK_EQ(ms_to_conn_units(7.5), 6);
  CHECK_EQ(ms_to_conn_units(1.0), 6);
  CHECK_EQ(ms_to_supervision_units(4000.0), 400);  // 10 ms units
}

void test_adv_data_building() {
  // The advertisement macOS could not be persuaded to send: flags plus the
  // full 128-bit Zoom service UUID, and nothing else competing for the space.
  Uuid zoom = uuid_const("5e981594-cd7d-4201-86b9-560cf375abae");
  std::vector<uint8_t> ad;
  CHECK(append_ad_flags(&ad, kFlagGeneralDiscoverable | kFlagBrEdrNotSupported));
  CHECK(append_ad_service(&ad, zoom));
  CHECK_EQ(ad.size(), static_cast<size_t>(21));  // 3 flags + 18 UUID
  // The UUID structure is exactly the template found in Zoom's firmware:
  // length 0x11, type 0x07, then the UUID little-endian.
  CHECK_STR(to_hex(ad), "020106" "1107aeab75f30c56b98601427dcd9415985e");

  // Now the case that defeated CoreBluetooth. "UltraSync BLUE" is 14 bytes,
  // so its structure costs 16, and 21 + 16 = 37 > 31. Here that is a refusal
  // with the UUID still in place, rather than a silent demotion of the UUID
  // into Apple's overflow area where a DA14580 cannot see it.
  std::vector<uint8_t> before = ad;
  CHECK(!append_ad_name(&ad, "UltraSync BLUE"));
  CHECK(ad == before);  // the failed append changed nothing

  // A short name does fit, and the budget is the real 31 bytes.
  CHECK(append_ad_name(&ad, "US"));
  CHECK_EQ(ad.size(), static_cast<size_t>(25));

  // Service data, which is how a Tentacle publishes its clock.
  std::vector<uint8_t> sd;
  CHECK(append_ad_service_data(&sd, uuid_from_16(0xfdac),
                               from_hex("223d18110b2c0000")));
  CHECK_STR(to_hex(sd), "0b16" "acfd" "223d18110b2c0000");
}

void test_adv_data_parsing() {
  // A real Tentacle advertisement: flags, then FDAC service data carrying a
  // type-0x22 timecode payload. tentacle.h decodes the payload; this checks
  // only that the right bytes are handed to it.
  std::vector<uint8_t> raw =
      from_hex("020106" "0b16acfd223d18110b2c0000" "06094653352d41");
  AdInfo info = summarise_ad(parse_ad(raw));
  CHECK(info.has_flags);
  CHECK_EQ(info.flags, 0x06);
  CHECK_STR(info.name, "FS5-A");
  CHECK_EQ(info.service_data.size(), static_cast<size_t>(1));
  CHECK(info.service_data[0].first == uuid_from_16(0xfdac));
  CHECK_STR(to_hex(info.service_data[0].second), "223d18110b2c0000");

  // A 128-bit service list, the shape the Zoom adapter would answer to.
  std::vector<uint8_t> zoomad =
      from_hex("020106" "1107aeab75f30c56b98601427dcd9415985e");
  AdInfo zi = summarise_ad(parse_ad(zoomad));
  CHECK_EQ(zi.services.size(), static_cast<size_t>(1));
  CHECK_STR(uuid_to_string(zi.services[0]),
            "5e981594-cd7d-4201-86b9-560cf375abae");

  // Truncation stops the walk instead of reading past the end; a zero length
  // is the padding it is, not a structure.
  CHECK_EQ(parse_ad(from_hex("020106" "0916414243")).size(),
           static_cast<size_t>(1));
  CHECK_EQ(parse_ad(from_hex("020106" "000000")).size(), static_cast<size_t>(1));
  CHECK_EQ(parse_ad(std::vector<uint8_t>()).size(), static_cast<size_t>(0));

  // Round trip: what we build is what we parse.
  std::vector<uint8_t> built;
  append_ad_flags(&built, 0x06);
  append_ad_name(&built, "octomancer");
  AdInfo bi = summarise_ad(parse_ad(built));
  CHECK_STR(bi.name, "octomancer");
}

void test_stream_framing() {
  // Two events back to back, then half of a third. The parser must consume
  // exactly the whole ones and leave the fragment for the next read, because
  // a USB read boundary lands wherever it likes.
  std::vector<uint8_t> stream = from_hex(
      "040e0401030c00"      // Command Complete for Reset
      "040e0401052000"      // Command Complete for LE Set Random Address
      "040e04010b20");      // truncated: one byte short
  std::vector<Packet> pkts;
  size_t used = parse_stream(stream.data(), stream.size(), &pkts);
  CHECK_EQ(pkts.size(), static_cast<size_t>(2));
  CHECK_EQ(used, static_cast<size_t>(14));
  CHECK_EQ(stream.size() - used, static_cast<size_t>(6));

  Event evt;
  CHECK(parse_event(pkts[0].payload, &evt));
  CHECK_EQ(evt.code, kEvtCommandComplete);
  CommandComplete cc;
  CHECK(parse_command_complete(evt, &cc));
  CHECK_EQ(cc.opcode, static_cast<uint16_t>(kOpReset));
  CHECK_EQ(cc.status, static_cast<uint8_t>(kSuccess));

  // A desynchronised stream resynchronises by dropping one byte at a time
  // rather than discarding the buffer, so a real packet behind the garbage
  // still arrives.
  std::vector<uint8_t> noisy = from_hex("ff" "040e0401030c00");
  std::vector<Packet> np;
  parse_stream(noisy.data(), noisy.size(), &np);
  CHECK_EQ(np.size(), static_cast<size_t>(1));
}

void test_event_parsing() {
  // An LE Advertising Report carrying one report.
  std::vector<uint8_t> payload = from_hex(
      "3e" "1b"             // LE Meta, 27 bytes
      "02"                  // subevent: advertising report
      "01"                  // one report
      "00"                  // event type: ADV_IND
      "01"                  // address type: random
      "5e4d3c2b1ac0"        // address, little-endian
      "0f"                  // 15 bytes of data
      "020106" "0b16acfd223d18110b2c0000"
      "c4");                // RSSI -60
  Event evt;
  CHECK(parse_event(payload, &evt));
  CHECK_EQ(evt.code, kEvtLeMeta);
  CHECK_EQ(evt.subevent, kLeAdvertisingReport);

  std::vector<AdvReport> reports;
  CHECK(parse_adv_reports(evt, &reports));
  CHECK_EQ(reports.size(), static_cast<size_t>(1));
  CHECK_STR(address_to_string(reports[0].addr), "C0:1A:2B:3C:4D:5E");
  CHECK_EQ(reports[0].addr.type, static_cast<uint8_t>(kAddrRandom));
  CHECK_EQ(reports[0].rssi, -60);
  AdInfo info = summarise_ad(parse_ad(reports[0].data));
  CHECK_EQ(info.service_data.size(), static_cast<size_t>(1));

  // LE Connection Complete, and the enhanced form that a controller with
  // privacy support sends instead. Both have to land in the same struct or a
  // connection is made and never noticed.
  Event conn;
  CHECK(parse_event(from_hex("3e" "13" "01" "00" "4000" "00" "01"
                             "5e4d3c2b1ac0" "1800" "0000" "9001" "05"),
                    &conn));
  ConnectionComplete cc;
  CHECK(parse_connection_complete(conn, &cc));
  CHECK_EQ(cc.status, static_cast<uint8_t>(kSuccess));
  CHECK_EQ(cc.handle, static_cast<uint16_t>(0x0040));
  CHECK_EQ(cc.role, static_cast<uint8_t>(0));
  CHECK_STR(address_to_string(cc.peer), "C0:1A:2B:3C:4D:5E");
  CHECK_EQ(cc.interval, static_cast<uint16_t>(0x0018));

  Event econn;
  CHECK(parse_event(from_hex("3e" "1f" "0a" "00" "4100" "01" "01"
                             "5e4d3c2b1ac0"
                             "000000000000"   // local resolvable private addr
                             "000000000000"   // peer resolvable private addr
                             "2400" "0000" "9001" "05"),
                    &econn));
  ConnectionComplete ecc;
  CHECK(parse_connection_complete(econn, &ecc));
  CHECK_EQ(ecc.handle, static_cast<uint16_t>(0x0041));
  CHECK_EQ(ecc.role, static_cast<uint8_t>(1));
  CHECK_STR(address_to_string(ecc.peer), "C0:1A:2B:3C:4D:5E");
  CHECK_EQ(ecc.interval, static_cast<uint16_t>(0x0024));

  Event disc;
  CHECK(parse_event(from_hex("05" "04" "00" "4000" "13"), &disc));
  DisconnectionComplete dc;
  CHECK(parse_disconnection_complete(disc, &dc));
  CHECK_EQ(dc.handle, static_cast<uint16_t>(0x0040));
  CHECK_EQ(dc.reason, static_cast<uint8_t>(kRemoteUserTerminated));

  Event enc;
  CHECK(parse_event(from_hex("08" "04" "00" "4000" "01"), &enc));
  EncryptionChange ec;
  CHECK(parse_encryption_change(enc, &ec));
  CHECK_EQ(ec.enabled, static_cast<uint8_t>(1));

  // Command Status, which is what a command that takes time answers with.
  Event st;
  CHECK(parse_event(from_hex("0f" "04" "00" "01" "0d20"), &st));
  CommandStatus cs;
  CHECK(parse_command_status(st, &cs));
  CHECK_EQ(cs.status, static_cast<uint8_t>(kSuccess));
  CHECK_EQ(cs.opcode, static_cast<uint16_t>(kOpLeCreateConnection));

  // Short and malformed events are refused rather than read past the end.
  Event bad;
  CHECK(!parse_event(from_hex("0e"), &bad));
  CHECK(!parse_event(from_hex("0e10" "00"), &bad));  // length exceeds the buffer
  std::vector<AdvReport> none;
  Event notreport;
  parse_event(from_hex("3e" "02" "02" "01"), &notreport);
  CHECK(!parse_adv_reports(notreport, &none));  // claims a report, has no body
}

void test_acl_framing() {
  std::vector<uint8_t> body = from_hex("0400010004000102");
  std::vector<uint8_t> pkt =
      build_acl(0x0040, kAclFirstFlushable, body.data(), body.size());
  CHECK_EQ(pkt[0], static_cast<uint8_t>(kPacketAclData));
  CHECK_STR(to_hex(pkt), "02" "4020" "0800" "0400010004000102");

  AclHeader hdr;
  std::vector<uint8_t> data;
  CHECK(parse_acl(std::vector<uint8_t>(pkt.begin() + 1, pkt.end()), &hdr,
                  &data));
  CHECK_EQ(hdr.handle, static_cast<uint16_t>(0x0040));
  CHECK_EQ(hdr.pb_flag, static_cast<uint8_t>(kAclFirstFlushable));
  CHECK(data == body);

  // A header that promises more than it delivers is refused.
  AclHeader h2;
  std::vector<uint8_t> d2;
  CHECK(!parse_acl(from_hex("4020" "0800" "0400"), &h2, &d2));
}

void test_describe() {
  std::vector<std::string> lines = describe_ad(
      from_hex("020106" "0b16acfd223d18110b2c0000" "06094653352d41"));
  CHECK_EQ(lines.size(), static_cast<size_t>(3));
  CHECK(lines[0].find("le-only") != std::string::npos);
  CHECK(lines[1].find("FDAC") != std::string::npos);
  CHECK(lines[1].find("223d18110b2c0000") != std::string::npos);
  CHECK_STR(lines[2], "name FS5-A");
}

}  // namespace

int main() {
  test_uuids();
  test_addresses();
  test_command_framing();
  test_unit_conversion();
  test_adv_data_building();
  test_adv_data_parsing();
  test_stream_framing();
  test_event_parsing();
  test_acl_framing();
  test_describe();
  return octotest::report("test_hci");
}
