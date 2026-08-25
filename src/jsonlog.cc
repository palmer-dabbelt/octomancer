#include "jsonlog.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "timeutil.h"

namespace octo {

namespace {

void make_parents(const std::string& path) {
  size_t at = path.find('/', 1);
  while (at != std::string::npos) {
    ::mkdir(path.substr(0, at).c_str(), 0700);
    at = path.find('/', at + 1);
  }
}

std::string generation(const std::string& path, int n) {
  return path + "." + std::to_string(n);
}

}  // namespace

void rotate_files(const std::string& path, int keep) {
  if (keep <= 0) return;
  ::unlink(generation(path, keep).c_str());
  // Downwards, so a generation is never overwritten before it has been moved.
  for (int n = keep - 1; n >= 1; --n) {
    ::rename(generation(path, n).c_str(), generation(path, n + 1).c_str());
  }
  ::rename(path.c_str(), generation(path, 1).c_str());
}

JsonLog::~JsonLog() { close(); }

bool JsonLog::open(const std::string& path, std::string* err) {
  close();
  if (path.empty()) return true;
  make_parents(path);
  file_ = std::fopen(path.c_str(), "a");
  if (file_ == nullptr) {
    if (err) *err = "cannot open " + path + ": " + strerror(errno);
    return false;
  }
  path_ = path;
  // Start counting from what is already there, or a restart every few minutes
  // would keep resetting the tally and the file would never rotate.
  const long long at = ::ftello(file_);
  bytes_ = at > 0 ? at : 0;
  return true;
}

void JsonLog::record(const char* event, const std::string& fields) {
  if (file_ == nullptr) return;
  const double wall = wall_now();
  int wrote = std::fprintf(file_, "{\"event\":\"%s\",\"wall\":%.3f,\"local\":\"%s\"",
                           event, wall, format_local(wall).c_str());
  if (wrote < 0) wrote = 0;
  if (!fields.empty()) {
    std::fputc(',', file_);
    std::fwrite(fields.data(), 1, fields.size(), file_);
    wrote += 1 + static_cast<int>(fields.size());
  }
  std::fputs("}\n", file_);
  wrote += 2;
  bytes_ += wrote;
  // Flush every line. An overnight run that ends in a crash or a closed lid
  // must still have its data on disk, and one fflush a minute costs nothing.
  std::fflush(file_);
  maybe_rotate();
}

void JsonLog::maybe_rotate() {
  if (file_ == nullptr || !rotation_.enabled()) return;
  if (static_cast<double>(bytes_) < rotation_.max_bytes) return;

  std::fclose(file_);
  file_ = nullptr;
  rotate_files(path_, rotation_.keep);

  file_ = std::fopen(path_.c_str(), "a");
  bytes_ = 0;
  rotations_ += 1;
  if (file_ == nullptr) {
    // Losing the log is not worth losing the daemon over: the clock keeps
    // being corrected either way, and this is the one line that says so.
    std::fprintf(stderr, "octomancer: cannot reopen %s after rotating: %s\n",
                 path_.c_str(), strerror(errno));
    return;
  }
  record("rotated", "\"kept\":" + std::to_string(rotation_.keep));
}

void JsonLog::close() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
  path_.clear();
  bytes_ = 0;
}

// ------------------------------------------------------------------ console

ConsoleLog::~ConsoleLog() {
  if (!path_.empty()) std::fflush(stdout);
}

bool ConsoleLog::point_streams_at_file(std::string* err) {
  if (std::freopen(path_.c_str(), "a", stdout) == nullptr) {
    if (err) *err = "cannot open " + path_ + ": " + strerror(errno);
    return false;
  }
  // stderr shares the descriptor rather than opening the file twice, so the
  // two streams cannot land at different offsets and overwrite each other.
  if (::dup2(fileno(stdout), fileno(stderr)) < 0) {
    if (err) *err = std::string("cannot redirect stderr: ") + strerror(errno);
    return false;
  }
  // Line buffered: a log nobody can tail while it is being written is only
  // half a log.
  ::setvbuf(stdout, nullptr, _IOLBF, 0);
  ::setvbuf(stderr, nullptr, _IONBF, 0);
  return true;
}

bool ConsoleLog::open(const std::string& path, const Rotation& rotation,
                      std::string* err) {
  if (path.empty()) return true;
  make_parents(path);
  path_ = path;
  rotation_ = rotation;
  if (!point_streams_at_file(err)) {
    path_.clear();
    return false;
  }
  return true;
}

void ConsoleLog::maybe_rotate() {
  if (path_.empty() || !rotation_.enabled()) return;
  std::fflush(stdout);
  const long long at = ::ftello(stdout);
  if (at < 0 || static_cast<double>(at) < rotation_.max_bytes) return;

  rotate_files(path_, rotation_.keep);
  std::string err;
  if (!point_streams_at_file(&err)) {
    // stdout is gone, so there is nowhere left to complain to. Give up on
    // rotating rather than spinning on it every cycle for the rest of the run.
    rotation_.max_bytes = 0.0;
    return;
  }
  rotations_ += 1;
  std::printf("--- rotated %s, keeping %d generations ---\n", path_.c_str(),
              rotation_.keep);
  std::fflush(stdout);
}

}  // namespace octo
