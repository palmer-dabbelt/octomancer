#include "camdb.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <sstream>

#include "jsonlog.h"
#include "logscan.h"
#include "timeutil.h"

namespace octo {

namespace {

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));

std::string fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

std::string read_file(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return std::string();
  std::string out;
  char buf[8192];
  size_t got;
  while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, got);
  std::fclose(f);
  return out;
}

}  // namespace

std::string default_camera_db_path() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') return std::string();
  return std::string(home) + "/.octomancer/per_camera.json";
}

std::vector<double> CameraRecord::recent_apply_delays(size_t n) const {
  std::vector<double> out;
  if (n == 0) return out;
  // Walk backwards to collect the newest n that count, then put them back in
  // chronological order: a caller taking a median does not care, but one
  // plotting them does, and returning them shuffled invites a subtle bug.
  for (auto it = samples.rbegin(); it != samples.rend() && out.size() < n; ++it) {
    if (!it->timing_ok) continue;
    // A sample measured on an older basis is history, not evidence.
    if (it->measure_epoch != kMeasureEpoch) continue;
    out.push_back(it->apply_delay_s());
  }
  std::reverse(out.begin(), out.end());
  return out;
}

// ------------------------------------------------------------- serialising

std::string camera_line(const CameraRecord& rec) {
  std::string s = "{\"t\":\"camera\"";
  s += ",\"id\":\"" + json_escape(rec.id) + "\"";
  s += ",\"name\":\"" + json_escape(rec.name) + "\"";
  s += fmt(",\"first_wall\":%.3f", rec.first_seen_wall);
  s += fmt(",\"last_wall\":%.3f", rec.last_seen_wall);
  s += fmt(",\"sessions\":%llu", static_cast<unsigned long long>(rec.sessions));
  s += fmt(",\"writes\":%llu", static_cast<unsigned long long>(rec.writes));
  s += fmt(",\"fps\":%d", rec.fps);
  // Absent rather than zero when unknown: a bias of 0 is a real, learned value
  // and must not read the same as never having measured one.
  if (rec.has_bias) s += fmt(",\"bias\":%d", rec.bias);
  if (rec.has_lead) s += fmt(",\"lead_s\":%.6f", rec.lead_s);
  if (rec.has_drift) {
    s += fmt(",\"drift_ppm\":%.3f", rec.drift_ppm);
    s += fmt(",\"drift_span_s\":%.1f", rec.drift_span_s);
  }
  s += "}";
  return s;
}

std::string write_line(const std::string& id, const WriteSample& s) {
  std::string out = "{\"t\":\"write\"";
  out += ",\"id\":\"" + json_escape(id) + "\"";
  out += fmt(",\"wall\":%.3f", s.wall);
  out += fmt(",\"error_before_s\":%.6f", s.error_before_s);
  out += fmt(",\"error_after_s\":%.6f", s.error_after_s);
  out += fmt(",\"lead_used_s\":%.6f", s.lead_used_s);
  out += fmt(",\"latency_s\":%.6f", s.latency_s);
  out += fmt(",\"fps\":%d", s.fps);
  out += fmt(",\"bias\":%d", s.bias);
  out += std::string(",\"verified\":") + (s.verified ? "true" : "false");
  out += std::string(",\"timing_ok\":") + (s.timing_ok ? "true" : "false");
  out += fmt(",\"measure_epoch\":%d", s.measure_epoch);
  out += "}";
  return out;
}

// --------------------------------------------------------------- replaying

void replay_camera_db(const std::string& text, size_t max_samples,
                      std::map<std::string, CameraRecord>* out) {
  if (out == nullptr) return;
  out->clear();
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    LogRecord rec;
    // A line that does not parse is skipped rather than fatal. The realistic
    // way this file gets damaged is a lid closing mid-append, which truncates
    // exactly one line at the end.
    if (!parse_record(line, &rec)) continue;
    const std::string type = rec.text("t");
    const std::string id = rec.text("id");
    if (id.empty()) continue;

    if (type == "camera") {
      CameraRecord& c = (*out)[id];
      c.id = id;
      c.name = rec.text("name", c.name);
      c.first_seen_wall = rec.number("first_wall", c.first_seen_wall);
      c.last_seen_wall = rec.number("last_wall", c.last_seen_wall);
      c.sessions = static_cast<uint64_t>(rec.number("sessions", 0.0));
      c.writes = static_cast<uint64_t>(rec.number("writes", 0.0));
      c.fps = static_cast<int>(rec.number("fps", c.fps));
      if (rec.has("bias")) {
        c.has_bias = true;
        c.bias = static_cast<int>(rec.number("bias", 0.0));
      }
      if (rec.has("lead_s")) {
        c.has_lead = true;
        c.lead_s = rec.number("lead_s", 0.0);
      }
      if (rec.has("drift_ppm")) {
        c.has_drift = true;
        c.drift_ppm = rec.number("drift_ppm", 0.0);
        c.drift_span_s = rec.number("drift_span_s", 0.0);
      }
    } else if (type == "write") {
      CameraRecord& c = (*out)[id];
      if (c.id.empty()) c.id = id;
      WriteSample s;
      s.wall = rec.number("wall", 0.0);
      s.error_before_s = rec.number("error_before_s", 0.0);
      s.error_after_s = rec.number("error_after_s", 0.0);
      s.lead_used_s = rec.number("lead_used_s", 0.0);
      s.latency_s = rec.number("latency_s", 0.0);
      s.fps = static_cast<int>(rec.number("fps", 0.0));
      s.bias = static_cast<int>(rec.number("bias", 0.0));
      s.verified = rec.flag("verified", false);
      // Databases written before the lead was learned have no timing_ok, and
      // their writes all predate the correction being applied -- so they are
      // exactly the samples worth learning from. Fall back to verified.
      s.timing_ok = rec.flag("timing_ok", s.verified);
      // Absent means it was written before the basis was versioned, which is
      // exactly the case the epoch exists to exclude -- so the default is 0,
      // never the current epoch.
      s.measure_epoch = static_cast<int>(rec.number("measure_epoch", 0.0));
      c.samples.push_back(s);
      while (max_samples > 0 && c.samples.size() > max_samples) {
        c.samples.pop_front();
      }
    }
    // A "compact" marker carries nothing that replay needs; it exists so a
    // human reading the file can see where the rewrites happened.
  }
}

// ------------------------------------------------------------------ CamDb

CamDb::~CamDb() { close(); }

void CamDb::close() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

bool CamDb::open(const std::string& path, const CamDbOptions& opt,
                 std::string* err) {
  close();
  cam_.clear();
  bytes_ = 0;
  path_ = path;
  opt_ = opt;
  if (path.empty()) return true;  // disabled, not an error

  make_parents(path);
  replay_camera_db(read_file(path), opt_.max_samples, &cam_);

  file_ = std::fopen(path.c_str(), "ae");
  if (file_ == nullptr) {
    if (err) *err = "could not open " + path + ": " + std::strerror(errno);
    path_.clear();
    return false;
  }
  std::fseek(file_, 0, SEEK_END);
  const long at = std::ftell(file_);
  bytes_ = at > 0 ? at : 0;
  return true;
}

const CameraRecord* CamDb::find(const std::string& id) const {
  auto it = cam_.find(id);
  return it == cam_.end() ? nullptr : &it->second;
}

bool CamDb::append(const std::string& line, std::string* err) {
  if (file_ == nullptr) return true;
  if (std::fputs(line.c_str(), file_) < 0 || std::fputc('\n', file_) < 0) {
    if (err) *err = "could not write " + path_ + ": " + std::strerror(errno);
    return false;
  }
  bytes_ += static_cast<long long>(line.size()) + 1;
  // Flush every line, for the same reason JsonLog does: a write learned about
  // at 3am must be on disk before the lid closes at 3:01.
  std::fflush(file_);
  if (should_compact()) return compact(err);
  return true;
}

double CamDb::live_bytes_estimate() const {
  double total = 64.0;  // the compact marker
  for (const auto& kv : cam_) {
    total += static_cast<double>(camera_line(kv.second).size()) + 1.0;
    for (const WriteSample& s : kv.second.samples) {
      total += static_cast<double>(write_line(kv.first, s).size()) + 1.0;
    }
  }
  return total;
}

bool CamDb::should_compact() const {
  if (file_ == nullptr) return false;
  if (opt_.compact_factor <= 1.0) return false;
  // Cheap guard first: below the floor there is nothing worth reclaiming, and
  // this saves walking every sample on every append.
  if (static_cast<double>(bytes_) < opt_.compact_min_bytes) return false;
  return static_cast<double>(bytes_) >=
         live_bytes_estimate() * opt_.compact_factor;
}

bool CamDb::forget(const std::string& id, std::string* err) {
  if (cam_.erase(id) == 0) return true;
  if (file_ == nullptr) return true;  // nothing on disk to rewrite
  // compact() writes the file out of cam_, so the erase above is the deletion
  // and this is only how it is made durable.
  return compact(err);
}

bool CamDb::compact(std::string* err) {
  if (path_.empty()) return true;

  // Write the replacement beside the original and rename it into place, so a
  // crash halfway through leaves the old file intact rather than a half-built
  // one. We are the only writer, and we reopen afterwards, so the rename
  // cannot strand anybody on an unnamed inode.
  const std::string tmp = path_ + ".tmp";
  std::FILE* out = std::fopen(tmp.c_str(), "we");
  if (out == nullptr) {
    if (err) *err = "could not open " + tmp + ": " + std::strerror(errno);
    return false;
  }

  size_t kept = 0;
  std::string body =
      fmt("{\"t\":\"compact\",\"wall\":%.3f,\"cameras\":%zu", wall_now(),
          cam_.size());
  for (const auto& kv : cam_) kept += kv.second.samples.size();
  body += fmt(",\"samples\":%zu}\n", kept);
  for (const auto& kv : cam_) {
    body += camera_line(kv.second) + "\n";
    for (const WriteSample& s : kv.second.samples) {
      body += write_line(kv.first, s) + "\n";
    }
  }

  const bool wrote =
      std::fwrite(body.data(), 1, body.size(), out) == body.size();
  const bool flushed = std::fflush(out) == 0;
  // fsync before the rename, or the rename can be durable while the contents
  // are not, which is the one failure that loses everything at once.
  const bool synced = ::fsync(::fileno(out)) == 0;
  std::fclose(out);
  if (!wrote || !flushed || !synced) {
    if (err) *err = "could not write " + tmp + ": " + std::strerror(errno);
    ::unlink(tmp.c_str());
    return false;
  }

  close();
  if (::rename(tmp.c_str(), path_.c_str()) != 0) {
    if (err) *err = "could not replace " + path_ + ": " + std::strerror(errno);
    ::unlink(tmp.c_str());
    // Reopen the original so the caller is not left with a dead database.
    file_ = std::fopen(path_.c_str(), "ae");
    return false;
  }

  file_ = std::fopen(path_.c_str(), "ae");
  if (file_ == nullptr) {
    if (err) *err = "could not reopen " + path_ + ": " + std::strerror(errno);
    return false;
  }
  bytes_ = static_cast<long long>(body.size());
  compactions_ += 1;
  return true;
}

bool CamDb::note_seen(const std::string& id, const std::string& name, int fps,
                      bool new_session, std::string* err) {
  if (id.empty()) return true;
  CameraRecord& c = cam_[id];
  const double now = wall_now();
  if (c.id.empty()) c.id = id;
  if (c.first_seen_wall <= 0.0) c.first_seen_wall = now;
  c.last_seen_wall = now;
  if (!name.empty()) c.name = name;
  if (fps > 0) c.fps = fps;
  if (new_session) c.sessions += 1;
  if (file_ == nullptr) return true;
  return append(camera_line(c), err);
}

bool CamDb::record_write(const std::string& id, const WriteSample& s,
                         std::string* err) {
  if (id.empty()) return true;
  CameraRecord& c = cam_[id];
  if (c.id.empty()) c.id = id;
  // These two bracket everything known about the body, so they only ever widen.
  // A sample carrying an older timestamp than the record -- backfilled from a
  // log, say -- must not make the body look as though it was last seen before
  // it was last seen.
  if (c.first_seen_wall <= 0.0 || s.wall < c.first_seen_wall) {
    c.first_seen_wall = s.wall;
  }
  if (s.wall > c.last_seen_wall) c.last_seen_wall = s.wall;
  if (s.fps > 0) c.fps = s.fps;
  c.writes += 1;
  c.samples.push_back(s);
  while (opt_.max_samples > 0 && c.samples.size() > opt_.max_samples) {
    c.samples.pop_front();
  }
  if (file_ == nullptr) return true;
  return append(write_line(id, s), err);
}

bool CamDb::learn(const std::string& id, bool has_bias, int bias,
                  bool has_lead, double lead_s, bool has_drift,
                  double drift_ppm, double drift_span_s) {
  if (id.empty()) return false;
  CameraRecord& c = cam_[id];
  if (c.id.empty()) c.id = id;
  bool changed = false;
  if (has_bias && (!c.has_bias || c.bias != bias)) {
    c.has_bias = true;
    c.bias = bias;
    changed = true;
  }
  // A hair of movement in a learned median is not news. Without this the
  // daemon would append a line after every single write.
  if (has_lead && (!c.has_lead || std::abs(c.lead_s - lead_s) > 1e-4)) {
    c.has_lead = true;
    c.lead_s = lead_s;
    changed = true;
  }
  if (has_drift && (!c.has_drift || std::abs(c.drift_ppm - drift_ppm) > 0.05 ||
                    drift_span_s > c.drift_span_s * 1.5)) {
    c.has_drift = true;
    c.drift_ppm = drift_ppm;
    c.drift_span_s = drift_span_s;
    changed = true;
  }
  return changed;
}

bool CamDb::record_params(const std::string& id, std::string* err) {
  if (id.empty() || file_ == nullptr) return true;
  auto it = cam_.find(id);
  if (it == cam_.end()) return true;
  return append(camera_line(it->second), err);
}

}  // namespace octo
