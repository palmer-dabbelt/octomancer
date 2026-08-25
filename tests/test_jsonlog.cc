// Log rotation.
//
// Rotation is the one thing in the logging path that can destroy data, so it
// is worth testing against a real filesystem rather than a mock: the failure
// mode that matters is a rename that loses a generation, and a mock would
// happily agree that it did not.
#include "jsonlog.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "harness.h"

namespace {

// Somewhere to make a mess. $TMPDIR is set for `make check`, and the pid keeps
// two runs of the suite from colliding.
std::string scratch(const char* name) {
  const char* base = getenv("TMPDIR");
  std::string dir = base && *base ? base : "/tmp";
  if (dir.back() != '/') dir += '/';
  dir += "octo-test-" + std::to_string(::getpid()) + "-" + name;
  ::mkdir(dir.c_str(), 0700);
  return dir;
}

void remove_tree(const std::string& dir) {
  // Only ever one flat directory of small files, so no recursion is needed.
  for (const char* suffix : {"", ".1", ".2", ".3", ".4", ".5", ".6", ".7",
                            ".8", ".9", ".10", ".11", ".12"}) {
    ::unlink((dir + "/log" + suffix).c_str());
  }
  ::rmdir(dir.c_str());
}

bool exists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

long long size_of(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return -1;
  return static_cast<long long>(st.st_size);
}

int count_lines(const std::string& path) {
  FILE* f = ::fopen(path.c_str(), "r");
  if (f == nullptr) return -1;
  int n = 0;
  int c;
  while ((c = fgetc(f)) != EOF) {
    if (c == '\n') n++;
  }
  ::fclose(f);
  return n;
}

void test_rotation_keeps_generations() {
  const std::string dir = scratch("rotate");
  const std::string path = dir + "/log";

  octo::JsonLog log;
  octo::Rotation r;
  r.max_bytes = 400;  // a handful of records
  r.keep = 3;
  log.set_rotation(r);
  std::string err;
  CHECK(log.open(path, &err));

  for (int i = 0; i < 200; ++i) {
    log.record("cycle", "\"n\":" + std::to_string(i));
  }
  log.close();

  CHECK(exists(path));
  CHECK(exists(path + ".1"));
  CHECK(exists(path + ".2"));
  CHECK(exists(path + ".3"));
  // ...and no further: keep is a ceiling, not a suggestion.
  CHECK(!exists(path + ".4"));

  // Every generation stayed under the size that triggered its rotation, plus
  // the one record that crossed it.
  CHECK(size_of(path + ".1") >= 400);
  CHECK(size_of(path + ".1") < 400 * 2);

  remove_tree(dir);
}

void test_oldest_generation_is_the_one_dropped() {
  const std::string dir = scratch("order");
  const std::string path = dir + "/log";

  // Hand-made generations, so which one survives is unambiguous.
  for (const char* g : {".1", ".2"}) {
    FILE* f = ::fopen((path + g).c_str(), "w");
    ::fputs(g, f);
    ::fclose(f);
  }
  FILE* f = ::fopen(path.c_str(), "w");
  ::fputs("live", f);
  ::fclose(f);

  octo::rotate_files(path, 2);

  // live -> .1, old .1 -> .2, old .2 dropped.
  CHECK(!exists(path));
  char buf[16] = {0};
  f = ::fopen((path + ".1").c_str(), "r");
  CHECK(f != nullptr);
  if (f) {
    CHECK(fgets(buf, sizeof buf, f) != nullptr);
    ::fclose(f);
  }
  CHECK_STR(buf, "live");

  f = ::fopen((path + ".2").c_str(), "r");
  CHECK(f != nullptr);
  if (f) {
    buf[0] = 0;
    CHECK(fgets(buf, sizeof buf, f) != nullptr);
    ::fclose(f);
  }
  CHECK_STR(buf, ".1");
  CHECK(!exists(path + ".3"));

  remove_tree(dir);
}

void test_disabled_rotation_never_rotates() {
  const std::string dir = scratch("nokeep");
  const std::string path = dir + "/log";

  octo::JsonLog log;
  octo::Rotation r;
  r.max_bytes = 0.0;  // the probe modes want this
  log.set_rotation(r);
  std::string err;
  CHECK(log.open(path, &err));
  for (int i = 0; i < 100; ++i) log.record("cycle", "\"n\":1");
  log.close();

  CHECK(!exists(path + ".1"));
  CHECK_EQ(count_lines(path), 100);
  CHECK_EQ(log.rotations(), 0u);

  remove_tree(dir);
}

// A restart must not reset the size tally, or a daemon restarted every few
// minutes would append forever and never cross its own threshold.
void test_reopen_counts_what_is_already_there() {
  const std::string dir = scratch("reopen");
  const std::string path = dir + "/log";

  octo::Rotation r;
  r.max_bytes = 500;
  r.keep = 2;

  for (int run = 0; run < 6; ++run) {
    octo::JsonLog log;
    log.set_rotation(r);
    std::string err;
    CHECK(log.open(path, &err));
    for (int i = 0; i < 3; ++i) log.record("cycle", "\"run\":1");
    log.close();
  }

  CHECK(exists(path + ".1"));
  CHECK(size_of(path) < 500 * 2);

  remove_tree(dir);
}

// Nothing may be lost across a rotation: every record written has to be in
// exactly one of the files afterwards.
void test_no_record_is_lost() {
  const std::string dir = scratch("nolose");
  const std::string path = dir + "/log";

  octo::JsonLog log;
  octo::Rotation r;
  // Sized so several rotations happen but comfortably fewer than `keep`, so
  // nothing is dropped and the accounting below has to balance exactly. A
  // record is around 75 bytes.
  r.max_bytes = 300;
  r.keep = 12;
  log.set_rotation(r);
  std::string err;
  CHECK(log.open(path, &err));

  const int kRecords = 20;
  for (int i = 0; i < kRecords; ++i) log.record("cycle", "\"n\":1");
  log.close();

  int total = count_lines(path);
  int rotated = 0;
  for (int g = 1; g <= r.keep; ++g) {
    const std::string gen = path + "." + std::to_string(g);
    if (!exists(gen)) continue;
    total += count_lines(gen);
    rotated++;
  }
  CHECK(rotated > 1);                                  // it really did rotate
  CHECK_EQ(rotated, static_cast<int>(log.rotations()));  // nothing dropped
  // Each rotation writes one "rotated" marker of its own on the way out.
  CHECK_EQ(total, kRecords + rotated);

  remove_tree(dir);
}

}  // namespace

int main() {
  test_rotation_keeps_generations();
  test_oldest_generation_is_the_one_dropped();
  test_disabled_rotation_never_rotates();
  test_reopen_counts_what_is_already_there();
  test_no_record_is_lost();
  return octotest::report("test_jsonlog");
}
