// The per-camera configuration file.
//
// The thing this file is really testing is that a permission cannot change by
// accident. Everything else in octomancer measures, learns and adjusts; this
// one setting is a person's decision, and the only acceptable ways for it to
// change are somebody running a command or somebody editing the file. So:
// unknown cameras are off, an unreadable line is an error rather than a shrug,
// and a rewrite keeps every comment and every setting it did not come to
// change.
#include <stdio.h>
#include <unistd.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "camconf.h"
#include "harness.h"

using octo::CamConf;
using octo::CameraConfig;

namespace {

std::string temp_path(const char* tag) {
  return "/tmp/octo-conf-" + std::to_string(getpid()) + "-" + tag + ".conf";
}

void write_file(const std::string& path, const std::string& body) {
  std::ofstream out(path, std::ios::trunc);
  out << body;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream all;
  all << in.rdbuf();
  return all.str();
}

// The most important test here. A camera nobody has said anything about must
// not be written to, because "octomancer found a camera and set its clock" is
// not something that should happen without being asked.
void test_unknown_camera_is_off() {
  const std::string path = temp_path("missing");
  ::unlink(path.c_str());

  CamConf conf;
  std::string err;
  // A file that does not exist is not an error -- it is a fresh install, and
  // it gives the same answer an empty one would.
  CHECK(conf.load(path, &err));
  CHECK(conf.loaded());
  CHECK(!conf.file_exists());
  CHECK(!conf.writes_enabled("anything-at-all"));
  CHECK(!conf.any_writes_enabled());
}

void test_reads_what_was_written_by_hand() {
  const std::string path = temp_path("hand");
  write_file(path,
             "# a comment\n"
             "\n"
             "default writes=off\n"
             "camera AAA writes=on name=Bench\n"
             "camera BBB writes=off\n");

  CamConf conf;
  std::string err;
  CHECK(conf.load(path, &err));
  CHECK(conf.file_exists());
  CHECK(conf.writes_enabled("AAA"));
  CHECK(!conf.writes_enabled("BBB"));
  CHECK(!conf.writes_enabled("CCC"));  // not mentioned: the default
  CHECK(conf.any_writes_enabled());

  const CameraConfig* a = conf.find("AAA");
  CHECK(a != nullptr);
  CHECK_EQ(a->name, std::string("Bench"));
  ::unlink(path.c_str());
}

void test_default_line_applies_to_the_unmentioned() {
  const std::string path = temp_path("default-on");
  write_file(path, "default writes=on\ncamera BBB writes=off\n");

  CamConf conf;
  std::string err;
  CHECK(conf.load(path, &err));
  CHECK(conf.default_writes_enabled());
  CHECK(conf.writes_enabled("never-seen"));
  CHECK(!conf.writes_enabled("BBB"));  // an explicit off beats the default
  ::unlink(path.c_str());
}

void test_spellings_of_yes_and_no() {
  const std::string path = temp_path("spellings");
  write_file(path,
             "camera A writes=on\n"
             "camera B writes=true\n"
             "camera C writes=YES\n"
             "camera D writes=1\n"
             "camera E writes=off\n"
             "camera F writes=false\n"
             "camera G writes=No\n"
             "camera H writes=0\n");

  CamConf conf;
  std::string err;
  CHECK(conf.load(path, &err));
  CHECK(conf.writes_enabled("A"));
  CHECK(conf.writes_enabled("B"));
  CHECK(conf.writes_enabled("C"));
  CHECK(conf.writes_enabled("D"));
  CHECK(!conf.writes_enabled("E"));
  CHECK(!conf.writes_enabled("F"));
  CHECK(!conf.writes_enabled("G"));
  CHECK(!conf.writes_enabled("H"));
  ::unlink(path.c_str());
}

// A value nobody can read is not treated as "off". Silently choosing a
// meaning for what someone typed is how a camera ends up either unsynced all
// night or written to when they said not to; both deserve an error at startup.
void test_unreadable_value_is_an_error() {
  const std::string path = temp_path("bad");
  write_file(path, "camera AAA writes=maybe\n");

  CamConf conf;
  std::string err;
  CHECK(!conf.load(path, &err));
  CHECK(!err.empty());
  // The message has to name the line, or it is not actionable.
  CHECK(err.find("1") != std::string::npos);
  ::unlink(path.c_str());
}

// The opposite case: a *directive* from a newer version is ignored rather than
// fatal, so a file written by a future octomancer does not stop this one from
// starting. Values it cannot read are still errors; keys it has never heard of
// are not.
void test_unknown_keys_are_tolerated() {
  const std::string path = temp_path("future");
  write_file(path,
             "flux capacitor=on\n"
             "camera AAA writes=on lens=50mm\n");

  CamConf conf;
  std::string err;
  CHECK(conf.load(path, &err));
  CHECK(conf.writes_enabled("AAA"));
  ::unlink(path.c_str());
}

// A configuration file that eats your comments is one you stop editing.
void test_rewriting_preserves_the_file_around_it() {
  const std::string path = temp_path("preserve");
  write_file(path,
             "# my notes\n"
             "# the A camera is the one on the tripod\n"
             "default writes=off\n"
             "camera AAA writes=off name=Tripod lens=50mm\n"
             "\n"
             "# BBB is the handheld\n"
             "camera BBB writes=off\n");

  CamConf conf;
  std::string err;
  CHECK(conf.load(path, &err));
  CHECK(conf.set_writes("AAA", "Tripod", true, &err));

  const std::string after = read_file(path);
  CHECK(after.find("# my notes") != std::string::npos);
  CHECK(after.find("the A camera is the one on the tripod") != std::string::npos);
  CHECK(after.find("# BBB is the handheld") != std::string::npos);
  // The unknown key on the line that was rewritten survives too.
  CHECK(after.find("lens=50mm") != std::string::npos);
  CHECK(after.find("camera BBB writes=off") != std::string::npos);
  CHECK(after.find("camera AAA writes=on") != std::string::npos);

  // ...and it means what it says when read back.
  CHECK(conf.writes_enabled("AAA"));
  CHECK(!conf.writes_enabled("BBB"));
  ::unlink(path.c_str());
}

void test_setting_a_new_camera_appends() {
  const std::string path = temp_path("append");
  ::unlink(path.c_str());

  CamConf conf;
  std::string err;
  CHECK(conf.load(path, &err));
  CHECK(conf.set_writes("NEW", "A:1EAE18A7", true, &err));
  CHECK(conf.writes_enabled("NEW"));

  const std::string after = read_file(path);
  // A file created from nothing explains itself, because the next person to
  // open it has never seen this format.
  CHECK(after.find("# octomancer camera configuration") != std::string::npos);
  CHECK(after.find("camera NEW writes=on") != std::string::npos);
  CHECK(after.find("name=A:1EAE18A7") != std::string::npos);

  // A second load from disk agrees with what the writer thought.
  CamConf again;
  CHECK(again.load(path, &err));
  CHECK(again.writes_enabled("NEW"));
  ::unlink(path.c_str());
}

void test_toggling_back_off_does_not_duplicate_the_line() {
  const std::string path = temp_path("toggle");
  ::unlink(path.c_str());

  CamConf conf;
  std::string err;
  CHECK(conf.load(path, &err));
  CHECK(conf.set_writes("AAA", "One", true, &err));
  CHECK(conf.set_writes("AAA", "One", false, &err));
  CHECK(conf.set_writes("AAA", "One", true, &err));

  const std::string after = read_file(path);
  size_t count = 0;
  for (size_t at = after.find("camera AAA"); at != std::string::npos;
       at = after.find("camera AAA", at + 1)) {
    ++count;
  }
  CHECK_EQ(static_cast<int>(count), 1);
  CHECK(conf.writes_enabled("AAA"));
  ::unlink(path.c_str());
}

// Camera names come off the air and are user-set, so they can contain a space
// or an '=' -- either of which would turn one line into two settings.
void test_hostile_camera_name_cannot_forge_a_setting() {
  const std::string path = temp_path("hostile");
  ::unlink(path.c_str());

  CamConf conf;
  std::string err;
  CHECK(conf.load(path, &err));
  CHECK(conf.set_writes("AAA", "evil writes=on", false, &err));

  // Written, then read back by a fresh parser: the smuggled writes=on must not
  // have become this camera's setting.
  CamConf again;
  CHECK(again.load(path, &err));
  CHECK(!again.writes_enabled("AAA"));
  CHECK(!again.any_writes_enabled());
  ::unlink(path.c_str());
}

void test_reload_picks_up_an_edit() {
  const std::string path = temp_path("reload");
  write_file(path, "camera AAA writes=off\n");

  CamConf conf;
  std::string err;
  CHECK(conf.load(path, &err));
  CHECK(!conf.writes_enabled("AAA"));

  write_file(path, "camera AAA writes=on\n");
  CHECK(conf.reload(&err));
  CHECK(conf.writes_enabled("AAA"));

  // A reload that cannot parse leaves the caller to decide; it reports the
  // failure rather than quietly emptying itself.
  write_file(path, "camera AAA writes=perhaps\n");
  CHECK(!conf.reload(&err));
  CHECK(!err.empty());
  ::unlink(path.c_str());
}

void test_no_path_means_nothing_is_permitted() {
  CamConf conf;
  std::string err;
  CHECK(conf.load("", &err));
  CHECK(conf.loaded());
  CHECK(!conf.writes_enabled("anything"));
  CHECK(!conf.any_writes_enabled());
  // ...and there is nowhere to write it, which is an error rather than a
  // silent no-op.
  CHECK(!conf.set_writes("AAA", "x", true, &err));
}

}  // namespace

int main() {
  test_unknown_camera_is_off();
  test_reads_what_was_written_by_hand();
  test_default_line_applies_to_the_unmentioned();
  test_spellings_of_yes_and_no();
  test_unreadable_value_is_an_error();
  test_unknown_keys_are_tolerated();
  test_rewriting_preserves_the_file_around_it();
  test_setting_a_new_camera_appends();
  test_toggling_back_off_does_not_duplicate_the_line();
  test_hostile_camera_name_cannot_forge_a_setting();
  test_reload_picks_up_an_edit();
  test_no_path_means_nothing_is_permitted();
  return octotest::report("test_camconf");
}
