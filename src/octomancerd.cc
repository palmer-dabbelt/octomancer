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
#include <cstdio>
#include <cstdlib>
#include <string>

#include "client.h"
#include "jsonlog.h"
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
  double log_interval = 60.0;
  double probe_seconds = 0.0;
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
      {"radio", required_argument, nullptr, kRadio},
      {"dongle", required_argument, nullptr, kDongle},
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

  octo::Registry registry(opt.policy);
  std::string err;

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

  octo::Server server(registry, opt.socket_path);
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
  while (!g_stop) {
    server.serve(200);
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
  }

  if (!opt.quiet) std::fprintf(stderr, "octomancerd: stopping\n");
  log.record("stop", "");
  server.shutdown();
  scanner->stop();
  return 0;
}
