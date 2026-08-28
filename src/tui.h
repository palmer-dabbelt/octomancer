// The terminal interface: what `octomancer tui` puts on the screen.
//
// This is `octomancer status` that stays. The numbers on a bench move --- a
// box drifts, a camera goes quiet, a sync lands --- and the one-shot command
// answers about the instant it was run and then scrolls away. Watching a
// jam-sync take hold with `status` means running it over and over and reading
// a different set of lines each time; watching it here means looking at one
// set of lines that change.
//
// The seam this file sits on is the same one src/camera.h describes. Deciding
// what the page says is arithmetic over a snapshot and lives here, where a
// test can read it. Putting a terminal into raw mode, waiting for a keypress
// and asking two daemons for a new snapshot is I/O, and lives in
// src/tui_term.cc behind run_tui(). Nothing in this file writes to a file
// descriptor or knows how wide the window is.
#ifndef OCTO_TUI_H
#define OCTO_TUI_H

#include <string>
#include <vector>

#include "agents.h"
#include "devices.h"

namespace octo {

// One daemon as the page sees it: what launchd says about the process, and
// whether the socket answered this time round.
struct TuiDaemon {
  Agent agent = Agent::kBench;
  AgentState state;
  bool answering = false;
};

// A single poll's worth of answers -- everything the page is drawn from, and
// nothing about a terminal.
struct TuiFrame {
  std::string version;
  // For the clock in the corner, and for the daemon uptimes.
  double now_wall = 0.0;
  // In the order they are drawn, which is the order they are listed in.
  std::vector<TuiDaemon> daemons;
  bool dry_run = false;
  int queued = 0;
  DeviceView view;
};

// The whole page as text, lines separated by '\n' and the last line ending in
// one. `color` adds ANSI attributes and changes nothing else, in the manner of
// render_devices: with it off the same bytes come out minus the escapes, which
// is what makes this testable.
//
// No cursor movement, no clearing, no padding to the width of a window. A
// caller that is driving a real terminal decides how to lay these lines onto
// it; a caller that is a test just reads them.
std::string render_tui(const TuiFrame& f, bool color);

// What a keypress means. Most keys mean nothing, on purpose: this page has one
// verb, and a program that reacts to keys nobody pressed deliberately is worse
// than one that ignores them.
enum class TuiAction {
  kNone,
  kQuit,
  kRedraw,  // the page is stale for a reason that is not the timer
};

TuiAction tui_action_for_key(char key);

// Cut a page down to a window that is `rows` lines tall.
//
// A page that is one line too long for its window scrolls, and a page that
// scrolls cannot be redrawn in place: every frame after the first lands one
// line lower than the last and the screen turns to sediment. So the fitting
// happens before anything is written, and it happens here rather than in the
// terminal glue because which line to sacrifice is a decision.
//
// The footer is the line that survives. Everything else on the page is
// information, and the footer is the way out; a person looking at a table that
// has taken over their terminal needs the way out more than they need the last
// two devices. What was dropped is said out loud in its place, because a table
// that quietly ends early reads as a bench with fewer boxes in it.
//
// `rows <= 0` means "no window to speak of", and the page comes back whole.
std::vector<std::string> fit_to_rows(const std::string& page, int rows,
                                     bool color);

// ------------------------------------------------------------- the terminal

struct TuiOptions {
  std::string sync_socket_path;   // octomancer-sync's control socket
  std::string bench_socket_path;  // octomancerd's -- not interchangeable
  std::string camera_config_path;
  bool color = true;
  // How often to go and ask again. A second is fast enough that a sync landing
  // looks like it happened rather than like the page caught up, and slow
  // enough that two socket round-trips and a look at launchd are free.
  double interval_s = 1.0;
};

// Run until the person quits. Returns a process exit status: 0 for a normal
// quit, 2 when there is no terminal to draw on.
int run_tui(const TuiOptions& opt);

}  // namespace octo

#endif  // OCTO_TUI_H
