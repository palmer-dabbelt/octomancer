// Append-only JSONL, so a night of running leaves something to fit a line to.
//
// Append, never truncate: restarting the agent must not destroy the history
// that makes drift measurable in the first place.
#ifndef OCTO_JSONLOG_H
#define OCTO_JSONLOG_H

#include <cstdio>
#include <string>

namespace octo {

class JsonLog {
 public:
  JsonLog() = default;
  ~JsonLog();

  JsonLog(const JsonLog&) = delete;
  JsonLog& operator=(const JsonLog&) = delete;

  // An empty path disables logging. Creates parent directories.
  bool open(const std::string& path, std::string* err);
  bool enabled() const { return file_ != nullptr; }

  // `fields` is the interior of a JSON object, without the braces: this
  // prepends the event name and both clocks and writes one line.
  void record(const char* event, const std::string& fields);

  void close();

 private:
  std::FILE* file_ = nullptr;
  std::string path_;
};

}  // namespace octo

#endif  // OCTO_JSONLOG_H
