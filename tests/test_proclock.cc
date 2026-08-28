// The guard that keeps a second daemon from starting.
//
// Two sync daemons ran side by side for two hours because the older of them
// predated the control socket that would have refused it. They both connected
// to the same camera and shared one file of learned biases, and reported
// errors 55ms apart for the same reading. None of that is visible as a
// failure -- both look like they are working -- so it is worth a test.
#include <string>

#include <csignal>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "proclock.h"
#include "harness.h"

using octo::ProcLock;

namespace {

std::string scratch(const std::string& tag) {
  return "/tmp/octo-test-lock-" + tag + "-" + std::to_string(getpid());
}

void test_a_second_lock_is_refused() {
  const std::string path = scratch("dup") + ".lock";
  ::unlink(path.c_str());

  ProcLock first;
  long holder = -1;
  std::string err;
  CHECK(first.acquire(path, &holder, &err));
  CHECK(first.held());
  CHECK(err.empty());

  // flock is held by the open file description, so a second lock on the same
  // path conflicts even from this process -- which is what makes this
  // testable without forking.
  ProcLock second;
  holder = -1;
  CHECK(!second.acquire(path, &holder, &err));
  CHECK(!second.held());
  CHECK(!err.empty());
  // It says who has it, so the refusal is actionable rather than just a no.
  CHECK_EQ(holder, static_cast<long>(getpid()));

  ::unlink(path.c_str());
}

void test_releasing_lets_the_next_one_in() {
  const std::string path = scratch("release") + ".lock";
  ::unlink(path.c_str());

  long holder = -1;
  std::string err;
  {
    ProcLock first;
    CHECK(first.acquire(path, &holder, &err));
  }  // destructor releases

  ProcLock second;
  CHECK(second.acquire(path, &holder, &err));
  CHECK(second.held());

  second.release();
  CHECK(!second.held());
  // Releasing twice is not a crash.
  second.release();

  ::unlink(path.c_str());
}

// The reason this is a lock and not a pid file: a daemon that is killed
// outright leaves the file behind, and the next one must still start. A pid
// file would need to decide whether pid 1234 is the old daemon or something
// else that has since been given that number.
void test_a_killed_holder_does_not_lock_anyone_out() {
  const std::string path = scratch("killed") + ".lock";
  ::unlink(path.c_str());

  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    ProcLock lock;
    long h = 0;
    std::string e;
    if (!lock.acquire(path, &h, &e)) _exit(2);
    ::pause();   // hold it until killed
    _exit(3);
  }

  // Wait for the child to actually hold it, rather than guessing at a sleep.
  // The deadline is deliberately far longer than the wait can honestly need:
  // a working fork gets there in milliseconds, so a generous budget costs
  // nothing except how long a genuinely broken lock takes to say so, whereas
  // a tight one fails on a machine that is merely busy.
  const int kStepMs = 5;
  const int kDeadlineMs = 30000;
  ProcLock probe;
  bool taken = false;
  for (int i = 0; i < kDeadlineMs / kStepMs && !taken; ++i) {
    long h = 0;
    std::string e;
    if (!probe.acquire(path, &h, &e) && h == static_cast<long>(child)) {
      taken = true;
      break;
    }
    probe.release();
    ::usleep(kStepMs * 1000);
  }
  CHECK(taken);

  ::kill(child, SIGKILL);
  int status = 0;
  ::waitpid(child, &status, 0);

  // The file is still there, and it still says the dead pid.
  struct stat st;
  CHECK_EQ(::stat(path.c_str(), &st), 0);

  ProcLock after;
  long holder = -1;
  std::string err;
  CHECK(after.acquire(path, &holder, &err));
  CHECK(after.held());

  ::unlink(path.c_str());
}

// The default path is three directories deep under a home that may only have
// the first of them. Creating just the leaf fails with ENOENT, and the daemon
// that cannot take its lock does not start.
void test_missing_parent_directories_are_created() {
  const std::string root = scratch("deep");
  const std::string path = root + "/one/two/three/octomancer-sync.lock";

  ProcLock lock;
  long holder = -1;
  std::string err;
  CHECK(lock.acquire(path, &holder, &err));
  CHECK(lock.held());
  CHECK(err.empty());

  struct stat st;
  CHECK_EQ(::stat(path.c_str(), &st), 0);

  lock.release();
  ::unlink(path.c_str());
  ::rmdir((root + "/one/two/three").c_str());
  ::rmdir((root + "/one/two").c_str());
  ::rmdir((root + "/one").c_str());
  ::rmdir(root.c_str());
}

void test_an_unwritable_directory_is_a_refusal_not_a_crash() {
  ProcLock lock;
  long holder = -1;
  std::string err;
  CHECK(!lock.acquire("/no/such/place/octomancer.lock", &holder, &err));
  CHECK(!lock.held());
  CHECK(!err.empty());
}

void test_default_paths_are_per_program() {
  const std::string sync = octo::default_lock_path("octomancer-sync");
  const std::string bench = octo::default_lock_path("octomancerd");
  CHECK(sync != bench);
  CHECK(sync.find("octomancer-sync.lock") != std::string::npos);
  CHECK(bench.find("octomancerd.lock") != std::string::npos);
}

}  // namespace

int main() {
  test_a_second_lock_is_refused();
  test_releasing_lets_the_next_one_in();
  test_a_killed_holder_does_not_lock_anyone_out();
  test_missing_parent_directories_are_created();
  test_an_unwritable_directory_is_a_refusal_not_a_crash();
  test_default_paths_are_per_program();
  return octotest::report("test_proclock");
}
