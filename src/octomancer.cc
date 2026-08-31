// octomancer -- the one command a person runs.
//
// Everything here is a question or an instruction put to a running daemon over
// its socket. There are two of them and they hear different things --
// octomancerd listens to the Tentacle boxes, octomancer-sync connects to the
// cameras -- so `status` asks both and merges the answers; every other command
// belongs to one of them. This program never touches a radio and never opens
// the camera database; if the daemon it needs is not running it says so, rather
// than quietly doing the daemon's job differently.
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
#include "boxcdc.h"
#include "camconf.h"
#include "client.h"
#include "control.h"
#include "devices.h"
#include "hciport.h"
#include "loop.h"
#include "proto.h"
#include "server.h"
#include "timeutil.h"
#include "tui.h"

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Options {
  // Two daemons, two sockets, and they are not interchangeable. `--socket` has
  // always meant octomancer-sync's control socket and keeps meaning that;
  // octomancerd's is a separate flag rather than a mode of the first one,
  // because pointing one at the other fails as a parse error somewhere deep in
  // a reply and reads like a corrupt daemon.
  std::string socket_path = octo::default_control_socket_path();
  std::string bench_socket_path = octo::default_socket_path();
  std::vector<std::string> cameras;
  std::vector<std::string> boxes;
  bool all_cameras = false;
  std::string daemon = "all";
  double timeout = 180.0;
  bool json = false;
  bool color = false;
  bool verbose = false;
  bool wait = true;
};

// ------------------------------------------------------------- the dongle

// This machine's offset from UTC, in seconds east, right now.
//
// The box needs this and cannot work it out: it has no timezone database, and
// a Tentacle broadcasts a *local* time of day. See firmware/src/boxclock.cc.
int utc_offset_now() {
  const std::time_t now = std::time(nullptr);
  struct tm parts;
  ::localtime_r(&now, &parts);
  return static_cast<int>(parts.tm_gmtoff);
}

// Ask the dongle what it can hear, and print it.
//
// The other radio, in other words. doc/box-notes.md's whole experiment is that
// two radios listening to the same room should report the same boxes with
// slightly different offsets, and this is the first half of being able to see
// that: a Mac holding a link open to an nRF52840 running the same sync daemon
// as firmware.
//
// It also sets the box's clock, which is not a courtesy. A box with no wall
// clock records no offsets at all -- src/registry.cc declines to subtract a
// clock it does not have -- so without this the answer is an empty roster and
// no explanation.
int run_dongle(const std::string& device, bool verbose) {
  std::string err;
  std::unique_ptr<octo::hci::Port> port = octo::hci::open_port(device, &err);
  if (port == nullptr) {
    std::fprintf(stderr, "octomancer: %s\n", err.c_str());
    if (octo::hci::no_port_found(err)) {
      std::fprintf(stderr,
                   "no dongle found. A dongle running this firmware appears as"
                   " 'octomancer-sync' on USB; one running anything else does"
                   " not answer this protocol.\n");
    }
    return 1;
  }

  std::unique_ptr<octo::Loop> loop = octo::make_loop();
  const std::string port_name = port->name();
  std::unique_ptr<octo::BoxLink> link =
      octo::BoxLink::attach(loop.get(), std::move(port));
  if (link == nullptr) {
    std::fprintf(stderr, "octomancer: could not attach to %s\n",
                 port_name.c_str());
    return 1;
  }

  // Flushed, because everything after this that goes wrong goes to stderr,
  // and a buffered stdout would print the port name after the complaint about
  // it.
  std::printf("%s\n", port_name.c_str());
  std::fflush(stdout);

  int devices = 0;
  bool greeted = false;
  bool finished = false;
  int exit_code = 1;

  link->on_closed([&](const std::string& why) {
    std::fprintf(stderr, "octomancer: link closed: %s\n", why.c_str());
    finished = true;
  });

  link->on_message([&](const octo::Message& msg) {
    if (msg.verb == "hello") {
      greeted = true;
      std::printf("  %s, protocol %s\n",
                  msg.get("role").empty() ? "?" : msg.get("role").c_str(),
                  msg.get("proto").empty() ? "?" : msg.get("proto").c_str());
      // The clock, before anything is asked of it. Everything the box can say
      // about a Tentacle is measured against this.
      octo::Message set;
      set.verb = "time";
      set.set_double("wall", octo::wall_now(), 3);
      set.set_int("zone", utc_offset_now());
      link->send(set);
      return;
    }

    // A box with no console says what it is doing this way, and at least one
    // of these lines is why the last run ended.
    if (msg.verb == "say") {
      std::printf("  %s\n", msg.get("text").c_str());
      return;
    }

    if (msg.verb == "ok" && msg.get("what") == "time") {
      octo::Message ask;
      ask.verb = "devices";
      ask.set("id", "1");
      link->send(ask);
      return;
    }

    if (msg.verb == "dev") {
      ++devices;
      const std::string name =
          msg.get("name").empty() ? msg.get("id") : msg.get("name");
      double offset = 0.0;
      if (msg.get_double("offset", &offset)) {
        std::printf("  %-24s %+8.3f s\n", name.c_str(), offset);
      } else {
        // No offset is a real answer and a different one from zero: the box
        // heard it and had nothing to measure it against.
        std::printf("  %-24s        -- (no time)\n", name.c_str());
      }
      return;
    }

    // `end`, not `done` -- src/syncd.cc. Worth being exact about: a client that
    // waits for the wrong terminator gets every device, prints them, and then
    // reports a timeout, which reads as a broken dongle rather than a broken
    // client.
    if (msg.verb == "end") {
      std::printf("  %d device%s\n", devices, devices == 1 ? "" : "s");
      exit_code = 0;
      finished = true;
      return;
    }

    if (msg.verb == "err") {
      std::fprintf(stderr, "octomancer: dongle refused: %s\n",
                   msg.get("reason").c_str());
      finished = true;
      return;
    }

    if (verbose) std::printf("  < %s\n", octo::encode(msg).c_str());
  });

  // Bounded, because a dongle that has stopped answering must not hang a
  // command someone typed. Long enough for a box to enumerate a bench.
  loop->every(0.05, [&]() {
    if (finished) loop->stop();
  });
  loop->after(10.0, [&]() {
    if (!finished) {
      if (greeted) {
        std::fprintf(stderr,
                     "octomancer: the dongle greeted us and then went quiet."
                     " That is what a crashed box looks like from here: the"
                     " USB pull-up stays up, so the device is still listed"
                     " while nothing is left running behind it. Reflash it,"
                     " and the next image will say what killed it --"
                     " firmware/src/faultlog.h.\n");
      } else {
        std::fprintf(stderr,
                     "octomancer: no greeting from %s in ten seconds. Either"
                     " it is not running this firmware -- one that is calls"
                     " itself 'octomancer-sync' on USB -- or it is running"
                     " it and has stopped.\n",
                     port_name.c_str());
      }
      loop->stop();
    }
  });
  loop->run();
  return exit_code;
}

void usage(FILE* out) {
  std::fprintf(out,
      "usage: octomancer [options] <command> [arguments]\n"
      "\n"
      "  status                one line per device, plus what the two daemons\n"
      "                        are doing (the default). --verbose for the\n"
      "                        rest of what they know.\n"
      "  tui                   the same page, on screen and staying there: it\n"
      "                        redraws every second until you press q. For\n"
      "                        watching a jam take hold rather than asking\n"
      "                        whether it has.\n"
      "  list-cameras          one line per camera the daemon knows about\n"
      "  dongle [PORT]         ask the dongle what it can hear. The other\n"
      "                        radio: an nRF52840 running the same sync\n"
      "                        daemon as firmware, listening to the same room\n"
      "                        and reporting the same boxes with its own\n"
      "                        offsets. Sets its clock first, because a box\n"
      "                        with no wall clock measures nothing.\n"
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
      "  enable | disable      whether a timecode box is listened to. They\n"
      "                        are heard unless somebody says otherwise, so\n"
      "                        this is for the one in the next room that is\n"
      "                        not part of this shoot. Say which with --box.\n"
      "                        With no --box, reports the file.\n"
      "  warn [on|off]         whether to be told when a device is wrong: a\n"
      "                        marker beside it here and a blip in the menu\n"
      "                        bar, red when it is too far from the bench and\n"
      "                        yellow when it has been quiet too long to say.\n"
      "                        Off until asked for, so name the devices you\n"
      "                        are working with, with --box and --camera.\n"
      "                        With no argument, reports the file.\n"
      "  remove                take a device off the list entirely: its\n"
      "                        settings and everything either daemon has\n"
      "                        learned about it, deleted. Not `disable`,\n"
      "                        which is a decision that gets remembered --\n"
      "                        this leaves nothing behind, so a device still\n"
      "                        in range comes back at its defaults. Say which\n"
      "                        with --box and --camera.\n"
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
      "  --box NAME|ID         which timecode box, repeatable. enable,\n"
      "                        disable and warn take this.\n"
      "  --daemon WHICH        `all` (the default), `bench` for octomancerd,\n"
      "                        or `sync` for octomancer-sync. Only start,\n"
      "                        stop and restart look at this.\n"
      "  --socket PATH         octomancer-sync's control socket (default\n"
      "                        %s)\n"
      "  --bench-socket PATH   octomancerd's socket -- a different program on\n"
      "                        a different path, and not interchangeable with\n"
      "                        --socket. status and enable/disable read it.\n"
      "                        (default\n"
      "                        %s)\n"
      "  --timeout SEC         how long to wait for a request (default 180)\n"
      "  --no-wait             queue the request and exit without waiting\n"
      "  --verbose             every column status has, each camera's detail,\n"
      "                        and what launchd makes of the daemons\n"
      "  --json                machine-readable output where there is any\n"
      "  --no-color            plain text even on a terminal\n"
      "  --version, --help\n"
      "\n"
      "If nothing answers: `octomancer start`. Run a daemon in a terminal\n"
      "instead to watch it work.\n",
      octo::default_control_socket_path().c_str(),
      octo::default_socket_path().c_str());
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

// Everything octomancer-sync knows about one camera, which is a good deal
// more than a row in the device table can hold. Only `--verbose` asks for it:
// on a bench with six cameras this is sixty lines, and the question "is
// anything wrong" is answered by the table above it.
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

// A device to write a line about: the id the file is keyed on, and the name to
// put beside it and to say back to whoever typed this.
using Target = std::pair<std::string, std::string>;

// The word a person says for yes and the word they say for no.
//
// Three spellings each, because `writes on` and `writes enable` are the same
// sentence and refusing one of them teaches nothing.
bool parse_on_off(const std::string& word, bool* on) {
  if (word == "on" || word == "enable" || word == "yes") {
    *on = true;
    return true;
  }
  if (word == "off" || word == "disable" || word == "no") {
    *on = false;
    return true;
  }
  return false;
}

// Turning what somebody typed into ids, for cameras.
//
// `status` is whatever octomancer-sync answered with, or null when it did not
// answer -- which is not a failure here: an id works without a daemon, and a
// name that matches nothing is written through as an id, because naming a
// camera before it has ever been seen is a reasonable thing to want.
//
// The file is looked in second, for the case that is neither of those: a
// camera named by the name it advertises while nothing is answering. Its id is
// already written down beside that name from the last time a daemon was up,
// and without this the name would be taken for an id and a second line written
// about a camera that already has one.
std::vector<Target> resolve_cameras(const octo::Status* status,
                                    const octo::CamConf& conf,
                                    const std::vector<std::string>& want) {
  std::vector<Target> targets;
  for (const std::string& which : want) {
    std::string id = which, name;
    bool found = false;
    if (status != nullptr) {
      for (const octo::CameraStatus& c : status->cameras) {
        if (c.id == which || c.name == which) {
          id = c.id;
          name = c.name;
          found = true;
          break;
        }
      }
    }
    if (!found) {
      for (const octo::CameraConfig& c : conf.cameras()) {
        if (c.id == which || c.name == which) {
          id = c.id;
          name = c.name;
          break;
        }
      }
    }
    targets.emplace_back(id, name);
  }
  return targets;
}

// The same, for timecode boxes, and it looks in the same two places.
//
// A box can be named by the id in the file or by the name it advertises, and
// somebody has whichever of those is in front of them. Looking in both means a
// box can be switched off by name while octomancerd is up and back on by id
// when it is not.
std::vector<Target> resolve_boxes(const octo::Snapshot* snap,
                                  const octo::CamConf& conf,
                                  const std::vector<std::string>& want) {
  std::vector<Target> targets;
  for (const std::string& which : want) {
    std::string id = which, name;
    bool found = false;
    if (snap != nullptr) {
      for (const octo::DeviceSnapshot& d : snap->device) {
        if (d.id == which || d.name == which) {
          id = d.id;
          name = d.name;
          found = true;
          break;
        }
      }
    }
    if (!found) {
      for (const octo::BoxConfig& b : conf.boxes()) {
        if (b.id == which || b.name == which) {
          id = b.id;
          name = b.name;
          break;
        }
      }
    }
    targets.emplace_back(id, name);
  }
  return targets;
}

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
  if (!parse_on_off(argument, &enable)) {
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

  std::vector<Target> targets;
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
    targets = resolve_cameras(have_status ? &s : nullptr, conf, opt.cameras);
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

// ---------------------------------------------------------------- the boxes
//
// The same job as `writes`, on the other half of the same file. The verb
// carries the answer here rather than an argument, because `octomancer disable
// --box Tentacle_3` is how somebody says it out loud; everything else about
// this is `writes` with two words changed.
//
// A box defaults to *on*, which is the opposite of a camera, and src/camconf.h
// explains why. The consequence here is that the file usually holds only the
// boxes somebody has switched off, so the report also lists the boxes
// octomancerd can hear that the file has never heard of. A report that showed
// only the exceptions would look, on a perfectly healthy bench, exactly like
// one where nothing is configured at all.
int run_boxes_command(const Options& opt, bool enable, const Paint& p) {
  octo::CamConf conf;
  std::string err;
  if (!conf.load(octo::default_camera_config_path(), &err)) {
    std::fprintf(stderr, "octomancer: %s\n", err.c_str());
    return 1;
  }

  // Only for turning a name into an id, and for listing what is not in the
  // file. Not answering is fine: an id works without it.
  octo::Snapshot snap;
  std::string ignored;
  const bool have_bench = octo::fetch(opt.bench_socket_path, &snap, &ignored);

  // `writes` is the command for cameras and this one is the command for
  // boxes. Two commands that both set a per-device flag but mean opposite
  // things by "off" is already enough to keep straight; letting either accept
  // the other's selector would be one more way to change the wrong device.
  if (!opt.cameras.empty()) {
    std::fprintf(stderr,
                 "octomancer: enable and disable are for timecode boxes."
                 " A camera's permission is `octomancer writes on|off"
                 " --camera ID`.\n");
    return 2;
  }

  // No selector is a question, whichever of the two verbs was used to ask it.
  if (opt.boxes.empty()) {
    std::printf("%s%s\n", conf.path().c_str(),
                conf.file_exists() ? "" : "  (does not exist yet)");
    std::printf("  %-38s %s\n", "default",
                conf.default_box_enabled() ? "on" : "off");
    for (const octo::BoxConfig& b : conf.boxes()) {
      std::printf("  %-38s %s%s%s%s%s\n", b.id.c_str(),
                  p(b.enabled ? kGreen : kDim), b.enabled ? "on" : "off",
                  p(kReset), b.name.empty() ? "" : "  ", b.name.c_str());
    }
    if (have_bench) {
      for (const octo::DeviceSnapshot& d : snap.device) {
        if (conf.find_box(d.id) != nullptr) continue;
        const bool on = conf.default_box_enabled();
        std::printf("  %-38s %s%s%s  %s  %s(taking the default)%s\n",
                    d.id.c_str(), p(on ? kGreen : kDim), on ? "on" : "off",
                    p(kReset), d.name.c_str(), p(kDim), p(kReset));
      }
    }
    return 0;
  }

  const std::vector<Target> targets =
      resolve_boxes(have_bench ? &snap : nullptr, conf, opt.boxes);

  for (const auto& target : targets) {
    if (!conf.set_box_enabled(target.first, target.second, enable, &err)) {
      std::fprintf(stderr, "octomancer: %s\n", err.c_str());
      return 1;
    }
    std::printf("%s%s%s %s\n", p(enable ? kGreen : kYellow),
                (target.second.empty() ? target.first : target.second).c_str(),
                p(kReset), enable ? "enabled" : "disabled");
  }
  std::printf("saved to %s\n", conf.path().c_str());

  // Told to re-read for the same reason `writes` tells it: the daemon owns no
  // copy of this file it did not read from disk. Silent when nothing answers,
  // because the lists a person is about to look at are drawn from the file
  // directly, so a box switched off here is switched off with or without a
  // daemon.
  std::string reply, unused;
  if (octo::query(opt.socket_path, "reload", &reply, &unused, 3.0)) {
    std::printf("the running daemon has been told to re-read it\n");
  }
  return 0;
}

// -------------------------------------------------------------- the warning
//
// The third setting on the same file, and the only one that is neither a
// permission nor a filter: it does not change what anything does to a device,
// it changes whether anybody is told when that device is wrong. So it takes
// both selectors -- `--camera` and `--box` -- where the other two take one
// each, because "warn me about this thing" means the same sentence whichever
// kind of thing it is, and somebody standing at a bench is looking at one
// room, not at two lists.
//
// Off by default, and src/camconf.h explains why at length: an indicator that
// lights up about a box in a case in the truck is one people learn to ignore
// inside a day, and a red light that has been learned to mean nothing is worse
// than no red light at all.
// One line of that report. The kind is said on every line and in dim, because
// this is the one list in the program with cameras and timecode boxes in it at
// once, and an id says nothing about which of the two it names.
void print_warn_line(const std::string& id, const std::string& name, bool warn,
                     const char* kind, const Paint& p) {
  std::printf("  %-38s %s%s%s  %s%s%s(%s)%s\n", id.c_str(),
              p(warn ? kGreen : kDim), warn ? "on " : "off", p(kReset),
              name.c_str(), name.empty() ? "" : "  ", p(kDim), kind,
              p(kReset));
}

int run_warn_command(const Options& opt, const std::string& argument,
                     const Paint& p) {
  octo::CamConf conf;
  std::string err;
  if (!conf.load(octo::default_camera_config_path(), &err)) {
    std::fprintf(stderr, "octomancer: %s\n", err.c_str());
    return 1;
  }

  // No argument is a question, and the file answers it on its own. Unlike
  // `enable`, nothing is added here for the devices the file has never heard
  // of: they are off, because everything is off until asked for, and printing
  // every box in the room to say so would bury the two lines that matter.
  // `octomancer status` is where the room is listed.
  if (argument.empty()) {
    std::printf("%s%s\n", conf.path().c_str(),
                conf.file_exists() ? "" : "  (does not exist yet)");
    std::printf("  %-38s %s\n", "default", conf.default_warn() ? "on" : "off");
    bool any = false;
    for (const octo::CameraConfig& c : conf.cameras()) {
      any = any || c.warn;
      print_warn_line(c.id, c.name, c.warn, "camera", p);
    }
    for (const octo::BoxConfig& b : conf.boxes()) {
      any = any || b.warn;
      print_warn_line(b.id, b.name, b.warn, "timecode box", p);
    }
    if (!any && !conf.default_warn()) {
      std::printf("\n%sNothing warns, so nothing will ever go red.%s\n"
                  "Ask about one with: octomancer warn on --box <name>\n",
                  p(kYellow), p(kReset));
    }
    return 0;
  }

  bool warn = false;
  if (!parse_on_off(argument, &warn)) {
    std::fprintf(stderr, "octomancer: say `on` or `off`, not '%s'\n",
                 argument.c_str());
    return 2;
  }

  // --all is refused rather than taken to mean the cameras, which is all it
  // could mean: this command spans both kinds of device, and a flag that
  // silently covered half of them would be read as covering the room.
  if (opt.all_cameras) {
    std::fprintf(stderr,
                 "octomancer: --all is every *camera*, which is not the whole"
                 " room. Name the devices with --camera and --box.\n");
    return 2;
  }
  if (opt.cameras.empty() && opt.boxes.empty()) {
    std::fprintf(stderr,
                 "octomancer: say which device -- `--box NAME|ID` or"
                 " `--camera ID|NAME`, repeatable.\n"
                 "  `octomancer warn` shows what is set now.\n");
    return 2;
  }

  octo::Snapshot snap;
  std::string ignored;
  const bool have_bench =
      opt.boxes.empty()
          ? false
          : octo::fetch(opt.bench_socket_path, &snap, &ignored);
  octo::Status status;
  const bool have_status =
      opt.cameras.empty() ? false : fetch_status_quiet(opt, &status);

  std::vector<std::pair<bool, Target>> targets;  // camera?, device
  for (const Target& t :
       resolve_boxes(have_bench ? &snap : nullptr, conf, opt.boxes)) {
    targets.emplace_back(false, t);
  }
  for (const Target& t :
       resolve_cameras(have_status ? &status : nullptr, conf, opt.cameras)) {
    targets.emplace_back(true, t);
  }

  for (const auto& target : targets) {
    const Target& d = target.second;
    const bool ok = target.first
                        ? conf.set_camera_warn(d.first, d.second, warn, &err)
                        : conf.set_box_warn(d.first, d.second, warn, &err);
    if (!ok) {
      std::fprintf(stderr, "octomancer: %s\n", err.c_str());
      return 1;
    }
    std::printf("%s%s%s %s\n", p(warn ? kGreen : kYellow),
                (d.second.empty() ? d.first : d.second).c_str(), p(kReset),
                warn ? "will be warned about" : "will not be warned about");
  }
  std::printf("saved to %s\n", conf.path().c_str());

  // Nothing is told to re-read it, and there is nothing to tell: no daemon has
  // ever read this key. A warning is decided where the devices are drawn --
  // `octomancer status` and the app both call build_device_view -- so it is in
  // force the next time somebody looks, with or without a daemon running.
  return 0;
}

// ------------------------------------------------------------------ remove
//
// Take a device off the list: out of the configuration file, and out of
// whichever daemon is holding what it has learned about it.
//
// Deliberately not the same as `disable`, and the difference is worth being
// clear about because they are one word apart. `disable` is a decision that
// gets remembered -- which is exactly what keeps a switched-off device on the
// page forever. This removes the memory instead, so nothing is left with an
// opinion, and the next advertisement puts the device back at its defaults.
// Somebody who wants it gone and staying gone wants `disable`.
int run_remove_command(const Options& opt, const std::string& argument,
                       const Paint& p) {
  if (!argument.empty()) {
    std::fprintf(stderr,
                 "octomancer: name the device with --box or --camera, as in"
                 " `octomancer remove --box %s`.\n", argument.c_str());
    return 2;
  }
  if (opt.all_cameras) {
    std::fprintf(stderr,
                 "octomancer: --all is every *camera*, which is not the whole"
                 " room, and this is not a command to run on a guess. Name the"
                 " devices with --camera and --box.\n");
    return 2;
  }
  if (opt.cameras.empty() && opt.boxes.empty()) {
    std::fprintf(stderr,
                 "octomancer: say which device -- `--box NAME|ID` or"
                 " `--camera ID|NAME`, repeatable.\n");
    return 2;
  }

  octo::CamConf conf;
  std::string err;
  if (!conf.load(octo::default_camera_config_path(), &err)) {
    std::fprintf(stderr, "octomancer: %s\n", err.c_str());
    return 1;
  }

  octo::Snapshot snap;
  std::string ignored;
  const bool have_bench =
      opt.boxes.empty()
          ? false
          : octo::fetch(opt.bench_socket_path, &snap, &ignored);
  octo::Status status;
  const bool have_status =
      opt.cameras.empty() ? false : fetch_status_quiet(opt, &status);

  std::vector<std::pair<bool, Target>> targets;  // camera?, device
  for (const Target& t :
       resolve_boxes(have_bench ? &snap : nullptr, conf, opt.boxes)) {
    targets.emplace_back(false, t);
  }
  for (const Target& t :
       resolve_cameras(have_status ? &status : nullptr, conf, opt.cameras)) {
    targets.emplace_back(true, t);
  }

  int failures = 0;
  for (const auto& target : targets) {
    const bool camera = target.first;
    const Target& d = target.second;
    const std::string label = d.second.empty() ? d.first : d.second;

    // The file first. A daemon told to forget and then restarted would read
    // the line straight back in, so the line has to go first for the two to
    // agree however the ordering works out.
    const bool ok = camera ? conf.forget_camera(d.first, &err)
                           : conf.forget_box(d.first, &err);
    if (!ok) {
      std::fprintf(stderr, "octomancer: %s\n", err.c_str());
      return 1;
    }

    // Then the daemon holding the history. Reported and counted, but the
    // settings are already gone and a daemon that is not running has nothing
    // to forget.
    std::string reply, derr;
    const std::string ask =
        camera ? "forget camera=" + d.first : "forget " + d.first;
    const std::string where =
        camera ? opt.socket_path : opt.bench_socket_path;
    if (octo::query(where, ask, &reply, &derr, 5.0)) {
      std::printf("%s%s%s removed -- settings and history deleted\n",
                  p(kGreen), label.c_str(), p(kReset));
    } else {
      ++failures;
      std::printf("%s%s%s removed from %s\n", p(kYellow), label.c_str(),
                  p(kReset), conf.path().c_str());
      std::printf("  %s did not answer, so what it has learned about this"
                  " device is still in memory: %s\n",
                  camera ? "octomancer-sync" : "octomancerd", derr.c_str());
    }
    if (camera) {
      // The one part of this the program cannot do, said here rather than left
      // to be discovered when the next pairing behaves oddly.
      std::printf("  the Bluetooth pairing is not octomancer's to undo:"
                  " remove this Mac in the camera's setup menu, and remove"
                  " the camera in System Settings > Bluetooth\n");
    }
  }
  if (targets.empty()) {
    std::fprintf(stderr, "octomancer: no such device\n");
    return 1;
  }
  // Nothing is blacklisted, so say what will happen next rather than letting
  // somebody find out by watching the device come back.
  std::printf("Nothing is blocked: a device still switched on and in range"
              " will reappear, at its defaults.\n");
  return failures == 0 ? 0 : 1;
}

// ------------------------------------------------------------------ status
//
// The one command that talks to both daemons. Neither knows what the other can
// see -- octomancerd hears the boxes, octomancer-sync connects to the cameras
// -- and reconciling them into one list of devices is a decision with no radio
// in it, so it lives in src/devices.h and is shared with the app. What is left
// here is the part that cannot be done without a socket: asking, and saying
// which ask went unanswered.

struct DaemonReport {
  octo::Agent agent = octo::Agent::kBench;
  octo::AgentState state;
  bool answering = false;
  std::string error;   // what the socket said when it said no
};

// launchd's view of one daemon and this program's view of it, on one line.
// The sentence itself is agents.h's, so that the terminal interface says the
// same thing about the same daemon.
void print_daemon_line(const DaemonReport& d, const Paint& p, bool verbose) {
  const double now = octo::wall_now();
  const octo::AgentState& state = d.state;
  const octo::AgentSituation sit =
      octo::agent_situation(state, d.answering, now);
  const char* colour = sit.mood == octo::AgentMood::kBad    ? kRed
                       : sit.mood == octo::AgentMood::kWarn ? kYellow
                                                           : kGreen;
  std::printf("  %-16s %s%s%s\n", octo::agent_program(d.agent), p(colour),
              sit.said.c_str(), p(kReset));

  if (!verbose) return;
  std::string detail = state.installed ? "installed, starts at boot"
                                       : "not installed";
  if (state.loaded) detail += ", loaded";
  if (state.running) {
    detail += ", pid " + std::to_string(state.pid);
    if (state.has_started) {
      detail += ", started " + clock_of(state.started_wall);
    }
  } else if (state.last_exit != 0) {
    detail += ", last exit " + std::to_string(state.last_exit);
  }
  std::printf("  %-16s %s%s%s\n", "", p(kDim), detail.c_str(), p(kReset));
  if (!d.answering && !d.error.empty()) {
    std::printf("  %-16s %s%s%s\n", "", p(kDim), d.error.c_str(), p(kReset));
  }
}

void print_agent_states(const DaemonReport& bench, const DaemonReport& sync,
                        const Paint& p, bool verbose) {
  print_daemon_line(bench, p, verbose);
  print_daemon_line(sync, p, verbose);
}

int run_status_command(const Options& opt, const Paint& p) {
  DaemonReport bench;
  bench.agent = octo::Agent::kBench;
  bench.state = octo::agent_state(octo::Agent::kBench);
  octo::Snapshot snapshot;
  bench.answering = octo::fetch(opt.bench_socket_path, &snapshot, &bench.error);

  DaemonReport sync;
  sync.agent = octo::Agent::kSync;
  sync.state = octo::agent_state(octo::Agent::kSync);
  octo::Status status;
  std::string reply;
  sync.answering =
      octo::query(opt.socket_path, "status", &reply, &sync.error, 5.0) &&
      octo::parse_status(reply, &status, &sync.error);

  // Read here rather than asked of a daemon, on purpose: it is a file a person
  // owns, and status should say what that file says even when nothing is
  // running to have read it. A file that will not parse is worth saying out
  // loud but not worth refusing over -- half a parse would hide devices for a
  // reason nobody could see -- so the view is built without it instead.
  octo::CamConf conf;
  std::string conf_err;
  const bool conf_ok = conf.load(octo::default_camera_config_path(), &conf_err);
  if (!conf_ok) {
    std::fprintf(stderr,
                 "octomancer: %s\n"
                 "  Showing every device, as though none had been switched"
                 " off.\n", conf_err.c_str());
  }

  // The version and the two daemon lines are --verbose, because in the
  // ordinary case they say the same four words every time: this command cannot
  // answer at all without a daemon, and the daemons start themselves. Four
  // lines of preamble before the thing somebody actually ran the command for
  // is how a status page stops being read.
  //
  // The exception keeps its voice. A daemon that is *not* answering is
  // precisely the fact worth having at the moment it happens -- the table
  // above will be quietly missing half the room and nothing else on the page
  // would say why -- so that is printed whether or not anybody asked.
  bool said = false;
  if (opt.verbose || !bench.answering || !sync.answering) {
    std::printf("%soctomancer%s %s\n", p(kBold), p(kReset),
                sync.answering && !status.daemon.version.empty()
                    ? status.daemon.version.c_str()
                    : OCTO_VERSION);
    print_agent_states(bench, sync, p, opt.verbose);
    said = true;
  }
  // Both of these are conditions rather than status, and both stay: one says
  // nothing will be written to a camera however good the numbers look, and the
  // other says an answer somebody asked for has not happened yet.
  if (sync.answering && status.daemon.dry_run) {
    std::printf("  %-16s %sDRY RUN -- no camera will be written to%s\n", "",
                p(kYellow), p(kReset));
    said = true;
  }
  if (sync.answering && status.queued > 0) {
    std::printf("  %-16s %d request%s waiting\n", "queued", status.queued,
                status.queued == 1 ? "" : "s");
    said = true;
  }
  if (said) std::printf("\n");

  octo::DeviceSources src;
  src.bench = bench.answering ? &snapshot : nullptr;
  src.cameras = sync.answering ? &status : nullptr;
  src.conf = conf_ok ? &conf : nullptr;
  const octo::DeviceView view = octo::build_device_view(src);
  std::fputs(octo::render_devices(view, opt.verbose, opt.color).c_str(),
             stdout);

  // Only the cameras that got a row above. The table and this block describing
  // different sets of devices would be worse than either alone; the count of
  // hidden devices in the header already says that some were left out, and
  // `octomancer writes` is where you go to see which.
  if (opt.verbose && sync.answering && !status.cameras.empty()) {
    const double now = status.daemon.now_wall > 0.0 ? status.daemon.now_wall
                                                    : octo::wall_now();
    std::printf("\n");
    for (const octo::CameraStatus& c : status.cameras) {
      if (conf_ok && !conf.writes_enabled(c.id)) continue;
      print_camera_line(c, p, now);
      std::printf("\n");
    }
  }

  // Nonzero only when neither daemon answered, which is the only case where
  // this printed nothing about the room. One daemon down is a fact this
  // command reports rather than a failure of the command, and the line saying
  // so is the whole reason to run it at that moment.
  return (bench.answering || sync.answering) ? 0 : 1;
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
    kCamera = 1000, kBox, kAllCameras, kSocket, kBenchSocket, kTimeout, kJson,
    kNoColor, kNoWait, kVerbose, kDaemon, kVersion, kHelp,
  };
  static const struct option longs[] = {
      {"camera", required_argument, nullptr, kCamera},
      {"box", required_argument, nullptr, kBox},
      {"all", no_argument, nullptr, kAllCameras},
      {"daemon", required_argument, nullptr, kDaemon},
      {"socket", required_argument, nullptr, kSocket},
      {"bench-socket", required_argument, nullptr, kBenchSocket},
      {"timeout", required_argument, nullptr, kTimeout},
      {"json", no_argument, nullptr, kJson},
      {"no-color", no_argument, nullptr, kNoColor},
      {"no-wait", no_argument, nullptr, kNoWait},
      {"verbose", no_argument, nullptr, kVerbose},
      {"version", no_argument, nullptr, kVersion},
      {"help", no_argument, nullptr, kHelp},
      {nullptr, 0, nullptr, 0},
  };
  for (;;) {
    const int c = getopt_long(argc, argv, "", longs, nullptr);
    if (c == -1) break;
    switch (c) {
      case kCamera: opt.cameras.push_back(optarg); break;
      case kBox: opt.boxes.push_back(optarg); break;
      case kAllCameras: opt.all_cameras = true; break;
      case kDaemon: opt.daemon = optarg; break;
      case kSocket: opt.socket_path = optarg; break;
      case kBenchSocket: opt.bench_socket_path = optarg; break;
      case kTimeout: opt.timeout = std::atof(optarg); break;
      case kJson: opt.json = true; break;
      case kNoColor: opt.color = false; break;
      case kNoWait: opt.wait = false; break;
      case kVerbose: opt.verbose = true; break;
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
    if (command == "list-cameras") {
      octo::Status s;
      const int rc = fetch_status(opt, &s);
      if (rc != 0) return rc;
      print_camera_list(s, paint);
      return 0;
    }
    return run_status_command(opt, paint);
  }

  if (command == "tui") {
    octo::TuiOptions tui;
    tui.sync_socket_path = opt.socket_path;
    tui.bench_socket_path = opt.bench_socket_path;
    tui.camera_config_path = octo::default_camera_config_path();
    tui.color = opt.color;
    return octo::run_tui(tui);
  }

  if (command == "start" || command == "stop" || command == "restart") {
    return run_agent_command(opt, command, paint);
  }

  if (command == "writes") {
    return run_writes_command(opt, argument, paint);
  }

  if (command == "warn") {
    return run_warn_command(opt, argument, paint);
  }

  if (command == "enable" || command == "disable") {
    // A bare word after the verb is almost certainly the box somebody meant,
    // and guessing at it is worse than saying where it goes.
    if (!argument.empty()) {
      std::fprintf(stderr,
                   "octomancer: name the box with --box, as in `octomancer %s"
                   " --box %s`.\n", command.c_str(), argument.c_str());
      return 2;
    }
    return run_boxes_command(opt, command == "enable", paint);
  }

  if (command == "remove" || command == "forget") {
    return run_remove_command(opt, argument, paint);
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

  if (command == "dongle") {
    const std::string device = optind + 1 < argc ? argv[optind + 1] : "";
    return run_dongle(device, opt.verbose);
  }

  std::fprintf(stderr, "octomancer: unknown command '%s'\n", command.c_str());
  usage(stderr);
  return 2;
}
