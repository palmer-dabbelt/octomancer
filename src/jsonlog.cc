#include "jsonlog.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

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

}  // namespace

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
  return true;
}

void JsonLog::record(const char* event, const std::string& fields) {
  if (file_ == nullptr) return;
  const double wall = wall_now();
  std::fprintf(file_, "{\"event\":\"%s\",\"wall\":%.3f,\"local\":\"%s\"", event,
               wall, format_local(wall).c_str());
  if (!fields.empty()) {
    std::fputc(',', file_);
    std::fwrite(fields.data(), 1, fields.size(), file_);
  }
  std::fputs("}\n", file_);
  // Flush every line. An overnight run that ends in a crash or a closed lid
  // must still have its data on disk, and one fflush a minute costs nothing.
  std::fflush(file_);
}

void JsonLog::close() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
  path_.clear();
}

}  // namespace octo
