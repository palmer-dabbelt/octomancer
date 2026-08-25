// One running daemon per lock file.
//
// The control socket already refuses a second daemon that serves one, but that
// is not the same guarantee: a daemon started with control turned off, or an
// older build that had no socket at all, would sit alongside a running one and
// both would connect to the same camera and write the same database. The pair
// that made this worth having disagreed by 55ms about the same reading,
// because each was applying its own measured latency while sharing one file of
// learned biases.
//
// A lock rather than a pid file: the kernel drops it when the process dies, by
// any means, so there is no stale lock to detect and no window where a crashed
// daemon keeps its successor out. The pid is written into the file anyway, but
// only so the refusal can say who is holding it.
#ifndef OCTO_PROCLOCK_H_
#define OCTO_PROCLOCK_H_

#include <string>

namespace octo {

class ProcLock {
 public:
  ProcLock() = default;
  ~ProcLock();
  ProcLock(const ProcLock&) = delete;
  ProcLock& operator=(const ProcLock&) = delete;

  // True if this process now holds the lock. False if somebody else does, or
  // if the file could not be opened at all -- both are reasons not to start,
  // and *err says which. When another process holds it, *holder is their pid,
  // or 0 if the file did not say.
  bool acquire(const std::string& path, long* holder, std::string* err);

  bool held() const { return fd_ >= 0; }
  const std::string& path() const { return path_; }

  // Dropping the lock without exiting. Called by the destructor.
  void release();

 private:
  int fd_ = -1;
  std::string path_;
};

// Where a program's lock lives when nobody says otherwise: beside its socket,
// which is already the directory for one-per-user runtime state.
std::string default_lock_path(const std::string& program);

}  // namespace octo

#endif  // OCTO_PROCLOCK_H_
