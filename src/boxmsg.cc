#include "boxmsg.h"

#include <cstdio>
#include <cstdlib>

#include "escape.h"

namespace octo {
namespace {

// A bare token: what a verb and a key are allowed to be. Deliberately narrow.
// The tokenizer splits on space and then on '=', so a key containing either
// would produce a line that does not read back as what was written, and a
// format whose round trip depends on the values is not one worth having.
bool is_bare_token(const std::string& s) {
  if (s.empty()) return false;
  for (unsigned char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

bool Message::has(const std::string& key) const {
  for (const auto& f : fields) {
    if (f.first == key) return true;
  }
  return false;
}

std::string Message::get(const std::string& key,
                         const std::string& fallback) const {
  for (const auto& f : fields) {
    if (f.first == key) return f.second;
  }
  return fallback;
}

std::vector<std::string> Message::all(const std::string& key) const {
  std::vector<std::string> out;
  for (const auto& f : fields) {
    if (f.first == key) out.push_back(f.second);
  }
  return out;
}

bool Message::get_int(const std::string& key, int64_t* out) const {
  if (!has(key)) return false;
  const std::string v = get(key);
  if (v.empty()) return false;
  char* end = nullptr;
  const long long parsed = std::strtoll(v.c_str(), &end, 10);
  // The whole value has to parse. "12abc" is a mistake somewhere, and reading
  // it as 12 hides the mistake in a number that looks reasonable.
  if (end == nullptr || *end != '\0') return false;
  if (out) *out = static_cast<int64_t>(parsed);
  return true;
}

bool Message::get_double(const std::string& key, double* out) const {
  if (!has(key)) return false;
  const std::string v = get(key);
  if (v.empty()) return false;
  char* end = nullptr;
  const double parsed = std::strtod(v.c_str(), &end);
  if (end == nullptr || *end != '\0') return false;
  if (out) *out = parsed;
  return true;
}

bool Message::get_bool(const std::string& key, bool* out) const {
  if (!has(key)) return false;
  const std::string v = get(key);
  // Accept what a person would type as well as what the encoder emits. A
  // console that rejects "yes" is a console people swear at.
  if (v == "1" || v == "true" || v == "yes" || v == "on") {
    if (out) *out = true;
    return true;
  }
  if (v == "0" || v == "false" || v == "no" || v == "off") {
    if (out) *out = false;
    return true;
  }
  return false;
}

void Message::set(const std::string& key, const std::string& value) {
  for (auto& f : fields) {
    if (f.first == key) {
      f.second = value;
      return;
    }
  }
  fields.emplace_back(key, value);
}

void Message::add(const std::string& key, const std::string& value) {
  fields.emplace_back(key, value);
}

void Message::set_int(const std::string& key, int64_t value) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(value));
  set(key, buf);
}

void Message::set_double(const std::string& key, double value, int digits) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", digits, value);
  set(key, buf);
}

void Message::set_bool(const std::string& key, bool value) {
  set(key, value ? "1" : "0");
}

bool can_format_doubles() {
  // A value with a fraction that no integer path could produce by accident,
  // and a magnitude in the range the protocol actually carries.
  Message msg;
  msg.verb = "selftest";
  msg.set_double("x", -6.25, 3);
  if (msg.get("x") != "-6.250") return false;

  Message back;
  std::string err;
  if (!decode(encode(msg), &back, &err)) return false;
  double out = 0.0;
  if (!back.get_double("x", &out)) return false;
  return out > -6.2505 && out < -6.2495;
}

std::string encode(const Message& msg) {
  if (!is_bare_token(msg.verb)) return std::string();
  std::string out = msg.verb;
  for (const auto& f : msg.fields) {
    if (!is_bare_token(f.first)) continue;
    out.push_back(' ');
    out.append(f.first);
    out.push_back('=');
    out.append(escape(f.second));
  }
  return out;
}

bool decode(const std::string& line, Message* out, std::string* err) {
  auto bail = [&](const char* why) {
    if (err) *err = why;
    return false;
  };

  // A person on a terminal emulator sends CRLF and should not have to know it.
  size_t end = line.size();
  while (end > 0 && (line[end - 1] == '\r' || line[end - 1] == '\n')) --end;

  size_t i = 0;
  while (i < end && line[i] == ' ') ++i;
  if (i >= end) return bail("empty message");

  Message msg;

  size_t start = i;
  while (i < end && line[i] != ' ') ++i;
  msg.verb = line.substr(start, i - start);
  if (!is_bare_token(msg.verb)) return bail("the verb is not a bare token");

  while (i < end) {
    while (i < end && line[i] == ' ') ++i;
    if (i >= end) break;
    start = i;
    while (i < end && line[i] != ' ') ++i;
    const std::string token = line.substr(start, i - start);

    const size_t eq = token.find('=');
    // A bare word where a field belongs is a typo, not a flag. Guessing which
    // one it was is how a mistyped command silently does something else.
    if (eq == std::string::npos) return bail("a field has no '='");
    const std::string key = token.substr(0, eq);
    if (!is_bare_token(key)) return bail("a field key is not a bare token");
    msg.fields.emplace_back(key, unescape(token.substr(eq + 1)));
  }

  if (out) *out = std::move(msg);
  return true;
}

bool LineReader::feed(const char* data, size_t len,
                      std::vector<std::string>* out) {
  bool dropped = false;
  for (size_t i = 0; i < len; ++i) {
    const char c = data[i];
    if (c == '\n') {
      if (dropping_) {
        // The tail of an over-long line. Report once, at the newline, so a
        // caller learns that a command was lost rather than that many were.
        dropping_ = false;
        dropped = true;
      } else if (out) {
        out->push_back(buf_);
      }
      buf_.clear();
      continue;
    }
    if (dropping_) continue;
    if (buf_.size() >= kMaxLine) {
      // Drop the line, not the connection. A peer that overruns once is
      // usually a peer with a bug, and dropping the link makes that bug look
      // like a flaky cable.
      buf_.clear();
      dropping_ = true;
      continue;
    }
    buf_.push_back(c);
  }
  return !dropped;
}

bool LineReader::feed(const std::string& data, std::vector<std::string>* out) {
  return feed(data.data(), data.size(), out);
}

void LineReader::reset() {
  buf_.clear();
  dropping_ = false;
}

}  // namespace octo
