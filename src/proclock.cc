#include "proclock.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace octo {
namespace {

// Every missing component, not just the last one. The default path is three
// deep under a home directory, and on a home that does not already have an
// "Application Support" -- a fresh account, or a test -- creating only the
// leaf fails with ENOENT and the daemon does not start at all.
bool make_parents(const std::string& path, std::string* err) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) return true;
  const std::string dir = path.substr(0, slash);

  for (size_t at = dir.find('/', 1); ; at = dir.find('/', at + 1)) {
    const std::string part =
        at == std::string::npos ? dir : dir.substr(0, at);
    if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) {
      if (err) *err = "cannot create " + part + ": " + std::strerror(errno);
      return false;
    }
    if (at == std::string::npos) break;
  }
  return true;
}

// Whatever the holder wrote, if it is a number. Read positionally so this does
// not disturb the file offset, and treated as a hint throughout: a holder that
// was killed between opening the file and writing its pid still holds the
// lock, and is still a reason to refuse to start.
long read_holder_pid(int fd) {
  char buf[32];
  const ssize_t n = ::pread(fd, buf, sizeof(buf) - 1, 0);
  if (n <= 0) return 0;
  buf[n] = '\0';
  return std::strtol(buf, nullptr, 10);
}

}  // namespace

ProcLock::~ProcLock() { release(); }

void ProcLock::release() {
  if (fd_ < 0) return;
  // Closing drops the lock. The file is deliberately left behind: unlinking it
  // would let the next daemon create and lock a new file at the same path
  // while a third still holds this one, which is the classic way a lock file
  // stops being a lock.
  ::close(fd_);
  fd_ = -1;
}

bool ProcLock::acquire(const std::string& path, long* holder, std::string* err) {
  release();
  if (holder) *holder = 0;
  if (err) err->clear();
  path_ = path;

  if (!make_parents(path, err)) return false;

  const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (err) *err = "cannot open " + path + ": " + std::strerror(errno);
    return false;
  }

  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int saved = errno;
    const long pid = read_holder_pid(fd);
    ::close(fd);
    if (holder) *holder = pid;
    if (err) {
      if (saved == EWOULDBLOCK) {
        *err = "another one is already running";
        if (pid > 0) *err += " (pid " + std::to_string(pid) + ")";
      } else {
        *err = "cannot lock " + path + ": " + std::strerror(saved);
      }
    }
    return false;
  }

  // Ours. Leave the pid behind for whoever is refused next.
  if (::ftruncate(fd, 0) == 0) {
    char line[32];
    const int n = std::snprintf(line, sizeof(line), "%ld\n",
                                static_cast<long>(::getpid()));
    if (n > 0) {
      const ssize_t wrote = ::pwrite(fd, line, static_cast<size_t>(n), 0);
      (void)wrote;  // A pid we failed to record costs a worse message, no more.
    }
  }

  fd_ = fd;
  return true;
}

std::string default_lock_path(const std::string& program) {
  const char* home = std::getenv("HOME");
  const std::string base = home && *home ? home : "/tmp";
  return base + "/Library/Application Support/octomancer/" + program + ".lock";
}

}  // namespace octo
