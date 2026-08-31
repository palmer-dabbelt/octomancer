// octomancerd -- watch the Tentacle Sync boxes and say what they are doing.
//
// The service listens passively to BLE advertisements, keeps a picture of every
// box in range, and serves that picture on a Unix socket. It never connects to
// anything and never writes to anything, so it cannot disturb the Tentacle app,
// a camera, or a recording in progress.
//
// Notification policy lives here rather than in the UI: the decision about
// whether a box has drifted is made once, from the full history, by the process
// that has the full history. A UI is then free to display it, and a headless
// install can hand it to --notify-command instead.
#include <getopt.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <set>
#include <cstdarg>
#include <map>
#include <memory>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "bmd.h"
#include "boxble.h"
#include "boxcdc.h"
#include "client.h"
#include "dongle.h"
#include "hciport.h"
#include "loop.h"
#include "devicedb.h"
#include "jsonlog.h"
#include "escape.h"
#include "naming.h"
#include "proto.h"
#include "proclock.h"
#include "registry.h"
#include "render.h"
#include "radio.h"
#include "scanbridge.h"
#include "scanner.h"
#include "server.h"
#include "timeutil.h"

namespace {

volatile sig_atomic_t g_stop = 0;
int g_wake_pipe[2] = {-1, -1};

void on_signal(int) {
  g_stop = 1;
  // Wake the poll loop. write() is async-signal-safe; almost nothing else is.
  if (g_wake_pipe[1] >= 0) {
    const char byte = 'x';
    ssize_t ignored = ::write(g_wake_pipe[1], &byte, 1);
    (void)ignored;
  }
}

struct Options {
  std::string socket_path = octo::default_socket_path();
  std::string lock_path = octo::default_lock_path("octomancerd");
  std::string log_path;
  std::string console_path;
  std::string notify_command;
  // Where the roster is kept between runs. Empty disables it, which is what
  // --probe wants: a ten-second listen should not rewrite the file that the
  // running agent is curating.
  std::string devices_path = octo::DeviceDb::default_path();
  double log_interval = 60.0;
  double probe_seconds = 0.0;
  // A sync daemon of our own to listen to, over the box protocol. Empty with
  // use_peer set means "find one"; --no-peer turns the whole thing off.
  //
  // Called a peer rather than a dongle because both better words are taken and
  // mean other things. --dongle already names the serial port for --radio
  // dongle, which drives a dongle's radio over HCI from *this* process -- the
  // transitional arrangement src/radio.h argues against, and the opposite of
  // this. And --box means a Tentacle in the octomancer CLI, which is the one
  // thing on screen it must not be confused with.
  std::string peer_port;
  bool use_peer = true;
  // Whether to reach a dongle over the air; see BleUse in src/dongle.h.
  octo::BleUse ble_use = octo::BleUse::kAuto;
  bool foreground = false;
  bool quiet = false;
  octo::Rotation rotation;
  octo::Policy policy;
};

void usage(FILE* out) {
  std::fprintf(out,
      "usage: octomancerd [options]\n"
      "\n"
      "Watch Tentacle Sync boxes over BLE and serve their state on a socket.\n"
      "\n"
      "  --socket PATH         control socket (default %s)\n"
      "  --lock PATH           the file that keeps a second one of these from\n"
      "                        starting. --probe does not take it.\n"
      "  --log PATH            append JSONL observations here\n"
      "  --log-interval SEC    how often to log a summary (default 60)\n"
      "  --console PATH        send stdout and stderr here, and rotate them.\n"
      "                        A file the program does not own cannot be\n"
      "                        rotated safely, so redirecting from the shell\n"
      "                        or from launchd leaves it to grow.\n"
      "  --log-max-bytes N     rotate past this size, 0 to never (default 16M)\n"
      "  --log-keep N          generations to keep beside it (default 5)\n"
      "  --probe SEC           listen for SEC seconds, print a report, exit\n"
      "  --foreground          stay attached; the launchd agent uses this\n"
      "  --quiet               no chatter on stderr\n"
      "  --peer PORT           listen to a sync daemon of our own on PORT --\n"
      "                        a dongle running this firmware. Without it,\n"
      "                        one plugged in is found automatically. Its\n"
      "                        boxes are listed beside ours, tagged with\n"
      "                        which radio heard them; they are never merged,\n"
      "                        because nothing can prove two rows are the\n"
      "                        same box.\n"
      "  --no-peer             do not go looking for one\n"
      "  --peer-bluetooth W    off, auto or both (default auto). `auto`\n"
      "                        reaches a dongle over the air only when\n"
      "                        there is no cable to it -- holding a\n"
      "                        connection costs this Mac scan time, which\n"
      "                        is the thing it is here to spend. `both`\n"
      "                        brings the radio link up beside the cable\n"
      "                        so it can be tested without unplugging the\n"
      "                        only way of seeing what the box is doing.\n"
      "\n"
      "  --alert-threshold SEC a box this far from this Mac needs re-jamming\n"
      "                        (default 60)\n"
      "  --alert-clear SEC     ...and is considered fixed below this (default 45)\n"
      "  --alert-confirm N     consecutive observations before believing either\n"
      "                        transition (default 3)\n"
      "  --renotify SEC        repeat a standing alert this often (default 1800)\n"
      "  --notify-command CMD  run `sh -c CMD` on an alert. The box, the offset\n"
      "                        and the state arrive in $OCTOMANCER_* rather than\n"
      "                        interpolated into CMD, which would let a box name\n"
      "                        off the air run as a shell command.\n"
      "\n"
      "  --window SEC          how much history to keep per box (default 3600)\n"
      "  --stale-after SEC     stop counting a silent box (default 30)\n"
      "  --min-drift-span SEC  shortest history worth fitting drift to\n"
      "                        (default 900)\n"
      "\n"
      "  --devices PATH      the roster kept between runs ('' to disable,\n"
      "                      default ~/.octomancer/devices.json). octomancerd\n"
      "                      remembers every device it has seen and shows the\n"
      "                      ones it is not hearing as offline; the sync daemon\n"
      "                      deliberately does not, having nowhere to put them.\n"
      "  --radio KIND        auto (default: this host's own radio),\n"
      "                      corebluetooth, dongle, or fake. A dongle in a USB\n"
      "                      port is a second radio, not a better first one;\n"
      "                      auto never takes one. See doc/box-notes.md.\n"
      "  --dongle PORT       the dongle's serial port\n"
      "  --hci-trace         log every HCI packet\n"
      "  --version, --help\n",
      octo::default_socket_path().c_str());
}

bool parse_args(int argc, char** argv, Options* opt) {
  enum {
    kSocket = 1000, kLog, kLogInterval, kConsole, kLogMax, kLogKeep,
    kProbe, kForeground, kQuiet, kLockFile,
    kThreshold, kClear, kConfirm, kRenotify, kNotify,
    kWindow, kStale, kDriftSpan, kCameraGone,
    kRadio, kDongle, kHciTrace, kVersion, kHelp,
    kDevices, kPeer, kNoPeer, kPeerBluetooth,
  };
  static const struct option longs[] = {
      {"socket", required_argument, nullptr, kSocket},
      {"lock", required_argument, nullptr, kLockFile},
      {"log", required_argument, nullptr, kLog},
      {"log-interval", required_argument, nullptr, kLogInterval},
      {"console", required_argument, nullptr, kConsole},
      {"log-max-bytes", required_argument, nullptr, kLogMax},
      {"log-keep", required_argument, nullptr, kLogKeep},
      {"probe", required_argument, nullptr, kProbe},
      {"foreground", no_argument, nullptr, kForeground},
      {"quiet", no_argument, nullptr, kQuiet},
      {"alert-threshold", required_argument, nullptr, kThreshold},
      {"alert-clear", required_argument, nullptr, kClear},
      {"alert-confirm", required_argument, nullptr, kConfirm},
      {"renotify", required_argument, nullptr, kRenotify},
      {"notify-command", required_argument, nullptr, kNotify},
      {"window", required_argument, nullptr, kWindow},
      {"stale-after", required_argument, nullptr, kStale},
      {"min-drift-span", required_argument, nullptr, kDriftSpan},
      {"camera-gone-after", required_argument, nullptr, kCameraGone},
      {"devices", required_argument, nullptr, kDevices},
      {"radio", required_argument, nullptr, kRadio},
      {"dongle", required_argument, nullptr, kDongle},
      {"peer", required_argument, nullptr, kPeer},
      {"no-peer", no_argument, nullptr, kNoPeer},
      {"peer-bluetooth", required_argument, nullptr, kPeerBluetooth},
      {"hci-trace", no_argument, nullptr, kHciTrace},
      {"version", no_argument, nullptr, kVersion},
      {"help", no_argument, nullptr, kHelp},
      {nullptr, 0, nullptr, 0},
  };

  for (;;) {
    const int c = getopt_long(argc, argv, "", longs, nullptr);
    if (c == -1) break;
    switch (c) {
      case kSocket: opt->socket_path = optarg; break;
      case kLockFile: opt->lock_path = optarg; break;
      case kLog: opt->log_path = optarg; break;
      case kLogInterval: opt->log_interval = std::atof(optarg); break;
      case kConsole: opt->console_path = optarg; break;
      case kLogMax: opt->rotation.max_bytes = std::atof(optarg); break;
      case kLogKeep: opt->rotation.keep = std::atoi(optarg); break;
      case kProbe: opt->probe_seconds = std::atof(optarg); break;
      case kForeground: opt->foreground = true; break;
      case kQuiet: opt->quiet = true; break;
      case kThreshold: opt->policy.alert_enter = std::atof(optarg); break;
      case kClear: opt->policy.alert_exit = std::atof(optarg); break;
      case kConfirm: opt->policy.alert_confirm = std::atoi(optarg); break;
      case kRenotify: opt->policy.renotify_after = std::atof(optarg); break;
      case kNotify: opt->notify_command = optarg; break;
      case kDevices: opt->devices_path = optarg; break;
      case kPeer: opt->peer_port = optarg; opt->use_peer = true; break;
      case kNoPeer: opt->use_peer = false; break;
      case kPeerBluetooth:
        if (!octo::parse_ble_use(optarg, &opt->ble_use)) {
          std::fprintf(stderr,
                       "octomancerd: --peer-bluetooth wants off,"
                       " auto or both, not '%s'\n", optarg);
          return false;
        }
        break;
      case kWindow: opt->policy.window = std::atof(optarg); break;
      case kStale: opt->policy.stale_after = std::atof(optarg); break;
      case kDriftSpan: opt->policy.min_drift_span = std::atof(optarg); break;
      case kCameraGone: opt->policy.camera_gone_after = std::atof(optarg); break;
      case kRadio:
        if (!octo::parse_radio_kind(optarg, &octo::radio_options().kind)) {
          std::fprintf(stderr,
                       "%s: --radio must be auto, corebluetooth or dongle\n",
                       "octomancerd");
          return false;
        }
        break;
      case kDongle:
        octo::radio_options().device = optarg;
        // Naming a port is asking for it. Falling back to CoreBluetooth when
        // it turns out not to be there would hide a typo.
        octo::radio_options().kind = octo::RadioKind::kDongle;
        break;
      case kHciTrace: octo::radio_options().trace = true; break;
      case kVersion:
        std::printf("octomancerd %s\n", OCTO_VERSION);
        std::exit(0);
      case kHelp:
        usage(stdout);
        std::exit(0);
      default:
        usage(stderr);
        return false;
    }
  }

  if (opt->policy.alert_exit > opt->policy.alert_enter) {
    std::fprintf(stderr,
                 "octomancerd: --alert-clear (%.1f) must not exceed"
                 " --alert-threshold (%.1f), or a box could never stop"
                 " alerting\n",
                 opt->policy.alert_exit, opt->policy.alert_enter);
    return false;
  }
  return true;
}

// Run the notification hook. The box name arrives over the air from a device
// we do not control, so it is passed in the environment and never spliced into
// the command line.
void run_notify(const std::string& command, const octo::AlertEvent& event,
                double threshold) {
  if (command.empty()) return;
  const pid_t pid = ::fork();
  if (pid != 0) return;  // parent (or a failed fork) carries on

  char offset[64], thresh[64];
  std::snprintf(offset, sizeof offset, "%.3f", event.offset);
  std::snprintf(thresh, sizeof thresh, "%.1f", threshold);
  ::setenv("OCTOMANCER_BOX", event.name.c_str(), 1);
  ::setenv("OCTOMANCER_ID", event.id.c_str(), 1);
  ::setenv("OCTOMANCER_OFFSET", offset, 1);
  ::setenv("OCTOMANCER_THRESHOLD", thresh, 1);
  ::setenv("OCTOMANCER_STATE", event.entering ? "drifted" : "recovered", 1);
  ::execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
  ::_exit(127);
}

std::string json_escape(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (c == '"' || c == '\\') out.push_back('\\');
    if (static_cast<unsigned char>(c) < 0x20) { out += ' '; continue; }
    out.push_back(c);
  }
  return out;
}

void log_alert(octo::JsonLog* log, const octo::AlertEvent& e) {
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "\"box\":\"%s\",\"id\":\"%s\",\"offset_s\":%.3f,"
                "\"state\":\"%s\",\"repeat\":%s",
                json_escape(e.name).c_str(), json_escape(e.id).c_str(), e.offset,
                e.entering ? "drifted" : "recovered", e.repeat ? "true" : "false");
  log->record("alert", buf);
}

// How often the roster is written out. Half a minute, which is the same order
// as the staleness threshold: losing the last interval to a hard kill costs at
// most one device's last-seen time being a little early, and nothing else.
const double kSaveInterval = 30.0;

// How long to wait for the radio to say anything before deciding that silence
// is itself the answer. See the check in the main loop.
const double kRadioSilentAfter = 10.0;

// Failing to save is reported once and then carried on from, deliberately. A
// full disk or a read-only home should not stop a daemon whose actual job is
// listening to a radio -- but it must not be silent either, or a roster
// quietly stops surviving restarts and nobody finds out until they are looking
// for a box that has gone missing.
void save_devices(const octo::Registry& registry, const octo::NameBook& names,
                  const std::string& path, bool quiet) {
  static bool complained = false;
  octo::DeviceDb db;
  std::string err;

  std::vector<octo::RememberedDevice> rows =
      registry.remembered(octo::wall_now());
  // The names go back into the records on the way out. The registry knows
  // only what a device last called itself; everything else about its label --
  // what a person chose, what a probe found -- lives in the book.
  std::set<std::string> written;
  for (octo::RememberedDevice& d : rows) {
    const auto it = names.all().find(d.id);
    if (it == names.all().end()) continue;
    d.user_name = it->second.user;
    d.probed_name = it->second.probed;
    d.probed = it->second.probed_done;
    written.insert(d.id);
  }
  // A device somebody named that this Mac's radio has never heard has no row
  // in the registry to carry its name out on -- most often one of a dongle's,
  // which lives in a different namespace entirely. Losing it would mean a
  // rename silently failing to survive a restart, for exactly the devices
  // whose owner cared enough to label one.
  //
  // These are written with no sighting in them, and the loader knows to put
  // them in the name book rather than the roster. See the note there.
  for (const auto& entry : names.all()) {
    if (written.count(entry.first) != 0) continue;
    if (entry.second.user.empty()) continue;
    octo::RememberedDevice d;
    d.id = entry.first;
    d.name = entry.second.heard;
    d.user_name = entry.second.user;
    d.probed_name = entry.second.probed;
    d.probed = entry.second.probed_done;
    rows.push_back(d);
  }

  if (db.save(path, rows, &err)) {
    complained = false;
    return;
  }
  if (!complained && !quiet) {
    std::fprintf(stderr, "octomancerd: cannot save the device roster: %s\n",
                 err.c_str());
    complained = true;
  }
}

// A sync daemon of our own, on the end of a USB cable.
//
// All the judgement is in src/dongle.h, which is tested; this is the part that
// owns a file descriptor. It exists because octomancerd is the one program
// with any business knowing a dongle is plugged in -- see "Two radios, and
// which program knows" in doc/box-notes.md, and the long note above
// choose_dongle() in src/radio.h.
//
// Finding one is the awkward part. There is no way here to ask macOS which
// /dev/cu.usbmodem* is ours -- that would want IOKit, and this file is
// deliberately portable -- so every candidate is opened in turn and the
// greeting is what settles it. A port that does not say `hello` within a few
// seconds is somebody else's microcontroller: it is let go, and not tried
// again for a good while, because holding a stranger's Arduino open every
// three seconds forever is exactly the sort of thing that gets a daemon
// uninstalled. Nothing is ever written to a port that has not greeted us.
class BoxPeer {
 public:
  // How long to give a freshly opened port to introduce itself. Generous: the
  // firmware sends its greeting when DTR rises, and macOS takes a moment over
  // that.
  static constexpr double kGreetWithin = 4.0;
  // How often to go looking when there is nothing attached.
  static constexpr double kRetryEvery = 5.0;
  // How long a port that did not greet us is left alone.
  static constexpr double kShyFor = 300.0;

  // How often to try the radio again after it has given up. Longer than the
  // cable's retry: a failed connection over the air costs airtime in a room
  // this daemon is trying to listen to, and a dongle that is not there will
  // not be there a second later either.
  static constexpr double kBleRetryEvery = 30.0;

  BoxPeer(std::string named, octo::BleUse ble, bool quiet)
      : named_(std::move(named)),
        ble_use_(ble),
        quiet_(quiet),
        loop_(octo::make_loop()),
        born_(octo::mono_now()) {}

  // Everything that has to happen on a tick: service the link, give up on a
  // port that never spoke, ask for devices when one is due, and go looking
  // when there is nothing attached.
  void pump(double now) {
    if (loop_) loop_->tick(0.0);
    // CoreBluetooth delivers on a queue of its own; this is where what it left
    // becomes this thread's work. Before anything reads the link's state, so
    // that a disconnection noticed here is acted on in the same tick.
    if (air_) air_->pump();

    // The link reported itself closed from inside its own callback, where it
    // could not be destroyed. This is the first safe moment.
    if (gone_) drop(now);
    if (air_ && !air_->is_open()) {
      say("the radio link to %s closed", air_name_.c_str());
      air_.reset();
      air_greeted_ = false;
      next_air_try_ = now + kBleRetryEvery;
    }

    tend_radio(now);

    if (link_ && !usb_greeted_ && now - opened_at_ > kGreetWithin) {
      say("%s did not answer the box protocol -- not a sync daemon, leaving"
          " it alone", port_name_.c_str());
      shy_[port_name_] = now;
      drop(now);
    }

    // Exactly one link carries the conversation, even when both are up. Two
    // feeding one view would deliver every device list twice, and since a list
    // replaces the last one wholesale, that is not a doubled bench -- it is a
    // bench alternating between two radios' answers on no schedule anybody
    // chose. src/dongle.h's carrier() is the rule; this is where it lands.
    const octo::LinkWay was = carrying_;
    carrying_ = octo::carrier(link_ != nullptr && usb_greeted_,
                              air_ != nullptr && air_->ready() && air_greeted_);
    if (carrying_ != was) {
      // The view belongs to whichever link is speaking. Handing it over means
      // forgetting what the other one said: the two radios are the same radio
      // -- it is one dongle -- but a half-arrived batch from the old link
      // would be completed by the new one's `end` and produce a room that
      // never existed.
      view_.closed(now);
      if (carrying_ != octo::LinkWay::kNone) {
        view_.opened(now);
        view_.observe(greeting_, now);
      }
      say("now talking to the dongle over %s", octo::link_way_name(carrying_));
    }

    octo::BoxTransport* voice = carrying_ == octo::LinkWay::kUsb
                                    ? link_.get()
                                    : carrying_ == octo::LinkWay::kBluetooth
                                          ? air_.get()
                                          : nullptr;
    if (voice != nullptr) {
      // The date first, and before any question, because a box that does not
      // know the date cannot act on the answer to one.
      octo::Message tell;
      if (view_.wants_date(today(), &tell)) {
        voice->send(tell);
        if (voice->is_open()) {
          view_.dated(today());
          say("told the dongle today's date (%04d-%02d-%02d) -- it has no"
              " clock to remember one with",
              today().year, today().month, today().day);
        }
      }

      octo::Message ask;
      if (view_.wants_poll(now, &ask)) {
        voice->send(ask);
        // A failed write closes the link rather than returning, so asking
        // whether it is still open is how "did that go out" is spelled here.
        if (voice->is_open()) view_.polled(now);
      }
    }

    if (link_ != nullptr) return;
    if (now < next_try_) return;
    next_try_ = now + kRetryEvery;
    attach(now);
  }

  // Today, as this Mac understands it. UTC, because that is what a camera's
  // real-time clock is specified in -- see src/bmd.h, where writing local time
  // gets the offset applied twice.
  static octo::DateStamp today() {
    const octo::bmd::Civil now = octo::bmd::utc_civil(octo::wall_now());
    octo::DateStamp out;
    out.year = now.year;
    out.month = now.month;
    out.day = now.day;
    return out;
  }

  // Whether the radio link is up, for the status line. Reported separately
  // from which link is carrying, because "connected but not in charge" is the
  // whole point of the debug mode and has to be visible as itself.
  bool radio_attached() const { return air_ && air_->ready() && air_greeted_; }
  octo::LinkWay carrying() const { return carrying_; }
  const std::string& radio_name() const { return air_name_; }

  const octo::DongleView& view() const { return view_; }

 private:
  __attribute__((format(printf, 2, 3))) void say(const char* fmt, ...) {
    if (quiet_) return;
    va_list ap;
    va_start(ap, fmt);
    std::fputs("octomancerd: ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
  }

  void attach(double now) {
    std::vector<std::string> candidates;
    if (!named_.empty()) {
      candidates.push_back(named_);
    } else {
      candidates = octo::hci::list_candidate_ports();
    }
    for (const std::string& path : candidates) {
      auto shy = shy_.find(path);
      if (shy != shy_.end() && now - shy->second < kShyFor) continue;
      std::string err;
      std::unique_ptr<octo::hci::Port> port = octo::hci::open_port(path, &err);
      if (port == nullptr) continue;
      port_name_ = port->name();
      link_ = octo::BoxLink::attach(loop_.get(), std::move(port));
      if (link_ == nullptr) continue;
      opened_at_ = now;
      usb_greeted_ = false;
      link_->on_message([this](const octo::Message& msg) {
        if (msg.verb == "hello") {
          greeting_ = msg;
          if (!usb_greeted_) {
            usb_greeted_ = true;
            say("a sync daemon answered on %s", port_name_.c_str());
          }
        }
        // Only the carrier's messages reach the view; see pump().
        if (carrying_ == octo::LinkWay::kUsb) {
          view_.observe(msg, octo::mono_now());
        } else {
          ++usb_heard_;
        }
      });
      link_->on_closed([this](const std::string& why) {
        say("%s went away: %s", port_name_.c_str(), why.c_str());
        // Not drop(): we are inside the link's own callback, so the object
        // has to outlive this. The next pump() clears it.
        view_.closed(octo::mono_now());
        gone_ = true;
      });
      return;
    }
  }

  // Bring the radio link up, or leave it alone, per src/dongle.h's rule.
  //
  // The cost of getting this wrong is not a crash, it is a daemon that quietly
  // spends the radio it is supposed to be listening with. So the rule is in a
  // tested function and this only obeys it.
  void tend_radio(double now) {
    if (ble_use_ == octo::BleUse::kOff) return;

    // Give the cable first refusal.
    //
    // At the instant this daemon starts, nothing knows whether there is a
    // dongle in a USB port: the port has not been opened and no greeting has
    // arrived. So `auto` would reason "no cable, bring up the radio", connect
    // over the air, and drop it a second later when the cable introduced
    // itself -- a connection made and thrown away on every single start, and
    // airtime spent to learn something the next tick was going to say anyway.
    if (!settled_) {
      if (usb_greeted_) {
        settled_ = true;              // the cable answered; no need to wait
      } else if (now - born_ < kGreetWithin + kRetryEvery) {
        return;                       // still time for it to
      } else {
        settled_ = true;              // long enough; there is no cable
      }
    }

    const bool usb_ready = link_ != nullptr && usb_greeted_;
    const bool want = octo::want_bluetooth(
        usb_ready, ble_use_ == octo::BleUse::kBoth);

    if (!want) {
      if (air_) {
        say("the cable is enough; letting the radio link go");
        air_->close("usb is carrying");
        air_.reset();
        air_greeted_ = false;
      }
      return;
    }

    if (air_ || now < next_air_try_) return;
    next_air_try_ = now + kBleRetryEvery;

    std::string err;
    air_ = octo::open_box_ble(named_.empty() ? std::string() : std::string(),
                              &err);
    if (!air_) {
      // Only worth saying once: a host with no radio will have no radio next
      // time either, and a daemon that repeats itself every thirty seconds is
      // a log nobody reads.
      if (!ble_complained_) {
        ble_complained_ = true;
        say("cannot look for a dongle over the air: %s", err.c_str());
      }
      return;
    }
    air_greeted_ = false;
    air_name_ = "a dongle";
    air_->on_message([this](const octo::Message& msg) {
      if (msg.verb == "hello") {
        air_greeted_ = true;
        greeting_ = msg;
        air_name_ = air_ ? air_->name() : std::string("a dongle");
        say("a sync daemon answered over Bluetooth on %s", air_name_.c_str());
      }
      // Only the carrier's messages reach the view. The other link stays up in
      // debug mode and is heard and discarded, which is what makes it possible
      // to exercise the radio while the cable is still doing the work.
      if (carrying_ == octo::LinkWay::kBluetooth) {
        view_.observe(msg, octo::mono_now());
      } else {
        ++air_heard_;
      }
    });
    air_->on_closed([this](const std::string& why) {
      say("the radio link ended: %s", why.c_str());
    });
  }

  void drop(double now) {
    if (link_) link_->close("octomancerd let go");
    link_.reset();
    usb_greeted_ = false;
    // Only if the cable was the one talking. Tearing down the view because a
    // cable was unplugged, while the radio link is carrying, would blank a
    // bench that is still being measured.
    if (carrying_ == octo::LinkWay::kUsb) view_.closed(now);
    gone_ = false;
    port_name_.clear();
  }

  std::string named_;
  octo::BleUse ble_use_ = octo::BleUse::kAuto;
  bool quiet_;
  std::unique_ptr<octo::Loop> loop_;
  std::unique_ptr<octo::BoxLink> link_;
  octo::DongleView view_;
  std::map<std::string, double> shy_;
  std::string port_name_;
  double opened_at_ = 0.0;
  bool usb_greeted_ = false;
  uint64_t usb_heard_ = 0;
  double next_try_ = 0.0;
  bool gone_ = false;

  // The radio half.
  std::unique_ptr<octo::BoxTransport> air_;
  std::string air_name_;
  bool air_greeted_ = false;
  bool ble_complained_ = false;
  double next_air_try_ = 0.0;
  // When this object was made, and whether the cable has been given its
  // chance yet. See tend_radio.
  double born_ = 0.0;
  bool settled_ = false;
  // Messages heard over the radio while the cable was in charge. The debug
  // mode's whole output: a number that goes up is a link that works.
  uint64_t air_heard_ = 0;

  octo::LinkWay carrying_ = octo::LinkWay::kNone;
  // The greeting, kept so that a handover can tell the view what it is talking
  // to without waiting for the box to introduce itself again.
  octo::Message greeting_;
};

void log_snapshot(octo::JsonLog* log, const octo::Snapshot& s) {
  std::string fields;
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "\"radio\":\"%s\",\"devices\":%d,\"live\":%d,\"alerting\":%d,"
                "\"adverts\":%lld,\"undecodable\":%lld,\"clock_steps\":%lld",
                json_escape(s.radio).c_str(), s.devices, s.live, s.alerting,
                static_cast<long long>(s.adverts_total),
                static_cast<long long>(s.undecodable_total),
                static_cast<long long>(s.clock_steps));
  fields = buf;
  if (s.has_bench) {
    std::snprintf(buf, sizeof buf, ",\"bench_offset_s\":%.6f,\"bench_spread_s\":%.6f",
                  s.bench_offset, s.bench_spread);
    fields += buf;
  }
  fields += ",\"box\":{";
  bool first = true;
  for (const octo::DeviceSnapshot& d : s.device) {
    if (!d.has_time) continue;
    if (!first) fields += ",";
    first = false;
    std::snprintf(buf, sizeof buf,
                  "\"%s\":{\"offset_s\":%.6f,\"median_s\":%.6f,\"samples\":%d,"
                  "\"rssi\":%d,\"age_s\":%.1f,\"live\":%s,\"resolution\":\"%s\"",
                  json_escape(d.name).c_str(), d.offset, d.median_offset,
                  d.samples, d.rssi, d.age, d.live ? "true" : "false",
                  json_escape(d.resolution).c_str());
    fields += buf;
    if (d.has_drift) {
      std::snprintf(buf, sizeof buf, ",\"drift_ppm\":%.3f,\"drift_span_s\":%.1f",
                    d.drift_ppm, d.drift_span);
      fields += buf;
    }
    fields += "}";
  }
  fields += "}";
  if (s.camera.seen) {
    std::snprintf(buf, sizeof buf,
                  ",\"camera\":{\"present\":%s,\"name\":\"%s\",\"rssi\":%d,"
                  "\"age_s\":%.1f,\"since_s\":%.1f,\"sessions\":%lld}",
                  s.camera.present ? "true" : "false",
                  json_escape(s.camera.name).c_str(), s.camera.rssi,
                  s.camera.age, s.camera.since,
                  static_cast<long long>(s.camera.sessions));
    fields += buf;
  }
  log->record("bench", fields);
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  // The environment is read before the flags so a flag can override it. This
  // matters because the agents are started by launchd, where there is no
  // command line to edit -- OCTOMANCER_RADIO and OCTOMANCER_DONGLE are how an
  // unattended agent is pointed at the dongle.
  {
    std::string env_err;
    if (!octo::radio_options_from_env(&env_err)) {
      std::fprintf(stderr, "octomancerd: %s\n", env_err.c_str());
      return 2;
    }
  }
  if (!parse_args(argc, argv, &opt)) return 2;

  // A client that hangs up mid-reply must not take the daemon down with it,
  // and a notify hook that exits must not leave a zombie behind.
  ::signal(SIGPIPE, SIG_IGN);
  ::signal(SIGCHLD, SIG_IGN);
  if (::pipe(g_wake_pipe) != 0) {
    std::fprintf(stderr, "octomancerd: pipe: %s\n", strerror(errno));
    return 1;
  }
  ::signal(SIGINT, on_signal);
  ::signal(SIGTERM, on_signal);

  // One listener. A second would scan the same radio and keep a second copy of
  // the bench's history, and the two would drift apart. A --probe run is a
  // hand-run look at what is in the room and keeps nothing, so it is allowed
  // alongside the daemon.
  octo::ProcLock lock;
  if (opt.probe_seconds <= 0.0 && !opt.lock_path.empty()) {
    long holder = 0;
    std::string lock_err;
    if (!lock.acquire(opt.lock_path, &holder, &lock_err)) {
      std::fprintf(stderr, "octomancerd: %s\n", lock_err.c_str());
      if (holder > 0) {
        std::fprintf(stderr,
                     "  Stop that one first, or use --probe to listen"
                     " alongside it.\n  Lock: %s\n", opt.lock_path.c_str());
      }
      return 1;
    }
  }

  // Never, on this side of the seam. The default exists for a sync daemon,
  // which may be a box with nothing but NVS and has to hold a working set;
  // octomancerd has a filesystem and is supposed to know every device it has
  // ever seen. Set before the registry is built, because it is the registry's
  // own copy of the policy that matters.
  opt.policy.forget_after = 0.0;

  octo::Registry registry(opt.policy);
  std::string err;

  // The roster from last time. Loaded before anything can be served, so a
  // client connecting in the first second sees the same list as one connecting
  // later rather than watching it fill in.
  //
  // A file that will not parse is fatal. Carrying on would serve a roster
  // short of everything the file held and then overwrite the file with the
  // short version on the next save -- turning a parse error into data loss,
  // quietly, in the direction nobody checks.
  octo::DeviceDb devices;
  if (!opt.devices_path.empty()) {
    if (!devices.load(opt.devices_path, &err)) {
      std::fprintf(stderr, "octomancerd: %s\n", err.c_str());
      return 1;
    }
    const double now = octo::wall_now();
    for (const octo::RememberedDevice& d : devices.devices()) {
      // A record with no sighting in it is a name and nothing else: somebody
      // labelled a device this Mac's radio has never heard -- most often one
      // of a dongle's, which lives in a different namespace entirely. It
      // belongs in the name book, which is loaded separately, and not in the
      // roster.
      //
      // Handing it to the registry made it a device in its own right, dated
      // from the epoch, so the bench grew a phantom row aged fifty-six years
      // beside the real one it was the name of.
      if (d.last_seen_wall <= 0.0) continue;
      registry.remember(d, now);
    }
  }

  // Before anything is printed, so the first line of a run lands in the same
  // file as the rest of it.
  octo::ConsoleLog console;
  if (!console.open(opt.console_path, opt.rotation, &err)) {
    std::fprintf(stderr, "octomancerd: %s\n", err.c_str());
    return 1;
  }

  octo::JsonLog log;
  log.set_rotation(opt.rotation);
  if (!log.open(opt.log_path, &err)) {
    std::fprintf(stderr, "octomancerd: %s\n", err.c_str());
    return 1;
  }

  // Everything the radio says arrives here on CoreBluetooth's own queue and
  // is replayed on this thread. The registry has no lock any more -- it cannot
  // have one and still be compiled for the box -- so the handoff is the
  // bridge's job rather than the registry's. See src/scanbridge.h.
  octo::ScanBridge bridge(&octo::default_loop());
  if (!bridge.ok()) {
    std::fprintf(stderr, "octomancerd: %s\n", bridge.error().c_str());
    return 1;
  }
  bridge.on_advert(
      [&registry](const octo::Advert& a) {
        registry.observe(a.id, a.name, a.rssi, a.data.data(), a.data.size(),
                         a.mono, a.wall);
      });
  // A camera in range is worth knowing about even though this program will
  // never touch one: it is the cheap half of the question octomancer-sync
  // would otherwise answer with a twenty-second scan every minute. The radio
  // is already listening, so noticing costs nothing.
  bridge.on_camera(
      [&registry](const octo::Sighting& seen) {
        registry.observe_camera(seen.id, seen.name, seen.rssi, seen.mono,
                                seen.wall);
      });
  bridge.on_state(
      [&registry, &log, &opt](const std::string& state) {
        registry.set_radio(state);
        if (log.enabled()) {
          log.record("radio", "\"state\":\"" + json_escape(state) + "\"");
        }
        if (!opt.quiet) {
          std::fprintf(stderr, "octomancerd: radio %s\n", state.c_str());
          if (state == "unauthorized") {
            std::fprintf(stderr,
                         "octomancerd: Bluetooth is not permitted for this"
                         " program.\n  Approve it in System Settings > Privacy"
                         " & Security > Bluetooth.\n");
          }
        }
      });

  // Said before the scanner is started rather than after, so it is on the
  // console even when starting fails -- "cannot start the radio" is a much
  // better message when the line above it says which radio was meant.
  if (!opt.quiet) {
    std::fprintf(stderr, "octomancerd: radio: %s\n",
                 octo::describe_radio().c_str());
  }

  auto scanner = octo::make_ble_scanner(
      bridge.advert_sink(), bridge.camera_sink(), bridge.state_sink());
  if (!scanner || !scanner->start(&err)) {
    std::fprintf(stderr, "octomancerd: cannot start the radio: %s\n",
                 err.empty() ? "unsupported on this host" : err.c_str());
    return 1;
  }

  // --probe is the "does any of this work" mode: listen, report, exit. It runs
  // without a socket so it can be used while the agent is already running.
  if (opt.probe_seconds > 0.0) {
    const double until = octo::mono_now() + opt.probe_seconds;
    if (!opt.quiet) {
      std::fprintf(stderr, "listening %.0fs for Tentacle boxes...\n",
                   opt.probe_seconds);
    }
    while (!g_stop && octo::mono_now() < until) {
      struct timespec ts = {0, 200 * 1000 * 1000};
      nanosleep(&ts, nullptr);
      bridge.drain();
    }
    // Once more after the last sleep, so the report includes whatever arrived
    // during it. A probe that listens for ten seconds and then reports on nine
    // of them would be measuring its own bookkeeping.
    bridge.drain();
    const octo::Snapshot snap = registry.snapshot();
    std::fputs(octo::render_human(snap, isatty(1)).c_str(), stdout);
    scanner->stop();
    return snap.live > 0 ? 0 : 1;
  }

  // The registry's own handler, with one thing added: a `forget` is written
  // through immediately rather than waiting for the save timer.
  //
  // Everything else in the roster is a fact about the world that will be
  // re-observed within seconds, so losing thirty of them to a hard kill costs
  // nothing. A removal is the opposite -- it is a decision, it is the one
  // thing here a person actually asked for, and a device that came back from
  // the dead because the daemon was killed a moment later would be indexed
  // under "this program does not do what I tell it".
  // A sync daemon of our own, if there is one plugged in. Its boxes are
  // appended to every snapshot, tagged with which radio heard them; they are
  // never merged with ours, because the two radios cannot agree on what to
  // call a box and nothing in a Tentacle advertisement would settle it. See
  // src/dongle.h.
  std::unique_ptr<BoxPeer> box;
  if (opt.use_peer) {
    box.reset(new BoxPeer(opt.peer_port, opt.ble_use, opt.quiet));
  }

  // Every name on every row comes from here, whichever radio heard the device
  // and whether or not it is switched on today. See src/naming.h: the
  // registry knows what a device last called itself, which is only one of the
  // three things that can claim to know its name and the weakest of them.
  octo::NameBook names;
  for (const octo::RememberedDevice& d : devices.devices()) {
    octo::DeviceName entry;
    entry.heard = d.name;
    entry.user = d.user_name;
    entry.probed = d.probed_name;
    entry.probed_done = d.probed;
    names.put(d.id, entry);
  }

  auto assemble = [&registry, &box, &names]() {
    octo::Snapshot snap = registry.snapshot();
    if (box) {
      const std::vector<octo::DeviceSnapshot> rows =
          box->view().devices(octo::mono_now());
      for (const octo::DeviceSnapshot& d : rows) {
        ++snap.remote_devices;
        if (d.live) ++snap.remote_live;
        snap.device.push_back(d);
      }
    }
    // Applied last, so that it applies to a dongle's rows too -- which is the
    // case that has none of its own, because a passive listener never sends
    // the scan request that would fetch one.
    for (octo::DeviceSnapshot& d : snap.device) {
      names.heard(d.id, d.name);
      d.name = names.display(d.id);
    }
    return snap;
  };

  octo::Handler base = octo::registry_handler(registry, assemble);
  const std::string tag =
      "octomancer " + std::to_string(octo::kProtocolVersion) + "\n";

  // `name <id> <what to call it>` and `refresh <id>`.
  //
  // The identifier is taken verbatim up to the first space, and everything
  // after it is the name -- spaces and all, because "B camera" is what
  // somebody will type and quoting rules are a thing to get wrong at both
  // ends. That works because no identifier this program deals in contains a
  // space: a CoreBluetooth UUID is hex and dashes, a hardware address is hex
  // and colons.
  auto name_command = [&](const std::string& command) -> std::string {
    const size_t sp = command.find(' ', 5);
    const std::string id =
        sp == std::string::npos ? command.substr(5) : command.substr(5, sp - 5);
    if (id.empty()) {
      return tag + "error name needs a device id\n";
    }
    const std::string want =
        sp == std::string::npos ? std::string() : command.substr(sp + 1);
    names.rename(id, want);
    if (!opt.devices_path.empty()) {
      save_devices(registry, names, opt.devices_path, opt.quiet);
    }
    // Written through immediately rather than at the next save timer, for the
    // same reason `forget` is: it is a decision somebody made, and a decision
    // that did not survive a restart reads as the program ignoring them.
    return tag + (want.empty() ? "unnamed " : "named ") + octo::escape(id) +
           (want.empty() ? std::string() : " " + octo::escape(want)) + "\n";
  };

  auto refresh_command = [&](const std::string& command) -> std::string {
    const std::string id = command.substr(8);
    if (id.empty()) {
      return tag + "error refresh needs a device id\n";
    }
    // "Not here" is success, exactly as it is for `forget`: the caller asked
    // for what we know about the device to be dropped, and it is.
    const bool had = names.refresh(id);
    if (!opt.devices_path.empty()) {
      save_devices(registry, names, opt.devices_path, opt.quiet);
    }
    return tag + "refreshed " + octo::escape(id) +
           (had ? "" : " (nothing was remembered)") + "\n";
  };

  octo::Server server(
      [&](const std::string& command) {
        if (command.compare(0, 5, "name ") == 0) return name_command(command);
        if (command.compare(0, 8, "refresh ") == 0) {
          return refresh_command(command);
        }
        const std::string reply = base(command);
        if (!opt.devices_path.empty() && command.compare(0, 7, "forget ") == 0) {
          save_devices(registry, names, opt.devices_path, opt.quiet);
        }
        return reply;
      },
      opt.socket_path);
  if (!server.start(&err)) {
    std::fprintf(stderr, "octomancerd: %s\n", err.c_str());
    scanner->stop();
    return 1;
  }
  if (!opt.quiet) {
    std::fprintf(stderr, "octomancerd %s listening on %s\n", OCTO_VERSION,
                 server.path().c_str());
  }

  {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "\"socket\":\"%s\",\"radio\":\"%s\","
                  "\"alert_threshold_s\":%.1f,"
                  "\"alert_clear_s\":%.1f,\"window_s\":%.0f",
                  json_escape(server.path()).c_str(),
                  json_escape(octo::describe_radio()).c_str(),
                  opt.policy.alert_enter, opt.policy.alert_exit,
                  opt.policy.window);
    log.record("start", buf);
  }

  // A camera coming up is the one thing here worth reporting the instant it
  // happens rather than at the next summary: it is what octomancer-sync waits
  // for, and it is when a power cycle -- and so a reset clock -- would have
  // happened.
  bool camera_was_present = false;
  uint64_t camera_sessions_seen = 0;

  double next_log = octo::mono_now() + opt.log_interval;
  double next_save = octo::mono_now() + kSaveInterval;
  const double started_mono = octo::mono_now();
  bool warned_no_radio = false;
  while (!g_stop) {
    server.serve(200);
    // After the wait, so a dongle's answer is in before anything reads the
    // snapshot -- the same reason bridge.drain() is where it is.
    if (box) box->pump(octo::mono_now());
    // Before anything reads the registry, and after the wait that is where
    // the time goes. This program still runs its own loop rather than the
    // one in src/loop.h, so it drains the bridge by hand; the sync daemon
    // lets the loop do it.
    bridge.drain();
    console.maybe_rotate();

    {
      const octo::Snapshot now = registry.snapshot();
      if (now.camera.seen &&
          (now.camera.present != camera_was_present ||
           now.camera.sessions != camera_sessions_seen)) {
        const bool up = now.camera.present;
        camera_was_present = up;
        camera_sessions_seen = now.camera.sessions;
        if (log.enabled()) {
          char buf[512];
          std::snprintf(buf, sizeof buf,
                        "\"state\":\"%s\",\"id\":\"%s\",\"name\":\"%s\","
                        "\"rssi\":%d,\"sessions\":%lld",
                        up ? "up" : "down", json_escape(now.camera.id).c_str(),
                        json_escape(now.camera.name).c_str(), now.camera.rssi,
                        static_cast<long long>(now.camera.sessions));
          log.record("camera", buf);
        }
        if (!opt.quiet) {
          std::fprintf(stderr, "octomancerd: camera %s%s%s (session %lld)\n",
                       up ? "up" : "down",
                       now.camera.name.empty() ? "" : " -- ",
                       now.camera.name.c_str(),
                       static_cast<long long>(now.camera.sessions));
        }
      }
    }

    for (const octo::AlertEvent& event : registry.take_events()) {
      if (log.enabled()) log_alert(&log, event);
      if (!opt.quiet) {
        std::fprintf(stderr, "octomancerd: %s is %+.1fs off this Mac (%s)\n",
                     event.name.c_str(), event.offset,
                     event.entering ? "drifted" : "recovered");
      }
      run_notify(opt.notify_command, event, opt.policy.alert_enter);
    }

    if (log.enabled() && opt.log_interval > 0 && octo::mono_now() >= next_log) {
      next_log = octo::mono_now() + opt.log_interval;
      log_snapshot(&log, registry.snapshot());
    }

    // A radio that never says anything is the failure with no symptom.
    //
    // CoreBluetooth reports "unauthorized" when it knows the answer is no, and
    // that is handled where the state arrives. What it does under a missing
    // grant is worse: it never calls back at all. No error, no prompt, no
    // state -- and under launchd there is nobody to prompt, so the daemon sits
    // there looking healthy and hearing nothing, which is indistinguishable
    // from an empty room until somebody thinks to check.
    //
    // So it is timed. Ten seconds is far longer than a working radio takes and
    // far shorter than somebody's patience.
    if (!warned_no_radio && registry.snapshot().radio == "unknown" &&
        octo::mono_now() - started_mono > kRadioSilentAfter) {
      warned_no_radio = true;
      if (log.enabled()) {
        log.record("radio", "\"state\":\"silent\"");
      }
      if (!opt.quiet) {
        std::fprintf(stderr,
                     "octomancerd: the radio has not reported a state after"
                     " %.0fs.\n  On macOS that is what a missing Bluetooth"
                     " permission looks like -- there is no prompt and no"
                     " error.\n  Approve this program in System Settings >"
                     " Privacy & Security > Bluetooth.\n",
                     kRadioSilentAfter);
      }
    }

    // On a timer rather than on every change. The roster changes on every
    // advertisement -- five boxes at 2 Hz is ten rewrites a second -- and
    // nothing in it is worth an fsync at that rate: the cost of losing the
    // last half-minute is that a box switched off in that window reads as
    // last-seen half a minute earlier than it was.
    if (!opt.devices_path.empty() && octo::mono_now() >= next_save) {
      next_save = octo::mono_now() + kSaveInterval;
      save_devices(registry, names, opt.devices_path, opt.quiet);
    }
  }

  if (!opt.quiet) std::fprintf(stderr, "octomancerd: stopping\n");
  // Once more on the way out, so an orderly stop does not lose the last
  // interval. A kill -9 still does, which is what the timer above is for.
  if (!opt.devices_path.empty()) {
    save_devices(registry, names, opt.devices_path, opt.quiet);
  }
  log.record("stop", "");
  server.shutdown();
  scanner->stop();
  return 0;
}
