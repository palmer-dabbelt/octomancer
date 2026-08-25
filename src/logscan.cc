#include "logscan.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

namespace octo {

namespace {

void skip_space(const std::string& s, size_t* i) {
  while (*i < s.size() && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\n' ||
                           s[*i] == '\r')) {
    ++*i;
  }
}

// Advance past one JSON value and return its raw text, or false if it is
// malformed. Nesting is tracked so an object value is captured whole.
bool scan_value(const std::string& s, size_t* i, std::string* raw) {
  skip_space(s, i);
  if (*i >= s.size()) return false;
  const size_t start = *i;

  if (s[*i] == '"') {
    ++*i;
    while (*i < s.size()) {
      if (s[*i] == '\\') {
        *i += 2;
        continue;
      }
      if (s[*i] == '"') {
        ++*i;
        *raw = s.substr(start, *i - start);
        return true;
      }
      ++*i;
    }
    return false;  // unterminated string
  }

  if (s[*i] == '{' || s[*i] == '[') {
    int depth = 0;
    bool in_string = false;
    while (*i < s.size()) {
      const char c = s[*i];
      if (in_string) {
        if (c == '\\') {
          *i += 2;
          continue;
        }
        if (c == '"') in_string = false;
      } else if (c == '"') {
        in_string = true;
      } else if (c == '{' || c == '[') {
        ++depth;
      } else if (c == '}' || c == ']') {
        if (--depth == 0) {
          ++*i;
          *raw = s.substr(start, *i - start);
          return true;
        }
      }
      ++*i;
    }
    return false;  // unbalanced
  }

  // A bare token: number, true, false, null.
  while (*i < s.size() && s[*i] != ',' && s[*i] != '}' && s[*i] != ']') ++*i;
  size_t end = *i;
  while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
  if (end == start) return false;
  *raw = s.substr(start, end - start);
  return true;
}

bool scan_members(const std::string& s, size_t* i,
                  std::map<std::string, std::string>* out) {
  skip_space(s, i);
  if (*i >= s.size() || s[*i] != '{') return false;
  ++*i;
  skip_space(s, i);
  if (*i < s.size() && s[*i] == '}') {
    ++*i;
    return true;
  }
  for (;;) {
    skip_space(s, i);
    std::string key_raw;
    if (*i >= s.size() || s[*i] != '"') return false;
    if (!scan_value(s, i, &key_raw)) return false;
    std::string key;
    if (!json_string(key_raw, &key)) return false;

    skip_space(s, i);
    if (*i >= s.size() || s[*i] != ':') return false;
    ++*i;

    std::string value;
    if (!scan_value(s, i, &value)) return false;
    (*out)[key] = value;

    skip_space(s, i);
    if (*i < s.size() && s[*i] == ',') {
      ++*i;
      continue;
    }
    if (*i < s.size() && s[*i] == '}') {
      ++*i;
      return true;
    }
    return false;
  }
}

}  // namespace

bool json_string(const std::string& raw, std::string* out) {
  if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return false;
  std::string s;
  for (size_t i = 1; i + 1 < raw.size(); ++i) {
    if (raw[i] != '\\') {
      s.push_back(raw[i]);
      continue;
    }
    if (i + 2 >= raw.size() + 1) return false;
    ++i;
    switch (raw[i]) {
      case '"': s.push_back('"'); break;
      case '\\': s.push_back('\\'); break;
      case '/': s.push_back('/'); break;
      case 'n': s.push_back('\n'); break;
      case 'r': s.push_back('\r'); break;
      case 't': s.push_back('\t'); break;
      case 'b': s.push_back('\b'); break;
      case 'f': s.push_back('\f'); break;
      case 'u': {
        if (i + 4 >= raw.size()) return false;
        const std::string hex = raw.substr(i + 1, 4);
        const long code = std::strtol(hex.c_str(), nullptr, 16);
        i += 4;
        // Only the escapes octomancer itself emits are decoded, which are all
        // control characters below 0x80. Anything else is left as a marker
        // rather than half-decoded into invalid UTF-8.
        if (code < 0x80) {
          s.push_back(static_cast<char>(code));
        } else {
          s.push_back('?');
        }
        break;
      }
      default: return false;
    }
  }
  if (out) *out = s;
  return true;
}

bool LogRecord::has(const std::string& key) const {
  return fields_.find(key) != fields_.end();
}

void LogRecord::set(const std::string& key, const std::string& raw_value) {
  fields_[key] = raw_value;
}

std::string LogRecord::raw(const std::string& key) const {
  auto it = fields_.find(key);
  return it == fields_.end() ? std::string() : it->second;
}

std::string LogRecord::text(const std::string& key,
                            const std::string& fallback) const {
  auto it = fields_.find(key);
  if (it == fields_.end()) return fallback;
  std::string out;
  if (json_string(it->second, &out)) return out;
  return it->second;
}

double LogRecord::number(const std::string& key, double fallback) const {
  auto it = fields_.find(key);
  if (it == fields_.end()) return fallback;
  const std::string& raw = it->second;
  if (raw.empty() || raw.front() == '"' || raw.front() == '{' ||
      raw.front() == '[') {
    return fallback;
  }
  char* end = nullptr;
  const double v = std::strtod(raw.c_str(), &end);
  if (end == raw.c_str()) return fallback;
  return v;
}

bool LogRecord::flag(const std::string& key, bool fallback) const {
  auto it = fields_.find(key);
  if (it == fields_.end()) return fallback;
  if (it->second == "true") return true;
  if (it->second == "false") return false;
  return fallback;
}

bool parse_record(const std::string& line, LogRecord* out) {
  std::map<std::string, std::string> members;
  size_t i = 0;
  if (!scan_members(line, &i, &members)) return false;
  if (out) {
    for (const auto& entry : members) out->set(entry.first, entry.second);
  }
  return true;
}

bool parse_object(const std::string& raw,
                  std::map<std::string, std::string>* out) {
  size_t i = 0;
  return scan_members(raw, &i, out);
}

bool record_time(const LogRecord& rec, double* unix_seconds) {
  const std::string raw = rec.raw("wall");
  if (raw.empty()) return false;

  if (raw.front() != '"') {
    const double v = rec.number("wall", 0.0);
    if (v <= 0.0) return false;
    if (unix_seconds) *unix_seconds = v;
    return true;
  }

  // "2026-08-24T21:42:05.123" from the Python daemon this replaced.
  std::string s;
  if (!json_string(raw, &s)) return false;
  struct tm tm_local;
  std::memset(&tm_local, 0, sizeof tm_local);
  double frac = 0.0;
  int year, mon, day, hour, min;
  double sec;
  if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &year, &mon, &day, &hour,
                  &min, &sec) != 6) {
    return false;
  }
  tm_local.tm_year = year - 1900;
  tm_local.tm_mon = mon - 1;
  tm_local.tm_mday = day;
  tm_local.tm_hour = hour;
  tm_local.tm_min = min;
  tm_local.tm_sec = static_cast<int>(sec);
  frac = sec - tm_local.tm_sec;
  tm_local.tm_isdst = -1;
  const time_t t = ::mktime(&tm_local);
  if (t == static_cast<time_t>(-1)) return false;
  if (unix_seconds) *unix_seconds = static_cast<double>(t) + frac;
  return true;
}

}  // namespace octo
