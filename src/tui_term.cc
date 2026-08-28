// The half of `octomancer tui` that touches a terminal.
//
// src/tui.h explains the split: everything about what the page says is in
// src/tui.cc where a test can read it, and everything here is I/O -- putting
// the tty into a mode where a keystroke arrives without a newline after it,
// asking two daemons for a fresh snapshot every second, and laying the lines
// onto the screen without making it flicker.
//
// The one rule this file exists to keep: *whatever happens, give the terminal
// back*. A program that has switched to the alternate screen, hidden the
// cursor and turned off echo has taken something that is not its own, and a
// crash or a signal that skips the handing-back leaves a person typing blind
// into a shell that looks broken. So the restoring is a destructor, it runs on
// every path out, and the signals that would otherwise kill us are caught and
// turned into an ordinary end of the loop.
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "agents.h"
#include "camconf.h"
#include "client.h"
#include "control.h"
#include "devices.h"
#include "proto.h"
#include "server.h"
#include "timeutil.h"
#include "tui.h"

namespace octo {
namespace {

volatile sig_atomic_t g_quit = 0;
volatile sig_atomic_t g_resized = 0;

void on_quit_signal(int) { g_quit = 1; }
void on_resize_signal(int) { g_resized = 1; }

// Everything taken from the terminal, and the promise to give it back. The
// constructor takes; the destructor returns. Nothing in between may return
// early past it, which is the entire reason it is a type and not two
// functions.
class Screen {
 public:
  explicit Screen(int fd) : fd_(fd) {
    if (tcgetattr(fd_, &saved_) == 0) {
      restore_termios_ = true;
      struct termios raw = saved_;
      // ICANON off so a keystroke arrives without waiting for a newline, ECHO
      // off so it does not appear in the middle of the table. ISIG stays *on*:
      // Ctrl-C should still kill this program even if the redraw loop has
      // wedged, and a full-screen program that cannot be interrupted is the
      // one bug here that a person cannot work around.
      raw.c_lflag &= ~(ICANON | ECHO);
      // A read returns whatever has arrived and does not block. The waiting is
      // poll()'s job, because it can wait for a key and for the next refresh
      // at the same time.
      raw.c_cc[VMIN] = 0;
      raw.c_cc[VTIME] = 0;
      tcsetattr(fd_, TCSAFLUSH, &raw);
    }
    // The alternate screen, so that quitting puts back whatever was on the
    // terminal before -- a person who ran this in the middle of a session
    // gets their scrollback, not a screen full of a table that is no longer
    // true. Then the cursor out of the way: it has nothing to point at here
    // and it blinks in whatever cell the last line happened to end in.
    write_all("\033[?1049h\033[?25l");
  }

  ~Screen() {
    write_all("\033[?25h\033[?1049l");
    if (restore_termios_) tcsetattr(fd_, TCSAFLUSH, &saved_);
  }

  Screen(const Screen&) = delete;
  Screen& operator=(const Screen&) = delete;

  // One frame. Home the cursor and overwrite, rather than clearing first: a
  // clear followed by a draw is a window that is briefly empty, and at one
  // frame a second that reads as a flicker rather than as an update. Each line
  // erases to its own right so a short line does not leave the tail of a
  // longer one behind it, and the whole rest of the screen is erased at the
  // end for when the page has grown shorter.
  void draw(const std::vector<std::string>& lines) {
    std::string out = "\033[H";
    for (size_t i = 0; i < lines.size(); ++i) {
      out += lines[i];
      out += "\033[K";
      if (i + 1 < lines.size()) out += "\n";
    }
    out += "\033[J";
    write_all(out);
  }

  // How much room there is. Zero when the terminal will not say, which
  // fit_to_rows reads as "do not cut anything".
  int rows() const {
    struct winsize ws;
    if (ioctl(fd_, TIOCGWINSZ, &ws) != 0) return 0;
    return ws.ws_row;
  }

 private:
  void write_all(const std::string& s) {
    size_t sent = 0;
    while (sent < s.size()) {
      const ssize_t n = ::write(fd_, s.data() + sent, s.size() - sent);
      if (n > 0) {
        sent += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      return;  // a terminal that has stopped listening is not worth spinning on
    }
  }

  int fd_;
  struct termios saved_ {};
  bool restore_termios_ = false;
};

// One poll of everything the page is made of. Both daemons are asked and
// neither is required: a daemon that is not running is an ordinary Tuesday
// here, and the page says so on its own line rather than refusing to draw.
//
// The timeouts are short on purpose. This runs once a second, and a socket
// that takes five seconds to give up would stop the clock in the corner and
// make a slow daemon look like a hung program.
TuiFrame poll_once(const TuiOptions& opt) {
  TuiFrame f;
  f.version = OCTO_VERSION;
  f.now_wall = wall_now();

  TuiDaemon bench;
  bench.agent = Agent::kBench;
  bench.state = agent_state(Agent::kBench);
  Snapshot snapshot;
  std::string err;
  bench.answering = fetch(opt.bench_socket_path, &snapshot, &err, 2.0);

  TuiDaemon sync;
  sync.agent = Agent::kSync;
  sync.state = agent_state(Agent::kSync);
  Status status;
  std::string reply;
  sync.answering =
      query(opt.sync_socket_path, "status", &reply, &err, 2.0) &&
      parse_status(reply, &status, &err);

  if (sync.answering) {
    if (!status.daemon.version.empty()) f.version = status.daemon.version;
    f.dry_run = status.daemon.dry_run;
    f.queued = status.queued;
  }
  f.daemons.push_back(bench);
  f.daemons.push_back(sync);

  // Re-read every time round rather than once at startup. The file is a
  // person's, and `octomancer enable` in another window is a normal thing to
  // do while this is on screen -- a page that keeps showing a box somebody
  // just switched off would be answering a question from a minute ago. A file
  // that will not parse leaves the view unfiltered, exactly as the status
  // command does, rather than emptying the table for a reason nothing on
  // screen could explain.
  CamConf conf;
  std::string conf_err;
  const bool conf_ok = conf.load(opt.camera_config_path, &conf_err);

  DeviceSources src;
  src.bench = bench.answering ? &snapshot : nullptr;
  src.cameras = sync.answering ? &status : nullptr;
  src.conf = conf_ok ? &conf : nullptr;
  f.view = build_device_view(src);
  return f;
}

}  // namespace

int run_tui(const TuiOptions& opt) {
  // Both ends have to be a terminal: the drawing needs one to draw on, and the
  // keystrokes need one to come from. Piping this somewhere is not a mode
  // worth inventing -- `octomancer status` is that mode, and it is already
  // written.
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    std::fprintf(stderr,
                 "octomancer: tui needs a terminal on both stdin and stdout."
                 " For a pipe or a file, use `octomancer status`.\n");
    return 2;
  }

  signal(SIGINT, on_quit_signal);
  signal(SIGTERM, on_quit_signal);
  signal(SIGWINCH, on_resize_signal);
  // A terminal that goes away mid-write should end the loop through the
  // destructor below, not kill the process where nothing can put the tty back.
  signal(SIGPIPE, SIG_IGN);

  Screen screen(STDOUT_FILENO);

  bool due = true;  // the first frame is drawn before anything is waited for
  double next_poll = 0.0;
  for (;;) {
    if (g_quit) break;

    if (due || g_resized || mono_now() >= next_poll) {
      g_resized = 0;
      due = false;
      const TuiFrame frame = poll_once(opt);
      screen.draw(fit_to_rows(render_tui(frame, opt.color), screen.rows(),
                              opt.color));
      next_poll = mono_now() + opt.interval_s;
    }

    // Wait for whichever comes first, a key or the next refresh. Waiting on
    // the key alone would mean a page that only updates when somebody touches
    // it; sleeping for the interval alone would mean a `q` that takes up to a
    // second to be noticed.
    const double wait = next_poll - mono_now();
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;
    const int timeout_ms = wait <= 0.0 ? 0 : static_cast<int>(wait * 1000.0);
    const int ready = poll(&pfd, 1, timeout_ms);
    if (ready < 0) {
      if (errno == EINTR) continue;  // a signal, which the top of the loop reads
      break;
    }
    if (ready == 0) continue;

    char buf[32];
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
    if (n <= 0) {
      // End of input on a terminal means the terminal is gone. Staying would
      // be a loop nobody can talk to.
      if (n == 0) break;
      if (errno == EINTR || errno == EAGAIN) continue;
      break;
    }
    bool quit = false;
    bool redraw = false;
    // A paste, or an arrow key, arrives as several bytes at once. Every one of
    // them gets asked about: a `q` should work whatever else came in with it.
    for (ssize_t i = 0; i < n; ++i) {
      switch (tui_action_for_key(buf[i])) {
        case TuiAction::kQuit: quit = true; break;
        case TuiAction::kRedraw: redraw = true; break;
        case TuiAction::kNone: break;
      }
    }
    if (quit) break;
    if (redraw) due = true;
  }

  return 0;
}

}  // namespace octo
