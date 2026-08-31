// The page `octomancer tui` draws, without a terminal to draw it on.
//
// The reason these are worth writing: the interesting parts of a full-screen
// program are the ones nobody looks at. Somebody will read the table on every
// run, and would notice the day it went wrong. Nobody reads the footer after
// the first time -- so the day it stops being drawn is the day a person is
// stuck in a program with no visible way out, and the tests below are what
// stands between here and there.
//
// So: the way out is on the page, it survives a window too short to hold the
// page, and `q` means what the footer says it means.
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "agents.h"
#include "devices.h"
#include "harness.h"
#include "tui.h"

using octo::AgentMood;
using octo::AgentState;
using octo::DeviceKind;
using octo::DeviceRow;
using octo::LinkState;
using octo::TuiAction;
using octo::TuiDaemon;
using octo::TuiFrame;
using octo::WarnLevel;

namespace {

// 2023-11-14 22:13:20 UTC, which the tests read back as a wall clock. main()
// pins TZ so the corner of the page is the same string everywhere.
const double kNow = 1700000000.0;

TuiDaemon daemon(octo::Agent which, bool answering, bool running) {
  TuiDaemon d;
  d.agent = which;
  d.answering = answering;
  d.state.installed = true;
  d.state.loaded = true;
  d.state.running = running;
  d.state.pid = running ? 4321 : 0;
  return d;
}

DeviceRow box(const std::string& name, double offset) {
  DeviceRow r;
  r.kind = DeviceKind::kTentacle;
  r.id = name;
  r.name = name;
  r.link = LinkState::kOnTheAir;
  r.has_offset = true;
  r.offset_s = offset;
  r.has_age = true;
  r.age_s = 1.0;
  r.has_rssi = true;
  r.rssi = -55;
  return r;
}

TuiFrame frame_with(int devices) {
  TuiFrame f;
  f.version = "9.9.9";
  f.now_wall = kNow;
  f.daemons.push_back(daemon(octo::Agent::kBench, true, true));
  f.daemons.push_back(daemon(octo::Agent::kSync, true, true));
  for (int i = 0; i < devices; ++i) {
    f.view.rows.push_back(box("box-" + std::to_string(i), 0.001 * i));
  }
  octo::RadioView here;
  here.local = true;
  here.name = "this Mac";
  here.way = "local";
  here.has_canonical = devices > 0;
  here.contributing = devices;
  f.view.radios.push_back(here);
  f.view.has_canonical = devices > 0;
  f.view.contributing = devices;
  f.view.canonical_source = "octomancerd";
  return f;
}

bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

std::string strip_escapes(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\033') {
      while (i < s.size() && s[i] != 'm') ++i;
      continue;
    }
    out += s[i];
  }
  return out;
}

std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  std::string line;
  for (const char c : s) {
    if (c == '\n') {
      out.push_back(line);
      line.clear();
    } else {
      line += c;
    }
  }
  if (!line.empty()) out.push_back(line);
  return out;
}

// ------------------------------------------------------------------- tests

void test_the_page_says_how_to_leave_it() {
  const std::string page = octo::render_tui(frame_with(2), false);
  const std::vector<std::string> lines = split_lines(page);
  CHECK(!lines.empty());
  // Last line, every time. A footer somewhere in the middle of a table is not
  // a footer, and a person scanning the bottom of the window has to find it
  // there.
  CHECK_STR(lines.back(), "q to quit");
}

void test_the_page_carries_the_version_the_radios_and_the_table() {
  const std::string page = octo::render_tui(frame_with(2), false);
  CHECK(contains(page, "octomancer TUI (v9.9.9)"));
  CHECK(contains(page, "22:13:20"));
  // The date as well as the time. A camera's timecode is written by setting
  // its date, so what day this machine thinks it is has consequences, and it
  // is not a question anybody thinks to ask until it is already wrong.
  CHECK(contains(page, "2023-11-14"));
  // Daemons that are fine say nothing. Four lines of preamble is how the
  // table underneath stops being read, and "up 39s" was never the thing
  // anybody came to the page for.
  CHECK(!contains(page, "octomancerd"));
  CHECK(!contains(page, "octomancer-sync"));
  // The radios do get a permanent section: on a page that persists, the axis
  // every offset below is measured against is worth one line.
  CHECK(contains(page, "RADIO"));
  CHECK(contains(page, "this Mac"));
  // By the heading line: "TIMECODE" is a column in the RADIO section too.
  CHECK(contains(page, "\nTIMECODE "));
  CHECK(contains(page, "box-0"));
  CHECK(contains(page, "box-1"));
}

void test_a_daemon_that_is_not_answering_says_which_kind_of_wrong() {
  // Not running at all, which `octomancer start` fixes.
  TuiFrame f = frame_with(1);
  f.daemons[0] = daemon(octo::Agent::kBench, false, false);
  CHECK(contains(octo::render_tui(f, false), "octomancer start"));

  // Running and not answering, which it does not fix -- a different problem
  // wearing the same "the table is missing things" face.
  f.daemons[0] = daemon(octo::Agent::kBench, false, true);
  const std::string page = octo::render_tui(f, false);
  CHECK(contains(page, "pid 4321"));
  CHECK(contains(page, "socket does not answer"));
}

void test_the_conditions_appear_only_when_they_hold() {
  TuiFrame f = frame_with(1);
  std::string page = octo::render_tui(f, false);
  CHECK(!contains(page, "DRY RUN"));
  CHECK(!contains(page, "waiting"));

  f.dry_run = true;
  f.queued = 1;
  page = octo::render_tui(f, false);
  CHECK(contains(page, "DRY RUN"));
  CHECK(contains(page, "1 request waiting"));

  f.queued = 3;
  CHECK(contains(octo::render_tui(f, false), "3 requests waiting"));
}

void test_colour_only_adds_escapes() {
  const TuiFrame f = frame_with(3);
  const std::string plain = octo::render_tui(f, false);
  const std::string painted = octo::render_tui(f, true);
  CHECK(painted != plain);
  CHECK_STR(strip_escapes(painted), plain);
}

void test_a_page_that_fits_is_left_alone() {
  const std::string page = octo::render_tui(frame_with(2), false);
  const size_t lines = split_lines(page).size();
  const std::vector<std::string> fitted =
      octo::fit_to_rows(page, static_cast<int>(lines), false);
  CHECK_EQ(fitted.size(), lines);
  CHECK_STR(fitted.back(), "q to quit");

  // A window we could not measure is not a short window.
  CHECK_EQ(octo::fit_to_rows(page, 0, false).size(), lines);
}

void test_a_short_window_keeps_the_way_out_and_admits_the_cut() {
  const std::string page = octo::render_tui(frame_with(40), false);
  const size_t full = split_lines(page).size();
  CHECK(full > 10);

  const std::vector<std::string> fitted = octo::fit_to_rows(page, 10, false);
  CHECK_EQ(fitted.size(), static_cast<size_t>(10));
  CHECK_STR(fitted.back(), "q to quit");
  CHECK_STR(fitted.front(), split_lines(page).front());
  // The count is of what was dropped, not of what was kept: 10 rows hold 8
  // lines of page, the notice and the footer.
  CHECK_STR(fitted[8], "... " + std::to_string(full - 9) +
                           " more lines than this window has room for");
}

void test_one_row_is_spent_on_the_way_out() {
  const std::string page = octo::render_tui(frame_with(40), false);
  const std::vector<std::string> fitted = octo::fit_to_rows(page, 1, false);
  CHECK_EQ(fitted.size(), static_cast<size_t>(1));
  CHECK_STR(fitted[0], "q to quit");
}

void test_the_notice_pays_for_itself_out_of_the_page() {
  const std::string page = octo::render_tui(frame_with(3), false);
  const int full = static_cast<int>(split_lines(page).size());

  // One row short of the page, and two lines go: the last one, and one more
  // to make room for the line that says so. That is the honest price of the
  // notice, and the count has to name both -- a page that dropped two lines
  // and admitted to one would be the exact failure the notice exists to
  // prevent.
  const std::vector<std::string> fitted =
      octo::fit_to_rows(page, full - 1, false);
  CHECK_EQ(fitted.size(), static_cast<size_t>(full - 1));
  CHECK(contains(fitted[fitted.size() - 2], "2 more lines than"));
  CHECK_STR(fitted.back(), "q to quit");
}

void test_q_quits_and_almost_nothing_else_does_anything() {
  CHECK(octo::tui_action_for_key('q') == TuiAction::kQuit);
  CHECK(octo::tui_action_for_key('Q') == TuiAction::kQuit);
  // What somebody reaches for before reading the footer.
  CHECK(octo::tui_action_for_key('\003') == TuiAction::kQuit);
  CHECK(octo::tui_action_for_key('\004') == TuiAction::kQuit);
  CHECK(octo::tui_action_for_key('\014') == TuiAction::kRedraw);

  // Everything else, and this half of the test is the point of it: a page with
  // one verb should not acquire more by accident, and an arrow key -- which
  // arrives as an escape and two ordinary letters -- must not quit because one
  // of those letters was next to a real binding.
  const char* inert = "abcdefghijklmnoprstuvwxyz0123456789 \033[ABCD\r\n\t";
  for (const char* c = inert; *c != '\0'; ++c) {
    CHECK(octo::tui_action_for_key(*c) == TuiAction::kNone);
  }
}

// The sentence about a daemon is shared with `octomancer status`, so it is
// tested here where the rest of the page is: two surfaces disagreeing about
// whether a daemon is up is the kind of thing nobody thinks to suspect.
void test_the_daemon_sentence_is_one_sentence() {
  AgentState up;
  up.installed = up.loaded = up.running = true;
  up.pid = 11;
  up.has_started = true;
  up.started_wall = kNow - 3600.0;
  const octo::AgentSituation fine = octo::agent_situation(up, true, kNow);
  CHECK(fine.mood == AgentMood::kFine);
  CHECK_STR(fine.said, "answering, up 1h00m");

  const octo::AgentSituation bad = octo::agent_situation(up, false, kNow);
  CHECK(bad.mood == AgentMood::kBad);
  CHECK(contains(bad.said, "pid 11"));

  AgentState down;
  down.installed = true;
  const octo::AgentSituation warn = octo::agent_situation(down, false, kNow);
  CHECK(warn.mood == AgentMood::kWarn);
  CHECK(contains(warn.said, "octomancer start"));

  AgentState never;
  const octo::AgentSituation gone = octo::agent_situation(never, false, kNow);
  CHECK(gone.mood == AgentMood::kWarn);
  CHECK(contains(gone.said, "not installed"));
}

}  // namespace

int main() {
  setenv("TZ", "UTC", 1);
  tzset();

  test_the_page_says_how_to_leave_it();
  test_the_page_carries_the_version_the_radios_and_the_table();
  test_a_daemon_that_is_not_answering_says_which_kind_of_wrong();
  test_the_conditions_appear_only_when_they_hold();
  test_colour_only_adds_escapes();
  test_a_page_that_fits_is_left_alone();
  test_a_short_window_keeps_the_way_out_and_admits_the_cut();
  test_one_row_is_spent_on_the_way_out();
  test_the_notice_pays_for_itself_out_of_the_page();
  test_q_quits_and_almost_nothing_else_does_anything();
  test_the_daemon_sentence_is_one_sentence();
  return octotest::report("tui");
}
