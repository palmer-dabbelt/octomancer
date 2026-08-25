// Append-only JSONL, so a night of running leaves something to fit a line to.
//
// Append, never truncate: restarting the agent must not destroy the history
// that makes drift measurable in the first place. Rotation is the one
// exception, and it moves the history aside rather than deleting it.
#ifndef OCTO_JSONLOG_H
#define OCTO_JSONLOG_H

#include <cstdint>
#include <cstdio>
#include <string>

namespace octo {

// Keeping a log from growing without bound.
//
// Size-based rather than time-based: what matters is the disk, and these
// programs write at a rate that depends on how often they find something to
// say. A day of a quiet bench is a few hundred kilobytes; a day of a camera
// coming and going is several times that.
struct Rotation {
  // 0 disables rotation entirely, which is what a short-lived probe wants.
  double max_bytes = 16.0 * 1024 * 1024;
  // Generations kept beside the live file, as PATH.1 ... PATH.keep. The oldest
  // is deleted, so the total on disk is bounded at (keep + 1) * max_bytes.
  int keep = 5;

  bool enabled() const { return max_bytes > 0.0 && keep > 0; }
};

// Make a string safe to sit between quotes in a JSON document.
//
// Device names are user-set and arrive from the air, so they are assumed
// hostile: a name with a quote or a backslash in it must not be able to
// produce a line that parses as something other than what was meant.
std::string json_escape(const std::string& in);

// mkdir -p for everything above `path`, best-effort and mode 0700. Shared so
// that anything writing a file under a directory the user may not have
// created yet does it the same way.
void make_parents(const std::string& path);

// PATH.keep is removed, PATH.n becomes PATH.n+1, and PATH becomes PATH.1.
//
// Exposed because the caller has to be the one holding the file: renaming a
// file another process has open leaves that process writing to an inode with
// no name, which is how rotation silently loses a night of data.
void rotate_files(const std::string& path, int keep);

class JsonLog {
 public:
  JsonLog() = default;
  ~JsonLog();

  JsonLog(const JsonLog&) = delete;
  JsonLog& operator=(const JsonLog&) = delete;

  // An empty path disables logging. Creates parent directories.
  bool open(const std::string& path, std::string* err);
  bool enabled() const { return file_ != nullptr; }

  // Takes effect from the next record. Rotation is checked after each write
  // rather than before, so a single record is never split across two files.
  void set_rotation(const Rotation& r) { rotation_ = r; }
  const Rotation& rotation() const { return rotation_; }

  // How many times this instance has rotated, for the summary a daemon logs
  // about itself.
  uint64_t rotations() const { return rotations_; }
  double bytes() const { return static_cast<double>(bytes_); }

  // `fields` is the interior of a JSON object, without the braces: this
  // prepends the event name and both clocks and writes one line.
  void record(const char* event, const std::string& fields);

  void close();

 private:
  void maybe_rotate();

  std::FILE* file_ = nullptr;
  std::string path_;
  Rotation rotation_;
  long long bytes_ = 0;
  uint64_t rotations_ = 0;
};

// The human-readable output, when the program has been asked to own it.
//
// Normally stdout goes wherever it was pointed -- a terminal, a launchd
// StandardOutPath, a shell redirect -- and none of those can be rotated from
// here, because the writer holds the descriptor. Given --console the program
// opens the file itself, which is what makes rotating it safe.
class ConsoleLog {
 public:
  ~ConsoleLog();

  ConsoleLog(const ConsoleLog&) = delete;
  ConsoleLog& operator=(const ConsoleLog&) = delete;
  ConsoleLog() = default;

  // Points stdout and stderr at `path`. Both, and to the same file: a reader
  // going through the log wants the radio warnings interleaved with the cycle
  // that provoked them, not in a second file with no timestamps to join on.
  bool open(const std::string& path, const Rotation& rotation, std::string* err);
  bool enabled() const { return !path_.empty(); }

  // Call from the main loop, between cycles. Rotating mid-line would split a
  // sentence across two files.
  void maybe_rotate();

  uint64_t rotations() const { return rotations_; }

 private:
  bool point_streams_at_file(std::string* err);

  std::string path_;
  Rotation rotation_;
  uint64_t rotations_ = 0;
};

}  // namespace octo

#endif  // OCTO_JSONLOG_H
