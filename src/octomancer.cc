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
#include <errno.h>
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

#include "agents.h"
#include "bmd.h"
#include "camconf.h"
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
  bool all_cameras = false;
  std::string daemon = "all";
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
      "  scan                  look for Blackmagic cameras on the air. With\n"
      "                        --all, every LE device seen, which is how you\n"
      "                        tell a silent camera from a deaf radio.\n"
      "  pair                  bond with a camera, so that its encrypted\n"
      "                        control characteristics will answer. The camera\n"
      "                        displays a six-digit code and macOS asks for\n"
      "                        that number. Needed once, and again whenever\n"
      "                        either side forgets the other.\n"
      "  sync                  correct the clock now, even if it looks fine\n"
      "  start | stop | restart\n"
      "                        the daemons themselves. These go through\n"
      "                        launchd rather than a socket, for the obvious\n"
      "                        reason. `start` installs the LaunchAgent if it\n"
      "                        is not there yet, so it also survives a reboot.\n"
      "  writes [on|off]       whether octomancer may change a camera at\n"
      "                        all -- its clock and its timecode source. New\n"
      "                        cameras are off, so this is the first thing to\n"
      "                        run. With no argument, reports the file.\n"
      "  reload                re-read the camera configuration, for when it\n"
      "                        has been edited by hand\n"
      "  source [MODE]         report or set the camera's timecode source.\n"
      "                        MODE is `time-of-day` (the timecode follows the\n"
      "                        camera's clock, which is what lets it be\n"
      "                        synced) or `clip` (it parks at 00:00:00:00 and\n"
      "                        stops). 0 and 1 work too.\n"
      "\n"
      "  --all                 every camera the daemon knows about. Only\n"
      "                        writes takes it, and it is required there if\n"
      "                        --camera is not given.\n"
      "  --camera ID|NAME      which camera, repeatable. Without it, sync and\n"
      "                        source act on whichever camera the daemon is\n"
      "                        following.\n"
      "  --daemon WHICH        `all` (the default), `bench` for octomancerd,\n"
      "                        or `sync` for octomancer-sync. Only start,\n"
      "                        stop and restart look at this.\n"
      "  --socket PATH         the daemon's control socket (default %s)\n"
      "  --timeout SEC         how long to wait for a request (default 180)\n"
      "  --no-wait             queue the request and exit without waiting\n"
      "  --json                machine-readable output where there is any\n"
      "  --no-color            plain text even on a terminal\n"
      "  --version, --help\n"
      "\n"
      "The daemon is octomancer-sync. If nothing answers: `octomancer start`.\n"
      "Run it in a terminal instead to watch it work.\n",
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

// Same, but for the places that can carry on without a daemon -- writing the
// configuration file does not need one.
bool fetch_status_quiet(const Options& opt, octo::Status* out) {
  std::string reply, err;
  if (!octo::query(opt.socket_path, "status", &reply, &err, 3.0)) return false;
  return octo::parse_status(reply, out, &err);
}

int fetch_status(const Options& opt, octo::Status* out) {
  std::string reply, err;
  if (!octo::query(opt.socket_path, "status", &reply, &err, 5.0)) {
    std::fprintf(stderr,
                 "octomancer: %s\n"
                 "  Is octomancer-sync running? `octomancer start` will"
                 " start it.\n",
                 err.c_str());
    return 1;
  }
  if (!octo::parse_status(reply, out, &err)) {
    std::fprintf(stderr, "octomancer: %s\n", err.c_str());
    return 1;
  }
  return 0;
}

// -------------------------------------------------------------- the daemons

// Start, stop and restart do not go through a socket: a daemon that is not
// running cannot be asked to start. They go to launchd, and they report what
// happened per agent rather than as one verdict, because "one of the two came
// up" is a different situation from either "both did" or "neither did".
int run_agent_command(const Options& opt, const std::string& verb,
                      const Paint& p) {
  std::vector<octo::Agent> which;
  if (!octo::parse_agent_selection(opt.daemon, &which)) {
    std::fprintf(stderr,
                 "octomancer: unknown daemon '%s'. Use `all`, `bench` or"
                 " `sync`.\n", opt.daemon.c_str());
    return 2;
  }

  int failures = 0;
  for (const octo::Agent a : which) {
    std::string err;
    bool ok = false;
    if (verb == "start") {
      ok = octo::agent_start(a, &err);
    } else if (verb == "stop") {
      ok = octo::agent_stop(a, &err);
    } else {
      ok = octo::agent_restart(a, &err);
    }

    if (!ok) {
      ++failures;
      std::fprintf(stderr, "%s%-22s%s %s\n", p(kRed), octo::agent_label(a),
                   p(kReset), err.c_str());
      continue;
    }

    // Report what is actually true afterwards rather than that the command
    // was accepted. launchctl exits 0 for a great many things that do not end
    // with a running process.
    const octo::AgentState state = octo::agent_state(a);
    const char* colour = kGreen;
    std::string said;
    if (verb == "stop") {
      said = state.loaded ? "still loaded" : "stopped";
      if (state.loaded) colour = kYellow;
    } else if (state.running) {
      said = "running, pid " + std::to_string(state.pid);
    } else if (state.loaded) {
      // launchd has it but there is no process. Usually it just exited, and
      // the exit code is the only thing that explains why.
      said = "loaded, but not running";
      if (state.last_exit != 0) {
        said += " (last exit " + std::to_string(state.last_exit) + ")";
      }
      colour = kYellow;
    } else {
      said = "not loaded";
      colour = kYellow;
    }
    std::printf("%s%-22s%s %s  %s(%s)%s\n", p(colour), octo::agent_label(a),
                p(kReset), said.c_str(), p(kDim), octo::agent_description(a),
                p(kReset));
  }
  return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------- the config file
//
// Written here and never by the daemon, which is the whole point of keeping it
// separate from the daemon's own notebook: a permission cannot quietly become
// something else because a measurement moved.
int run_writes_command(const Options& opt, const std::string& argument,
                       const Paint& p) {
  octo::CamConf conf;
  std::string err;
  if (!conf.load(octo::default_camera_config_path(), &err)) {
    std::fprintf(stderr, "octomancer: %s\n", err.c_str());
    return 1;
  }

  // No argument is a question.
  if (argument.empty()) {
    std::printf("%s%s\n", conf.path().c_str(),
                conf.file_exists() ? "" : "  (does not exist yet)");
    std::printf("  %-38s %s\n", "default",
                conf.default_writes_enabled() ? "on" : "off");
    for (const octo::CameraConfig& c : conf.cameras()) {
      std::printf("  %-38s %s%s%s%s%s\n", c.id.c_str(),
                  p(c.writes_enabled ? kGreen : kDim),
                  c.writes_enabled ? "on" : "off", p(kReset),
                  c.name.empty() ? "" : "  ", c.name.c_str());
    }
    if (!conf.any_writes_enabled()) {
      std::printf("\n%sNo camera may be changed, so nothing will be synced.%s\n"
                  "Enable one with: octomancer writes on --camera <id>\n",
                  p(kYellow), p(kReset));
    }
    return 0;
  }

  bool enable = false;
  if (argument == "on" || argument == "enable" || argument == "yes") {
    enable = true;
  } else if (argument == "off" || argument == "disable" || argument == "no") {
    enable = false;
  } else {
    std::fprintf(stderr, "octomancer: say `on` or `off`, not '%s'\n",
                 argument.c_str());
    return 2;
  }

  // Which cameras. Nothing is assumed: this grants permission to change
  // hardware, and the one camera the daemon happens to know about today is not
  // the one it will know about tomorrow. Say which, or say --all and mean it.
  if (opt.cameras.empty() && !opt.all_cameras) {
    std::fprintf(stderr,
                 "octomancer: say which camera -- `--camera ID|NAME`, or"
                 " `--all` for every camera the daemon knows about.\n"
                 "  `octomancer list-cameras` shows them, and `octomancer"
                 " writes` shows what is set now.\n");
    return 2;
  }
  if (!opt.cameras.empty() && opt.all_cameras) {
    std::fprintf(stderr,
                 "octomancer: --all and --camera contradict each other.\n");
    return 2;
  }

  std::vector<std::pair<std::string, std::string>> targets;  // id, name
  if (opt.all_cameras) {
    octo::Status s;
    if (!fetch_status_quiet(opt, &s) || s.cameras.empty()) {
      std::fprintf(stderr,
                   "octomancer: --all is every camera the daemon knows about,"
                   " and it knows about none yet."
                   " Name one with --camera instead.\n");
      return 2;
    }
    for (const octo::CameraStatus& c : s.cameras) {
      targets.emplace_back(c.id, c.name);
    }
  } else {
    octo::Status s;
    const bool have_status = fetch_status_quiet(opt, &s);
    for (const std::string& want : opt.cameras) {
      std::string id = want, name;
      if (have_status) {
        for (const octo::CameraStatus& c : s.cameras) {
          if (c.id == want || c.name == want) {
            id = c.id;
            name = c.name;
            break;
          }
        }
      }
      targets.emplace_back(id, name);
    }
  }

  for (const auto& target : targets) {
    if (!conf.set_writes(target.first, target.second, enable, &err)) {
      std::fprintf(stderr, "octomancer: %s\n", err.c_str());
      return 1;
    }
    std::printf("%s%s%s writes %s\n", p(enable ? kGreen : kYellow),
                (target.second.empty() ? target.first : target.second).c_str(),
                p(kReset), enable ? "enabled" : "disabled");
  }
  std::printf("saved to %s\n", conf.path().c_str());

  // Tell the running daemon, if there is one. Not being able to is not a
  // failure: the file is written, and the daemon will read it when it starts.
  std::string reply, ignored;
  if (octo::query(opt.socket_path, "reload", &reply, &ignored, 3.0)) {
    std::printf("the running daemon has been told to re-read it\n");
  } else {
    std::printf("no daemon answered, so this takes effect when one starts\n");
  }
  return 0;
}

void print_agent_states(const Paint& p) {
  for (const octo::Agent a : {octo::Agent::kBench, octo::Agent::kSync}) {
    const octo::AgentState state = octo::agent_state(a);
    const char* colour = state.running ? kGreen : kYellow;
    std::string said;
    if (state.running) {
      said = "running, pid " + std::to_string(state.pid);
    } else if (state.loaded) {
      said = "loaded, not running";
    } else if (state.installed) {
      said = "installed, not loaded";
    } else {
      said = "not installed";
    }
    if (state.installed) said += ", starts at boot";
    std::printf("  %-22s %s%s%s\n", octo::agent_program(a), p(colour),
                said.c_str(), p(kReset));
  }
}

// `scan` and `pair` need a radio, and this program has none: Makefile.am
// explains why the front door links no CoreBluetooth. Both are handed to
// octomancer-sync, which has one, and which already holds the Bluetooth grant
// a second binary would have to be given separately -- having first been
// refused it in the silent way src/camera.h describes.
//
// This replaces the process rather than waiting on a child. Pairing is
// interactive: it wants the terminal, and it wants a Ctrl-C to arrive at the
// program doing the waiting rather than at a parent that would then have to
// think about forwarding it.
int exec_radio_program(std::vector<std::string> args) {
  const std::string program = octo::sibling_program_path("octomancer-sync");

  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(program.c_str()));
  for (std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);

  execv(program.c_str(), argv.data());
  // execv returns only on failure, and the one thing it will not try is PATH.
  execvp("octomancer-sync", argv.data());
  std::fprintf(stderr, "octomancer: could not run %s: %s\n", program.c_str(),
               std::strerror(errno));
  return 1;
}

// The sync daemon connects to the camera whenever it notices one, and a BLE
// peripheral takes a single connection at a time. Pairing alongside it
// usually works, because it spends most of its life not connected -- but when
// it does not work it fails as "could not connect", which is the least
// informative outcome of the four and the easiest to read as the wrong thing.
void warn_if_sync_daemon_may_hold_the_camera() {
  const octo::AgentState state = octo::agent_state(octo::Agent::kSync);
  if (!state.running) return;
  std::fprintf(stderr,
               "note: octomancer-sync is running (pid %d) and connects to the"
               " camera on its own.\n"
               "      If this cannot get a connection, stop it and try again:\n"
               "        octomancer stop --daemon sync\n\n",
               state.pid);
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  opt.color = isatty(1);

  enum {
    kCamera = 1000, kAllCameras, kSocket, kTimeout, kJson, kNoColor, kNoWait,
    kDaemon, kVersion, kHelp,
  };
  static const struct option longs[] = {
      {"camera", required_argument, nullptr, kCamera},
      {"all", no_argument, nullptr, kAllCameras},
      {"daemon", required_argument, nullptr, kDaemon},
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
      case kAllCameras: opt.all_cameras = true; break;
      case kDaemon: opt.daemon = optarg; break;
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
    if (command == "list-cameras") {
      if (rc != 0) return rc;
      print_camera_list(s, paint);
      return 0;
    }
    // A status that cannot reach the daemon is still worth printing: whether
    // the agents are loaded is exactly the question somebody has at that
    // moment, and it does not come from the socket.
    if (rc != 0) {
      std::fprintf(stderr, "\n");
      print_agent_states(paint);
      return rc;
    }
    print_status(s, paint);
    std::printf("  daemons\n");
    print_agent_states(paint);
    return 0;
  }

  if (command == "start" || command == "stop" || command == "restart") {
    return run_agent_command(opt, command, paint);
  }

  if (command == "writes") {
    return run_writes_command(opt, argument, paint);
  }

  if (command == "reload") {
    std::string reply, err;
    if (!octo::query(opt.socket_path, "reload", &reply, &err, 5.0)) {
      std::fprintf(stderr, "octomancer: %s\n", err.c_str());
      return 1;
    }
    // The daemon re-reads between cycles, not on the socket thread, so this
    // says what was asked for rather than what has happened.
    std::printf("asked the daemon to re-read %s\n",
                octo::default_camera_config_path().c_str());
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

  if (command == "scan") {
    // --log '' because the default writes a JSONL file into whatever
    // directory this was run from, and a scan is a question rather than a
    // session worth leaving a file behind for.
    std::vector<std::string> args = {"--scan-only", "--log", ""};
    if (opt.all_cameras) args.push_back("--all");
    if (!opt.cameras.empty()) {
      args.push_back("--camera");
      args.push_back(opt.cameras.front());
    }
    return exec_radio_program(args);
  }

  if (command == "pair") {
    warn_if_sync_daemon_may_hold_the_camera();
    std::vector<std::string> args = {"--pair", "--log", ""};
    if (!opt.cameras.empty()) {
      args.push_back("--camera");
      args.push_back(opt.cameras.front());
    }
    return exec_radio_program(args);
  }

  std::fprintf(stderr, "octomancer: unknown command '%s'\n", command.c_str());
  usage(stderr);
  return 2;
}
