#include "tui.h"

#include <ctime>
#include <cstdarg>
#include <cstdio>

#include "timeutil.h"

namespace octo {
namespace {

// The same palette devices.cc uses, for the same reason: the daemon lines and
// the table are one page, and a green that means "fine" above a table where
// green means "we are hearing it" has to be the same green.
struct Style {
  const char* dim;
  const char* bold;
  const char* red;
  const char* yellow;
  const char* green;
  const char* off;
};

Style style_for(bool color) {
  if (color) {
    return {"\033[2m", "\033[1m", "\033[31m", "\033[33m", "\033[32m",
            "\033[0m"};
  }
  return {"", "", "", "", "", ""};
}

std::string fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

// A wall clock as a person reads one. This is the only thing on the page that
// is guaranteed to change every second, which is the point of it: a frozen
// program and a quiet bench look identical otherwise, and one of those is
// worth knowing about.
std::string clock_of(double wall) {
  if (wall <= 0.0) return "--:--:--";
  const time_t secs = static_cast<time_t>(wall);
  struct tm tm_local;
  localtime_r(&secs, &tm_local);
  char buf[16];
  std::strftime(buf, sizeof buf, "%H:%M:%S", &tm_local);
  return buf;
}

const char* mood_colour(const Style& st, AgentMood m) {
  switch (m) {
    case AgentMood::kBad: return st.red;
    case AgentMood::kWarn: return st.yellow;
    case AgentMood::kFine: return st.green;
  }
  return "";
}

}  // namespace

std::string render_tui(const TuiFrame& f, bool color) {
  const Style st = style_for(color);
  std::string out;

  // The version and the clock, then a line per daemon. `octomancer status`
  // hides all of this unless something is wrong, because four lines of
  // preamble in front of a table somebody ran a command for is how a status
  // page stops being read. Here the argument runs the other way: the page
  // persists, nothing scrolls past it, and the lines cost a reader nothing
  // after the first glance. What they buy is that the two facts most likely
  // to explain a table with half the room missing are already on screen when
  // it happens, rather than a command away.
  out += fmt("%s%-24s%s %s%s%s\n", st.bold,
             ("octomancer " + f.version).c_str(), st.off, st.dim,
             clock_of(f.now_wall).c_str(), st.off);
  for (const TuiDaemon& d : f.daemons) {
    const AgentSituation sit =
        agent_situation(d.state, d.answering, f.now_wall);
    out += fmt("  %-16s %s%s%s\n", agent_program(d.agent),
               mood_colour(st, sit.mood), sit.said.c_str(), st.off);
  }

  // Both of these are conditions rather than status, and both earn their line:
  // one says nothing will be written to a camera however good the numbers
  // look, and the other says an answer somebody asked for has not happened
  // yet. On a page that redraws, the second one is also the only thing that
  // will show a sync being queued before it starts.
  if (f.dry_run) {
    out += fmt("  %-16s %sDRY RUN -- no camera will be written to%s\n", "",
               st.yellow, st.off);
  }
  if (f.queued > 0) {
    out += fmt("  %-16s %d request%s waiting\n", "queued", f.queued,
               f.queued == 1 ? "" : "s");
  }

  out += "\n";
  out += render_devices(f.view, false, color);
  out += "\n";

  // The one verb this page has. Dim, and on its own line at the bottom, where
  // a footer goes: somebody who already knows how to leave should not have to
  // read past it to get to the table.
  out += fmt("%sq to quit%s\n", st.dim, st.off);
  return out;
}

std::vector<std::string> fit_to_rows(const std::string& page, int rows,
                                     bool color) {
  const Style st = style_for(color);

  std::vector<std::string> lines;
  std::string line;
  for (const char c : page) {
    if (c == '\n') {
      lines.push_back(line);
      line.clear();
    } else {
      line += c;
    }
  }
  // A page that does not end in a newline still has a last line, and it is the
  // footer -- so it is exactly the line that must not be lost here.
  if (!line.empty()) lines.push_back(line);

  if (rows <= 0 || static_cast<int>(lines.size()) <= rows) return lines;
  if (lines.empty()) return lines;
  // One row, and the only thing worth spending it on is the way out.
  if (rows == 1) return {lines.back()};

  // The notice pays for itself out of the page: it takes a row, so a window
  // one line too short loses two lines rather than one. That is worth the
  // trade -- an unexplained short table is the thing being avoided -- but it
  // does mean the count below is never 1, and the singular is kept only
  // because a layout that changes should not have to remember to add it back.
  const int keep = rows - 2;  // the notice and the footer take the other two
  const int dropped = static_cast<int>(lines.size()) - keep - 1;
  std::vector<std::string> out(lines.begin(), lines.begin() + keep);
  out.push_back(fmt("%s... %d more line%s than this window has room for%s",
                    st.dim, dropped, dropped == 1 ? "" : "s", st.off));
  out.push_back(lines.back());
  return out;
}

TuiAction tui_action_for_key(char key) {
  switch (key) {
    case 'q':
    case 'Q':
      return TuiAction::kQuit;
    // Ctrl-C and Ctrl-D, which are what a person who has not read the footer
    // will reach for. Ctrl-C usually arrives as a signal rather than as a
    // byte -- the terminal is left able to send one on purpose -- but it
    // arrives here instead if it ever does not, and a program that has taken
    // over the screen must never be the one that cannot be left.
    case 0x03:
    case 0x04:
      return TuiAction::kQuit;
    // Ctrl-L, which has meant "the screen is lying to me, draw it again"
    // for long enough that it would be rude not to.
    case 0x0c:
      return TuiAction::kRedraw;
    default:
      return TuiAction::kNone;
  }
}

}  // namespace octo
