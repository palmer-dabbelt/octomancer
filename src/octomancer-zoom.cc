// octomancer-zoom -- the Zoom BTA-1 bench.
//
// doc/zoom-bta1-notes.md ends with an experiment that could not be run. In
// timecode mode the F6 scans and connects outward, so anything that wants to
// feed it timecode has to be a peripheral advertising on the F6's terms --
// and CoreBluetooth will not do that. It refuses to emit manufacturer data,
// gives no control over the scan response, and relegates 128-bit service
// UUIDs to an Apple-only overflow area a DA14580 cannot see. Eight variants
// were tried and the F6 answered none of them, with no way to find out what
// had actually gone out on the air.
//
// This is that experiment, on a radio that does as it is told. Every byte of
// the advertisement is specified here and can be printed back; --trace shows
// the HCI packets themselves.
//
// The modes, in the order they are useful:
//
//   --scan     what is on the air, decoded. The Zoom will not appear in
//              timecode mode -- it advertises nothing -- but this is how to
//              confirm the F6 is in control mode, and how to watch for
//              anything new when a button is pressed.
//   --dump     connect to the adapter in F6 Control mode and print its whole
//              attribute table. This is the confirmation pass that produced
//              the profile in the notes, repeatable now in one command.
//   --serve    advertise the profile and host it, logging everything the F6
//              does. The actual experiment.
//   --sweep    --serve, cycling the parts of the advertisement we are unsure
//              about, until something connects.
//
// What is still unknown is what bytes the F6 expects once it has connected.
// The GATT layout is only plumbing; the protocol carried over Server TX Data
// has never been seen. So --serve logs every write verbatim and --tx sends
// whatever is asked for: this is a bench, not a driver.
#include <getopt.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <memory>
#include <map>
#include <string>
#include <vector>

#include "att.h"
#include "hci.h"
#include "hcilink.h"
#include "timeutil.h"

namespace {

using octo::att::Server;
using octo::mono_now;
namespace att = octo::att;
namespace hci = octo::hci;

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// The profile, as read out of Zoom's firmware and confirmed against the
// adapter over the air. See doc/zoom-bta1-notes.md section 7.
const char kZoomService[] = "5e981594-cd7d-4201-86b9-560cf375abae";
const char kZoomTx[] = "4076b47f-130b-407c-a2f4-d52945cb84b5";
const char kZoomRx[] = "cbeb8809-028a-4195-b4a5-762ff6e500a9";
const char kZoomFlow[] = "f0262f5f-3eba-4719-a265-31f126a9c66c";

std::vector<uint8_t> from_hex(const std::string& s) {
  std::vector<uint8_t> out;
  std::string clean;
  for (char c : s) {
    if (c != ' ' && c != ':' && c != '-') clean.push_back(c);
  }
  for (size_t i = 0; i + 1 < clean.size(); i += 2) {
    out.push_back(
        static_cast<uint8_t>(std::stoul(clean.substr(i, 2), nullptr, 16)));
  }
  return out;
}

std::string stamp() {
  char buf[32];
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  std::strftime(buf, sizeof buf, "%H:%M:%S", &tm);
  return std::string(buf);
}

void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char* fmt, ...) {
  std::fprintf(stdout, "%s ", stamp().c_str());
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);
  std::fprintf(stdout, "\n");
  std::fflush(stdout);
}

struct Options {
  enum class Mode { kScan, kDump, kServe, kSweep } mode = Mode::kScan;
  double seconds = 30.0;
  std::string address;
  std::string device;
  bool trace = false;

  // What to advertise. Defaults reproduce the firmware's own template: flags
  // plus the full 128-bit service UUID, and no name competing for the space.
  std::string name;
  bool have_name = false;
  std::string uuid = kZoomService;
  bool advertise_uuid = true;
  std::vector<uint8_t> manufacturer;
  std::vector<uint8_t> scan_response;
  double interval_ms = 100.0;

  std::vector<uint8_t> tx;   // notify this once something subscribes
  bool tick = false;         // ...and keep notifying, once a second
};

void usage() {
  std::printf(
      "octomancer-zoom -- the Zoom BTA-1 bench\n"
      "\n"
      "Modes:\n"
      "  --scan [SECONDS]   decode everything on the air (default)\n"
      "  --dump ADDRESS     connect and print the whole attribute table\n"
      "  --serve            advertise the profile and host it\n"
      "  --sweep            --serve, cycling advertisement variants\n"
      "\n"
      "Advertisement:\n"
      "  --name NAME        local name (default: none, to leave room for the\n"
      "                     128-bit UUID -- the exact trade macOS lost)\n"
      "  --no-name          no local name at all (the default)\n"
      "  --uuid UUID        service UUID to advertise\n"
      "  --no-uuid          omit the service UUID\n"
      "  --manufacturer HEX manufacturer data, company ID first\n"
      "  --scan-response HEX  scan response payload\n"
      "  --interval MS      advertising interval (default 100)\n"
      "\n"
      "Data:\n"
      "  --tx HEX           notify this on Server TX Data once subscribed\n"
      "  --tick             keep notifying once a second\n"
      "\n"
      "Other:\n"
      "  --device PORT      the dongle's serial port\n"
      "  --trace            log every HCI packet\n"
      "  --seconds N        how long to run\n"
      "  --help\n");
}

// ---------------------------------------------------------------- scanning

int do_scan(const Options& opt) {
  hci::Link::Options lo;
  lo.device = opt.device;
  lo.trace = opt.trace;
  std::string err;
  std::unique_ptr<hci::Link> link = hci::Link::open(lo, &err);
  if (!link) {
    std::fprintf(stderr, "octomancer-zoom: %s\n", err.c_str());
    return 1;
  }
  say("radio: %s", link->describe().c_str());
  say("scanning for %.0f seconds; only changes are printed", opt.seconds);

  // Print a device when what it says changes, not on every advertisement.
  // A room produces thousands a minute and almost all of them repeat.
  std::map<std::string, std::string> last;

  if (!link->start_scan(/*active=*/true, /*filter_duplicates=*/false,
                        [&](const hci::AdvReport& r) {
                          std::string id = hci::address_to_string(r.addr);
                          std::string text = hci::to_hex(r.data);
                          auto it = last.find(id);
                          if (it != last.end() && it->second == text) return;
                          last[id] = text;

                          hci::AdInfo info =
                              hci::summarise_ad(hci::parse_ad(r.data));
                          say("%s %s rssi %d%s", id.c_str(),
                              hci::address_is_stable(r.addr) ? "       "
                                                             : "(private)",
                              r.rssi,
                              info.name.empty()
                                  ? ""
                                  : ("  \"" + info.name + "\"").c_str());
                          for (const std::string& line :
                               hci::describe_ad(r.data)) {
                            say("    %s", line.c_str());
                          }
                        },
                        &err)) {
    std::fprintf(stderr, "octomancer-zoom: %s\n", err.c_str());
    return 1;
  }

  double until = mono_now() + opt.seconds;
  while (!g_stop && mono_now() < until) {
    struct timespec ts = {0, 200 * 1000 * 1000};
    nanosleep(&ts, nullptr);
  }
  link->stop_scan(nullptr);
  say("saw %zu distinct devices", last.size());
  return 0;
}

// ------------------------------------------------------------------- dump

int do_dump(const Options& opt) {
  hci::Link::Options lo;
  lo.device = opt.device;
  lo.trace = opt.trace;
  std::string err;
  std::unique_ptr<hci::Link> link = hci::Link::open(lo, &err);
  if (!link) {
    std::fprintf(stderr, "octomancer-zoom: %s\n", err.c_str());
    return 1;
  }

  hci::Address peer;
  if (!hci::address_from_string(opt.address, &peer)) {
    std::fprintf(stderr, "octomancer-zoom: \"%s\" is not an address\n",
                 opt.address.c_str());
    return 1;
  }

  uint16_t conn = 0;
  bool ok = false;
  for (uint8_t type : {hci::kAddrPublic, hci::kAddrRandom}) {
    peer.type = type;
    say("connecting to %s (%s)", opt.address.c_str(),
        type == hci::kAddrPublic ? "public" : "random");
    if (link->connect(peer, 15.0, &conn, &err)) {
      ok = true;
      break;
    }
    say("  %s", err.c_str());
  }
  if (!ok) return 1;
  say("connected, handle 0x%04x", conn);

  std::vector<uint8_t> rsp;
  if (link->att_request(conn, att::exchange_mtu_request(247), &rsp, 5.0,
                        nullptr)) {
    uint16_t mtu = 0;
    if (att::parse_exchange_mtu_response(rsp, &mtu)) say("MTU %u", mtu);
  }

  // Walk every attribute rather than only the services. On a device whose
  // profile is the thing under investigation, the descriptors and their user
  // descriptions are exactly the interesting part -- "Server TX Data" is how
  // these characteristics got their names in the first place.
  uint16_t cursor = 0x0001;
  int printed = 0;
  for (int round = 0; round < 64 && cursor != 0; ++round) {
    if (!link->att_request(conn, att::find_information_request(cursor, 0xffff),
                           &rsp, 10.0, &err)) {
      break;
    }
    std::vector<att::HandleUuid> descs;
    if (!att::parse_find_information_response(rsp, &descs) || descs.empty()) {
      break;  // an Error Response: the end of the table
    }
    for (const att::HandleUuid& d : descs) {
      std::string line = "handle 0x" + std::to_string(d.handle) + "  " +
                         hci::uuid_to_string(d.uuid);
      // Read anything readable. A characteristic declaration decodes into the
      // properties and the value handle, which is the map of the device.
      std::vector<uint8_t> value;
      std::vector<uint8_t> read_rsp;
      if (link->att_request(conn, att::read_request(d.handle), &read_rsp, 5.0,
                            nullptr)) {
        att::ErrorResponse e;
        if (att::parse_error_response(read_rsp, &e)) {
          line += "  (" + std::string(att::error_name(e.error)) + ")";
        } else if (att::parse_read_response(read_rsp, &value)) {
          if (d.uuid == hci::uuid_from_16(att::kUuidCharacteristic) &&
              value.size() >= 5) {
            hci::Uuid cu;
            hci::uuid_from_le(value.data() + 3, value.size() - 3, &cu);
            line += "  char " + hci::uuid_to_string(cu) + " [" +
                    att::properties_to_string(value[0]) + "] value handle 0x" +
                    std::to_string(value[1] | (value[2] << 8));
          } else if (d.uuid == hci::uuid_from_16(att::kUuidPrimaryService)) {
            hci::Uuid su;
            hci::uuid_from_le(value.data(), value.size(), &su);
            line += "  service " + hci::uuid_to_string(su);
          } else {
            bool printable = !value.empty();
            for (uint8_t b : value) {
              if (b < 0x20 || b > 0x7e) printable = false;
            }
            line += "  = " + hci::to_hex(value);
            if (printable) {
              line += "  \"" + std::string(value.begin(), value.end()) + "\"";
            }
          }
        }
      }
      say("%s", line.c_str());
      ++printed;
      cursor = d.handle == 0xffff ? 0 : static_cast<uint16_t>(d.handle + 1);
    }
  }
  say("%d attributes", printed);
  link->disconnect(conn);
  return 0;
}

// ------------------------------------------------------------------ serve

// One advertisement, built from the options. Returns false when the requested
// fields do not fit -- which is the failure macOS hid by silently moving the
// UUID somewhere the peer could not see it.
bool build_adv(const Options& opt, std::vector<uint8_t>* out,
               std::string* why) {
  out->clear();
  if (!hci::append_ad_flags(
          out, hci::kFlagGeneralDiscoverable | hci::kFlagBrEdrNotSupported)) {
    *why = "flags did not fit";
    return false;
  }
  if (opt.advertise_uuid) {
    hci::Uuid u;
    if (!hci::uuid_from_string(opt.uuid, &u)) {
      *why = "\"" + opt.uuid + "\" is not a UUID";
      return false;
    }
    if (!hci::append_ad_service(out, u)) {
      *why = "the service UUID did not fit in 31 bytes";
      return false;
    }
  }
  if (opt.have_name && !hci::append_ad_name(out, opt.name)) {
    *why = "the name \"" + opt.name + "\" did not fit alongside the UUID";
    return false;
  }
  if (!opt.manufacturer.empty() &&
      !hci::append_ad(out, hci::kAdManufacturer, opt.manufacturer)) {
    *why = "the manufacturer data did not fit";
    return false;
  }
  return true;
}

int serve(const Options& opt, const std::vector<Options>& variants) {
  hci::Link::Options lo;
  lo.device = opt.device;
  lo.trace = opt.trace;
  std::string err;
  std::unique_ptr<hci::Link> link = hci::Link::open(lo, &err);
  if (!link) {
    std::fprintf(stderr, "octomancer-zoom: %s\n", err.c_str());
    return 1;
  }
  say("radio: %s", link->describe().c_str());

  // The profile, hosted exactly as the adapter presents it.
  att::ServerBuilder b;
  b.add_primary_service(hci::uuid_const(kZoomService));
  uint16_t tx = b.add_characteristic(hci::uuid_const(kZoomTx),
                                     att::kPropNotify | att::kPropRead, {},
                                     "Server TX Data");
  uint16_t rx = b.add_characteristic(
      hci::uuid_const(kZoomRx),
      att::kPropWrite | att::kPropWriteWithoutResponse, {}, "Server RX Data");
  uint16_t flow = b.add_characteristic(
      hci::uuid_const(kZoomFlow),
      att::kPropNotify | att::kPropWrite | att::kPropWriteWithoutResponse |
          att::kPropRead,
      {0x01}, "Flow Control");

  // Device Information, answering exactly what the real adapter answered.
  b.add_primary_service(hci::uuid_from_16(att::kUuidDeviceInformation));
  b.add_characteristic(hci::uuid_from_16(att::kUuidManufacturerName),
                       att::kPropRead, {'Z', 'O', 'O', 'M'});
  b.add_characteristic(hci::uuid_from_16(att::kUuidModelNumber),
                       att::kPropRead, {'F', '6'});
  b.add_characteristic(hci::uuid_from_16(att::kUuidSoftwareRevision),
                       att::kPropRead,
                       {'v', '_', '0', '.', '2', '1'});

  att::Server server(b.take());
  server.set_write_handler([&](uint16_t handle, const std::vector<uint8_t>& v) {
    const char* what = handle == rx     ? "Server RX Data"
                       : handle == flow ? "Flow Control"
                                        : "";
    say("  <-- WRITE handle 0x%04x %s%s%s", handle, what,
        *what ? " = " : "= ", hci::to_hex(v).c_str());
    // A write to a CCCD is a subscription, and it is the first thing worth
    // knowing: it means the far end found the characteristic and wants it.
    if (server.subscribed(tx)) say("      (subscribed to Server TX Data)");
  });

  uint16_t conn = 0;
  bool have_conn = false;

  link->set_connection_handlers(
      [&](const hci::Link::Conn& c) {
        conn = c.handle;
        have_conn = true;
        say("*** CONNECTED from %s (%s), handle 0x%04x ***",
            hci::address_to_string(c.peer).c_str(),
            c.peer.type == hci::kAddrPublic ? "public" : "random", c.handle);
      },
      [&](uint16_t handle, uint8_t reason) {
        if (handle != conn) return;
        have_conn = false;
        say("*** DISCONNECTED: %s ***", hci::status_name(reason));
      });

  link->set_att_handler([&](uint16_t c, const std::vector<uint8_t>& pdu) {
    if (pdu.empty()) return;
    say("  <-- %s  %s", att::opcode_name(pdu[0]), hci::to_hex(pdu).c_str());
    std::vector<uint8_t> rsp = server.handle_request(pdu);
    if (rsp.empty()) return;
    say("  --> %s  %s", att::opcode_name(rsp[0]), hci::to_hex(rsp).c_str());
    link->send_att(c, rsp, nullptr);
  });

  link->set_smp_handler([&](uint16_t c, const std::vector<uint8_t>& pdu) {
    // Nothing here pairs as a peripheral. Refusing plainly is better than
    // silence, which would leave the F6 waiting out its timeout -- and if the
    // F6 does ask, that itself is the answer to a question the notes leave
    // open.
    say("  <-- SMP %s  (refusing: pairing is not implemented as a peripheral)",
        hci::to_hex(pdu).c_str());
    std::vector<uint8_t> fail = {0x05, 0x05};  // Pairing Failed, not supported
    link->send_smp(c, fail, nullptr);
  });

  size_t variant = 0;
  double variant_until = 0;
  double next_tick = 0;

  while (!g_stop) {
    // Rotate the advertisement when sweeping and nothing has connected.
    if (!have_conn && mono_now() >= variant_until) {
      const Options& v = variants[variant % variants.size()];
      std::vector<uint8_t> adv;
      std::string why;
      if (!build_adv(v, &adv, &why)) {
        say("skipping variant %zu: %s", variant, why.c_str());
      } else {
        link->stop_advertising(nullptr);
        hci::AdvConfig cfg;
        cfg.adv_data = adv;
        cfg.scan_response = v.scan_response;
        cfg.type = hci::kAdvInd;
        cfg.interval_ms = v.interval_ms;
        if (!link->start_advertising(cfg, &err)) {
          say("could not advertise: %s", err.c_str());
        } else {
          say("advertising %s", hci::to_hex(adv).c_str());
          for (const std::string& line : hci::describe_ad(adv)) {
            say("    %s", line.c_str());
          }
          if (!v.scan_response.empty()) {
            say("  scan response %s", hci::to_hex(v.scan_response).c_str());
          }
        }
      }
      ++variant;
      variant_until = mono_now() + (variants.size() > 1 ? 25.0 : 1e9);
    }

    // Push whatever the caller asked for, once something is listening.
    if (have_conn && !opt.tx.empty() && server.subscribed(tx)) {
      double now = mono_now();
      if (now >= next_tick) {
        std::vector<uint8_t> note = server.notification(tx, opt.tx);
        if (!note.empty()) {
          say("  --> NOTIFY %s", hci::to_hex(opt.tx).c_str());
          link->send_att(conn, note, nullptr);
        }
        next_tick = opt.tick ? now + 1.0 : 1e9;
      }
    }

    struct timespec ts = {0, 200 * 1000 * 1000};
    nanosleep(&ts, nullptr);
  }

  link->stop_advertising(nullptr);
  if (have_conn) link->disconnect(conn);
  return 0;
}

// The parts of the advertisement the notes leave genuinely open. Each is a
// guess about what the F6 is looking for, and the sweep is a way of asking
// all of them without a person watching.
std::vector<Options> sweep_variants(const Options& base) {
  std::vector<Options> out;

  // 1. The firmware's own template: flags plus the 128-bit UUID, nothing else.
  //    This is the one CoreBluetooth could not reliably send.
  out.push_back(base);

  // 2. The same, plus a short name. "UltraSync BLUE" does not fit alongside a
  //    128-bit UUID -- 3 + 18 + 16 is 37 bytes against a 31-byte budget -- so
  //    the short forms are the only ones that can be tried together.
  for (const char* name : {"US", "USB", "UltraSy"}) {
    Options v = base;
    v.have_name = true;
    v.name = name;
    out.push_back(v);
  }

  // 3. The full name, with the UUID dropped to make room. If the F6 matches on
  //    name rather than service, this is what it wants.
  {
    Options v = base;
    v.advertise_uuid = false;
    v.have_name = true;
    v.name = "UltraSync BLUE";
    out.push_back(v);
  }

  // 4. Name in the scan response instead, which is where a device that wants
  //    both normally puts it. macOS gave no way to do this at all.
  {
    Options v = base;
    std::vector<uint8_t> sr;
    hci::append_ad_name(&sr, "UltraSync BLUE");
    v.scan_response = sr;
    out.push_back(v);
  }

  // 5. Manufacturer data. Atomos/Timecode Systems have their own company
  //    identifier, and a device that filters on one would ignore everything
  //    above. 0x0472 is Atomos's assigned ID.
  {
    Options v = base;
    v.manufacturer = {0x72, 0x04, 0x00, 0x00};
    out.push_back(v);
  }

  return out;
}

}  // namespace

int main(int argc, char** argv) {
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  Options opt;
  enum {
    kScan = 1000, kDump, kServe, kSweep, kName, kNoName, kUuid, kNoUuid,
    kManufacturer, kScanResponse, kInterval, kTx, kTick, kDevice, kTrace,
    kSeconds, kHelp,
  };
  static const struct option longs[] = {
      {"scan", optional_argument, nullptr, kScan},
      {"dump", required_argument, nullptr, kDump},
      {"serve", no_argument, nullptr, kServe},
      {"sweep", no_argument, nullptr, kSweep},
      {"name", required_argument, nullptr, kName},
      {"no-name", no_argument, nullptr, kNoName},
      {"uuid", required_argument, nullptr, kUuid},
      {"no-uuid", no_argument, nullptr, kNoUuid},
      {"manufacturer", required_argument, nullptr, kManufacturer},
      {"scan-response", required_argument, nullptr, kScanResponse},
      {"interval", required_argument, nullptr, kInterval},
      {"tx", required_argument, nullptr, kTx},
      {"tick", no_argument, nullptr, kTick},
      {"device", required_argument, nullptr, kDevice},
      {"trace", no_argument, nullptr, kTrace},
      {"seconds", required_argument, nullptr, kSeconds},
      {"help", no_argument, nullptr, kHelp},
      {nullptr, 0, nullptr, 0},
  };

  int c;
  while ((c = getopt_long(argc, argv, "", longs, nullptr)) != -1) {
    switch (c) {
      case kScan:
        opt.mode = Options::Mode::kScan;
        if (optarg) opt.seconds = std::atof(optarg);
        break;
      case kDump:
        opt.mode = Options::Mode::kDump;
        opt.address = optarg;
        break;
      case kServe: opt.mode = Options::Mode::kServe; break;
      case kSweep: opt.mode = Options::Mode::kSweep; break;
      case kName:
        opt.name = optarg;
        opt.have_name = true;
        break;
      case kNoName: opt.have_name = false; break;
      case kUuid: opt.uuid = optarg; break;
      case kNoUuid: opt.advertise_uuid = false; break;
      case kManufacturer: opt.manufacturer = from_hex(optarg); break;
      case kScanResponse: opt.scan_response = from_hex(optarg); break;
      case kInterval: opt.interval_ms = std::atof(optarg); break;
      case kTx: opt.tx = from_hex(optarg); break;
      case kTick: opt.tick = true; break;
      case kDevice: opt.device = optarg; break;
      case kTrace: opt.trace = true; break;
      case kSeconds: opt.seconds = std::atof(optarg); break;
      case kHelp: usage(); return 0;
      default: usage(); return 2;
    }
  }

  switch (opt.mode) {
    case Options::Mode::kScan: return do_scan(opt);
    case Options::Mode::kDump: return do_dump(opt);
    case Options::Mode::kServe: return serve(opt, {opt});
    case Options::Mode::kSweep: return serve(opt, sweep_variants(opt));
  }
  return 0;
}
