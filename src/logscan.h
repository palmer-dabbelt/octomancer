// Reading back the JSONL the sync daemon writes.
//
// This is a scanner for one specific shape of JSON -- a flat object per line,
// whose values are numbers, strings, booleans, or one level of nested object --
// and not a general JSON parser. That is the whole point: a general parser is
// either a dependency or a supply of subtle bugs, and the only documents this
// ever reads are ones octomancer wrote itself. It is strict about what it
// accepts and reports a failure rather than guessing, so a corrupted line from
// a machine that lost power mid-write is skipped instead of misread.
#ifndef OCTO_LOGSCAN_H
#define OCTO_LOGSCAN_H

#include <map>
#include <string>
#include <vector>

namespace octo {

class LogRecord {
 public:
  bool has(const std::string& key) const;

  // Values are returned by conversion, with a caller-supplied fallback, so
  // reading a field that an older log never wrote is not a special case at
  // every call site.
  std::string text(const std::string& key,
                   const std::string& fallback = std::string()) const;
  double number(const std::string& key, double fallback = 0.0) const;
  bool flag(const std::string& key, bool fallback = false) const;

  // The raw JSON text of a value, for a nested object.
  std::string raw(const std::string& key) const;

  const std::map<std::string, std::string>& fields() const { return fields_; }
  void set(const std::string& key, const std::string& raw_value);

 private:
  std::map<std::string, std::string> fields_;
};

// Parse one line. Returns false on anything that is not a flat JSON object.
bool parse_record(const std::string& line, LogRecord* out);

// Split a nested object -- the per-box map -- into its members.
bool parse_object(const std::string& raw, std::map<std::string, std::string>* out);

// Unquote a JSON string value. Returns false if it is not a string.
bool json_string(const std::string& raw, std::string* out);

// Seconds since the Unix epoch for a record's timestamp.
//
// Two spellings are accepted because the format changed when the daemon was
// ported to C++: the current one writes `wall` as a number, and the Python it
// replaced wrote an ISO-8601 string. Reading both means the nights of drift
// data already collected do not have to be thrown away.
bool record_time(const LogRecord& rec, double* unix_seconds);

}  // namespace octo

#endif  // OCTO_LOGSCAN_H
