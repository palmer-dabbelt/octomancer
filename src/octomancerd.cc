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
#include "config.h"

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
#include "registry.h"
#include "render.h"
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
  std::string log_path;
  std::string notify_command;
  double log_interval = 60.0;
  double probe_seconds = 0.0;
  bool foreground = false;
  bool quiet = false;
  octo::Policy policy;
};

void usage(FILE* out) {
  std::fprintf(out,
      "usage: octomancerd [options]\n"
      "\n"
      "Watch Tentacle Sync boxes over BLE and serve their state on a socket.\n"
      "\n"
      "  --socket PATH         control socket (default %s)\n"
      "  --log PATH            append JSONL observations here\n"
      "  --log-interval SEC    how often to log a summary (default 60)\n"
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
      "  --version, --help\n",
      octo::default_socket_path().c_str());
}

bool parse_args(int argc, char** argv, Options* opt) {
  enum {
    kSocket = 1000, kLog, kLogInterval, kProbe, kForeground, kQuiet,
    kThreshold, kClear, kConfirm, kRenotify, kNotify,
    kWindow, kStale, kDriftSpan, kVersion, kHelp,
  };
  static const struct option longs[] = {
      {"socket", required_argument, nullptr, kSocket},
      {"log", required_argument, nullptr, kLog},
      {"log-interval", required_argument, nullptr, kLogInterval},
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
      {"version", no_argument, nullptr, kVersion},
      {"help", no_argument, nullptr, kHelp},
      {nullptr, 0, nullptr, 0},
  };

  for (;;) {
    const int c = getopt_long(argc, argv, "", longs, nullptr);
    if (c == -1) break;
    switch (c) {
      case kSocket: opt->socket_path = optarg; break;
      case kLog: opt->log_path = optarg; break;
      case kLogInterval: opt->log_interval = std::atof(optarg); break;
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
      case kVersion:
        std::printf("octomancerd %s\n", PACKAGE_VERSION);
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
  log->record("bench", fields);
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
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

  octo::Registry registry(opt.policy);
  octo::JsonLog log;
  std::string err;
  if (!log.open(opt.log_path, &err)) {
    std::fprintf(stderr, "octomancerd: %s\n", err.c_str());
    return 1;
  }

  auto scanner = octo::make_ble_scanner(
      [&registry](const octo::Advert& a) {
        registry.observe(a.id, a.name, a.rssi, a.data.data(), a.data.size(),
                         a.mono, a.wall);
      },
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
    }
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
    std::fprintf(stderr, "octomancerd %s listening on %s\n", PACKAGE_VERSION,
                 server.path().c_str());
  }

  {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "\"socket\":\"%s\",\"alert_threshold_s\":%.1f,"
                  "\"alert_clear_s\":%.1f,\"window_s\":%.0f",
                  json_escape(server.path()).c_str(), opt.policy.alert_enter,
                  opt.policy.alert_exit, opt.policy.window);
    log.record("start", buf);
  }

  double next_log = octo::mono_now() + opt.log_interval;
  while (!g_stop) {
    server.serve(200);

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
