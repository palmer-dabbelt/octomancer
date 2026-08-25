// octomancerctl -- ask the running agent what it can see.
#include <getopt.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "client.h"
#include "proto.h"
#include "render.h"
#include "server.h"
#include "timeutil.h"

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

void usage(FILE* out) {
  std::fprintf(out,
      "usage: octomancerctl [options] [status|watch|json|ping]\n"
      "\n"
      "  status   one report and exit (the default)\n"
      "  watch    redraw until interrupted\n"
      "  json     one machine-readable snapshot\n"
      "  ping     check the agent is answering\n"
      "\n"
      "  --socket PATH      control socket (default %s)\n"
      "  --interval SEC     redraw period for watch (default 2)\n"
      "  --no-color         plain text even on a terminal\n"
      "  --help\n",
      octo::default_socket_path().c_str());
}

}  // namespace

int main(int argc, char** argv) {
  std::string socket_path = octo::default_socket_path();
  double interval = 2.0;
  bool color = isatty(1);

  enum { kSocket = 1000, kInterval, kNoColor, kHelp };
  static const struct option longs[] = {
      {"socket", required_argument, nullptr, kSocket},
      {"interval", required_argument, nullptr, kInterval},
      {"no-color", no_argument, nullptr, kNoColor},
      {"help", no_argument, nullptr, kHelp},
      {nullptr, 0, nullptr, 0},
  };
  for (;;) {
    const int c = getopt_long(argc, argv, "", longs, nullptr);
    if (c == -1) break;
    switch (c) {
      case kSocket: socket_path = optarg; break;
      case kInterval: interval = std::atof(optarg); break;
      case kNoColor: color = false; break;
      case kHelp: usage(stdout); return 0;
      default: usage(stderr); return 2;
    }
  }

  const std::string command = optind < argc ? argv[optind] : "status";
  std::string err;

  if (command == "json") {
    std::string reply;
    if (!octo::query(socket_path, "json", &reply, &err)) {
      std::fprintf(stderr, "octomancerctl: %s\n", err.c_str());
      return 1;
    }
    std::fputs(reply.c_str(), stdout);
    return 0;
  }

  if (command == "ping") {
    std::string reply;
    if (!octo::query(socket_path, "ping", &reply, &err)) {
      std::fprintf(stderr, "octomancerctl: %s\n", err.c_str());
      return 1;
    }
    std::printf("%s", reply.c_str());
    return reply.find("pong") == std::string::npos ? 1 : 0;
  }

  if (command == "status") {
    octo::Snapshot snap;
    if (!octo::fetch(socket_path, &snap, &err)) {
      std::fprintf(stderr, "octomancerctl: %s\n", err.c_str());
      return 1;
    }
    std::fputs(octo::render_human(snap, color).c_str(), stdout);
    return 0;
  }

  if (command == "watch") {
    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);
    while (!g_stop) {
      octo::Snapshot snap;
      std::string body;
      if (octo::fetch(socket_path, &snap, &err)) {
        body = octo::render_human(snap, color);
      } else {
        // Keep redrawing rather than exiting: the agent may be restarting,
        // and a watch that dies the moment it blinks is useless for watching.
        body = "octomancerctl: " + err + "\n";
      }
      if (color) std::fputs("\033[H\033[J", stdout);
      std::fputs(body.c_str(), stdout);
      std::fflush(stdout);

      const double until = octo::mono_now() + interval;
      while (!g_stop && octo::mono_now() < until) {
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, nullptr);
      }
    }
    return 0;
  }

  std::fprintf(stderr, "octomancerctl: unknown command %s\n", command.c_str());
  usage(stderr);
  return 2;
}
