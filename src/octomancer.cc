// octomancer -- the one command a person runs.
//
// Everything here is a question or an instruction put to the running daemon
// over its control socket. This program never touches a radio and never opens
// the camera database; if the daemon is not running it says so and stops,
// rather than quietly doing the daemon's job differently.
//
// Instructions are asynchronous by nature -- correcting a clock means finding
// a camera, connecting, waiting for a frame, writing on a second boundary and
// verifying, which is tens of seconds. So the daemon takes the request, hands
// back an id, and this program asks after it until it is finished. That keeps
// the socket to one command, one reply, close, and it means a client that is
// killed mid-sync does not abort the sync.
#include <getopt.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "bmd.h"
#include "client.h"
#include "control.h"
#include "proto.h"
#include "timeutil.h"

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Options {
  std::string socket_path = octo::default_control_socket_path();
  std::vector<std::string> cameras;
  double timeout = 180.0;
  bool json = false;
  bool color = false;
  bool wait = true;
};

void usage(FILE* out) {
  std::fprintf(out,
      "usage: octomancer [options] <command> [arguments]\n"
      "\n"
      "  status                what the daemon, the bench and the cameras are\n"
      "                        doing (the default)\n"
      "  list-cameras          one line per camera the daemon knows about\n"
      "  sync                  correct the clock now, even if it looks fine\n"
      "  source [MODE]         report or set the camera's timecode source.\n"
      "                        MODE is `time-of-day` (the timecode follows the\n"
      "                        camera's clock, which is what lets it be\n"
      "                        synced) or `clip` (it parks at 00:00:00:00 and\n"
      "                        stops). 0 and 1 work too.\n"
      "\n"
      "  --camera ID|NAME      which camera, repeatable. Without it, sync and\n"
      "                        source act on whichever camera the daemon is\n"
      "                        following.\n"
      "  --socket PATH         the daemon's control socket (default %s)\n"
      "  --timeout SEC         how long to wait for a request (default 180)\n"
      "  --no-wait             queue the request and exit without waiting\n"
      "  --json                machine-readable output where there is any\n"
      "  --no-color            plain text even on a terminal\n"
      "  --version, --help\n"
      "\n"
      "The daemon is octomancer-sync. If nothing answers, start it with\n"
      "`make install-agent`, or run it in a terminal to watch it work.\n",
      octo::default_control_socket_path().c_str());
}

// ------------------------------------------------------------------ output

const char* kReset = "\033[0m";
const char* kDim = "\033[2m";
const char* kBold = "\033[1m";
const char* kRed = "\033[31m";
const char* kGreen = "\033[32m";
const char* kYellow = "\033[33m";

struct Paint {
  bool on = false;
  const char* operator()(const char* code) const { return on ? code : ""; }
};

std::string clock_of(double wall) {
  if (wall <= 0.0) return "--";
  const time_t secs = static_cast<time_t>(wall);
  struct tm tm_local;
  localtime_r(&secs, &tm_local);
  char buf[16];
  std::strftime(buf, sizeof buf, "%H:%M:%S", &tm_local);
  return buf;
}

std::string ago(double wall, double now) {
  if (wall <= 0.0) return "never";
  const double d = now - wall;
  if (d < 90.0) return std::to_string(static_cast<int>(d)) + "s ago";
  if (d < 5400.0) return std::to_string(static_cast<int>(d / 60.0)) + "m ago";
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1fh ago", d / 3600.0);
  return buf;
}

// The two modes 4.7 has, spelled out. This is the parameter most likely to be
// the reason a camera is not syncing, so it is never rendered as a bare digit.
std::string source_words(bool has, int64_t value) {
  if (!has) return "unknown";
  if (value == octo::bmd::kTimecodeSourceTimeOfDay) return "time-of-day";
  if (value == octo::bmd::kTimecodeSourceClip) return "clip (does not follow the clock)";
  return "unrecognised (" + std::to_string(value) + ")";
}

void print_camera_line(const octo::CameraStatus& c, const Paint& p,
                       double now) {
  const bool healthy = c.has_error && std::fabs(c.error_s) < 0.05;
  const char* dot_color = !c.present ? kDim : (healthy ? kGreen : kYellow);
  std::printf("  %s%s%s %s%-22s%s %-38s\n", p(dot_color),
              c.present ? "*" : "-", p(kReset), p(kBold),
              (c.name.empty() ? "(unnamed)" : c.name).c_str(), p(kReset),
              c.id.c_str());

  std::printf("      %-12s %s\n", "timecode",
              c.timecode.empty() ? "--" : c.timecode.c_str());
  if (c.has_error) {
    std::printf("      %-12s %s%+.3fs%s\n", "off by",
                p(healthy ? kGreen : kYellow), c.error_s, p(kReset));
  }
  if (c.has_fps) std::printf("      %-12s %d\n", "rate", c.fps);
  std::printf("      %-12s %s%s%s\n", "tc source",
              p(c.has_source && c.source != octo::bmd::kTimecodeSourceTimeOfDay
                    ? kRed
                    : ""),
              source_words(c.has_source, c.source).c_str(), p(kReset));
  if (c.recording) {
    std::printf("      %-12s %sRECORDING -- the clock will not be touched%s\n",
                "transport", p(kRed), p(kReset));
  }
  if (!c.action.empty()) {
    std::printf("      %-12s %s\n", "last cycle", c.action.c_str());
  }
  std::printf("      %-12s %s", "last write",
              ago(c.has_last_write ? c.last_write_wall : 0.0, now).c_str());
  if (c.writes > 0) std::printf(" (%d this session)", c.writes);
  std::printf("\n");
  if (c.has_lead) {
    std::printf("      %-12s %.0fms\n", "send lead", c.lead_s * 1000.0);
  }
  if (c.has_drift) {
    std::printf("      %-12s %+.1f ppm\n", "drift", c.drift_ppm);
  }
}

void print_status(const octo::Status& s, const Paint& p) {
  const double now = s.daemon.now_wall > 0.0 ? s.daemon.now_wall : octo::wall_now();

  std::printf("%soctomancer%s %s -- up since %s%s\n", p(kBold), p(kReset),
              s.daemon.version.empty() ? "?" : s.daemon.version.c_str(),
              clock_of(s.daemon.started_wall).c_str(),
              s.daemon.dry_run ? ", DRY RUN (will not write)" : "");

  if (s.bench.has) {
    std::printf("  bench        %d box%s, %+.3fs, spread %.3fs (%s)\n",
                s.bench.boxes, s.bench.boxes == 1 ? "" : "es",
                s.bench.offset_s, s.bench.spread_s,
                s.bench.daemon_reachable ? "via octomancerd" : "own scan");
  } else {
    std::printf("  bench        %snothing heard yet%s\n", p(kDim), p(kReset));
  }
  if (s.queued > 0) {
    std::printf("  queued       %d request%s waiting\n", s.queued,
                s.queued == 1 ? "" : "s");
  }
  std::printf("\n");

  if (s.cameras.empty()) {
    std::printf("  %sno cameras seen yet%s\n", p(kDim), p(kReset));
    return;
  }
  for (const octo::CameraStatus& c : s.cameras) {
    print_camera_line(c, p, now);
    std::printf("\n");
  }
}

void print_camera_list(const octo::Status& s, const Paint& p) {
  if (s.cameras.empty()) {
    std::fprintf(stderr, "no cameras seen yet\n");
    return;
  }
  for (const octo::CameraStatus& c : s.cameras) {
    std::printf("%s%-6s%s %-22s %-38s %s\n",
                p(c.present ? kGreen : kDim), c.present ? "on" : "off",
                p(kReset), (c.name.empty() ? "(unnamed)" : c.name).c_str(),
                c.id.c_str(), source_words(c.has_source, c.source).c_str());
  }
}

// ----------------------------------------------------------------- requests

// Queue one request and, unless told not to, watch it to the end.
//
// The poll is deliberately slow. A sync takes tens of seconds and every ask
// wakes the daemon's socket thread; a tight loop would buy nothing but load.
int run_request(const Options& opt, const std::string& command) {
  std::string reply, err;
  if (!octo::query(opt.socket_path, command, &reply, &err, 5.0)) {
    std::fprintf(stderr, "octomancer: %s\n", err.c_str());
    return 1;
  }
  octo::RequestResult result;
  if (!octo::parse_result(reply, &result, &err)) {
    std::fprintf(stderr, "octomancer: %s\n", err.c_str());
    return 1;
  }
  if (!opt.wait) {
    std::printf("queued as request %lld\n", static_cast<long long>(result.id));
    return 0;
  }

  const double deadline = octo::mono_now() + opt.timeout;
  std::string shown;
  while (!g_stop) {
    if (octo::request_finished(result.state)) break;
    if (octo::mono_now() > deadline) {
      std::fprintf(stderr,
                   "octomancer: still %s after %.0fs -- giving up watching."
                   " The daemon has not dropped it; `octomancer status` will"
                   " show when it lands.\n",
                   octo::request_state_name(result.state), opt.timeout);
      return 2;
    }
    // Say what it is doing, once per transition, so a sync that is scanning
    // for twenty seconds does not look like a hang.
    const std::string state = octo::request_state_name(result.state);
    if (state != shown) {
      std::fprintf(stderr, "%s...\n", state.c_str());
      shown = state;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const std::string ask = "result id=" + std::to_string(result.id);
    if (!octo::query(opt.socket_path, ask, &reply, &err, 5.0)) {
      std::fprintf(stderr, "octomancer: %s\n", err.c_str());
      return 1;
    }
    if (!octo::parse_result(reply, &result, &err)) {
      std::fprintf(stderr, "octomancer: %s\n", err.c_str());
      return 1;
    }
  }
  if (g_stop) {
    std::fprintf(stderr,
                 "octomancer: interrupted. The daemon is still working on"
                 " request %lld.\n", static_cast<long long>(result.id));
    return 130;
  }

  if (result.state == octo::RequestState::kDone) {
    std::printf("%s\n", result.message.empty() ? "done"
                                               : result.message.c_str());
    return 0;
  }
  std::fprintf(stderr, "octomancer: %s\n",
               result.message.empty() ? "the request did not succeed"
                                      : result.message.c_str());
  return 1;
}

std::string camera_args(const Options& opt) {
  std::string out;
  for (const std::string& c : opt.cameras) {
    out += " camera=" + octo::escape(c);
  }
  return out;
}

int fetch_status(const Options& opt, octo::Status* out) {
  std::string reply, err;
  if (!octo::query(opt.socket_path, "status", &reply, &err, 5.0)) {
    std::fprintf(stderr,
                 "octomancer: %s\n"
                 "  Is octomancer-sync running? Start it with"
                 " `make install-agent`.\n",
                 err.c_str());
    return 1;
  }
  if (!octo::parse_status(reply, out, &err)) {
    std::fprintf(stderr, "octomancer: %s\n", err.c_str());
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  opt.color = isatty(1);

  enum {
    kCamera = 1000, kSocket, kTimeout, kJson, kNoColor, kNoWait,
    kVersion, kHelp,
  };
  static const struct option longs[] = {
      {"camera", required_argument, nullptr, kCamera},
      {"socket", required_argument, nullptr, kSocket},
      {"timeout", required_argument, nullptr, kTimeout},
      {"json", no_argument, nullptr, kJson},
      {"no-color", no_argument, nullptr, kNoColor},
      {"no-wait", no_argument, nullptr, kNoWait},
      {"version", no_argument, nullptr, kVersion},
      {"help", no_argument, nullptr, kHelp},
      {nullptr, 0, nullptr, 0},
  };
  for (;;) {
    const int c = getopt_long(argc, argv, "", longs, nullptr);
    if (c == -1) break;
    switch (c) {
      case kCamera: opt.cameras.push_back(optarg); break;
      case kSocket: opt.socket_path = optarg; break;
      case kTimeout: opt.timeout = std::atof(optarg); break;
      case kJson: opt.json = true; break;
      case kNoColor: opt.color = false; break;
      case kNoWait: opt.wait = false; break;
      case kVersion: std::printf("octomancer %s\n", OCTO_VERSION); return 0;
      case kHelp: usage(stdout); return 0;
      default: usage(stderr); return 2;
    }
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  Paint paint;
  paint.on = opt.color;

  const std::string command = optind < argc ? argv[optind] : "status";
  const std::string argument =
      optind + 1 < argc ? argv[optind + 1] : std::string();

  if (command == "status" || command == "list-cameras") {
    if (opt.json) {
      std::string reply, err;
      if (!octo::query(opt.socket_path, "json", &reply, &err, 5.0)) {
        std::fprintf(stderr, "octomancer: %s\n", err.c_str());
        return 1;
      }
      std::fputs(reply.c_str(), stdout);
      return 0;
    }
    octo::Status s;
    const int rc = fetch_status(opt, &s);
    if (rc != 0) return rc;
    if (command == "status") {
      print_status(s, paint);
    } else {
      print_camera_list(s, paint);
    }
    return 0;
  }

  if (command == "sync") {
    return run_request(opt, "sync" + camera_args(opt));
  }

  if (command == "source") {
    if (argument.empty()) {
      // No argument is a question, not a malformed instruction.
      octo::Status s;
      const int rc = fetch_status(opt, &s);
      if (rc != 0) return rc;
      if (s.cameras.empty()) {
        std::fprintf(stderr, "no cameras seen yet\n");
        return 1;
      }
      for (const octo::CameraStatus& c : s.cameras) {
        if (!opt.cameras.empty()) {
          bool wanted = false;
          for (const std::string& want : opt.cameras) {
            if (c.id == want || c.name == want) wanted = true;
          }
          if (!wanted) continue;
        }
        std::printf("%-22s %s\n",
                    (c.name.empty() ? c.id : c.name).c_str(),
                    source_words(c.has_source, c.source).c_str());
      }
      return 0;
    }

    int64_t value = 0;
    if (argument == "time-of-day" || argument == "timeofday" ||
        argument == "tod" || argument == "0") {
      value = octo::bmd::kTimecodeSourceTimeOfDay;
    } else if (argument == "clip" || argument == "1") {
      value = octo::bmd::kTimecodeSourceClip;
    } else {
      std::fprintf(stderr,
                   "octomancer: unknown timecode source '%s'."
                   " Use `time-of-day` or `clip`.\n", argument.c_str());
      return 2;
    }
    if (value == octo::bmd::kTimecodeSourceClip) {
      // Worth saying out loud: this is the setting that makes the camera
      // unsyncable, and someone typing it may not know that yet.
      std::fprintf(stderr,
                   "note: in `clip` the timecode parks at 00:00:00:00 and stops"
                   " following the camera's clock, so octomancer cannot sync it"
                   " until it is set back to `time-of-day`.\n");
    }
    return run_request(opt, "source value=" + std::to_string(value) +
                                camera_args(opt));
  }

  if (command == "ping") {
    std::string reply, err;
    if (!octo::query(opt.socket_path, "ping", &reply, &err, 5.0)) {
      std::fprintf(stderr, "octomancer: %s\n", err.c_str());
      return 1;
    }
    std::printf("ok\n");
    return 0;
  }

  std::fprintf(stderr, "octomancer: unknown command '%s'\n", command.c_str());
  usage(stderr);
  return 2;
}
