#include "control.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "proto.h"
#include "timeutil.h"

namespace octo {

namespace {

void put(std::string* out, const char* key, const std::string& value) {
  out->push_back(' ');
  out->append(key);
  out->push_back('=');
  out->append(escape(value));
}

void put(std::string* out, const char* key, double value, int digits = 4) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", digits, value);
  out->push_back(' ');
  out->append(key);
  out->push_back('=');
  out->append(buf);
}

void put(std::string* out, const char* key, long long value) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%lld", value);
  out->push_back(' ');
  out->append(key);
  out->push_back('=');
  out->append(buf);
}

void put_bool(std::string* out, const char* key, bool value) {
  out->push_back(' ');
  out->append(key);
  out->append(value ? "=1" : "=0");
}

std::string banner() {
  return "octomancer " + std::to_string(kProtocolVersion) + "\n";
}

// One line's worth of `key=value` tokens. Repeated keys keep the last, except
// where a caller walks the tokens itself -- camera= does, because repeating it
// is how several cameras are named in one request.
struct Tokens {
  std::vector<std::pair<std::string, std::string>> pairs;

  const std::string* find(const char* key) const {
    for (const auto& kv : pairs) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }
  bool num(const char* key, int64_t* out) const {
    const std::string* v = find(key);
    if (v == nullptr) return false;
    char* end = nullptr;
    const long long parsed = std::strtoll(v->c_str(), &end, 10);
    if (end == v->c_str() || (end != nullptr && *end != '\0')) return false;
    *out = parsed;
    return true;
  }
  bool real(const char* key, double* out) const {
    const std::string* v = find(key);
    if (v == nullptr) return false;
    *out = std::atof(v->c_str());
    return true;
  }
  bool flag(const char* key, bool* out) const {
    int64_t v = 0;
    if (!num(key, &v)) return false;
    *out = v != 0;
    return true;
  }
  bool str(const char* key, std::string* out) const {
    const std::string* v = find(key);
    if (v == nullptr) return false;
    *out = unescape(*v);
    return true;
  }
};

Tokens tokenize(const std::string& rest) {
  Tokens t;
  std::istringstream in(rest);
  std::string tok;
  while (in >> tok) {
    const size_t eq = tok.find('=');
    if (eq == std::string::npos) {
      t.pairs.emplace_back(tok, std::string());
    } else {
      t.pairs.emplace_back(tok.substr(0, eq), tok.substr(eq + 1));
    }
  }
  return t;
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                   s[e - 1] == '\n')) {
    --e;
  }
  return s.substr(b, e - b);
}

// Split a reply into its lines, dropping the banner and the trailer, and
// hand each remaining line back as verb + tokens.
bool split_reply(const std::string& text, std::string* err,
                 std::vector<std::pair<std::string, Tokens>>* out) {
  std::istringstream in(text);
  std::string line;
  bool seen_banner = false;
  bool seen_end = false;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty()) continue;
    if (!seen_banner) {
      if (line.compare(0, 11, "octomancer ") != 0) {
        if (err) *err = "not an octomancer reply: " + line;
        return false;
      }
      seen_banner = true;
      continue;
    }
    if (line == "end") {
      seen_end = true;
      break;
    }
    const size_t sp = line.find(' ');
    const std::string verb = sp == std::string::npos ? line : line.substr(0, sp);
    if (verb == "error") {
      if (err) *err = unescape(sp == std::string::npos ? "" : line.substr(sp + 1));
      return false;
    }
    out->emplace_back(verb, tokenize(sp == std::string::npos
                                         ? std::string()
                                         : line.substr(sp + 1)));
  }
  if (!seen_banner) {
    if (err) *err = "empty reply";
    return false;
  }
  if (!seen_end) {
    if (err) *err = "truncated reply";
    return false;
  }
  return true;
}

std::string json_string(const std::string& in) {
  std::string out = "\"";
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20 || c == 0x7f) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
  return out;
}

std::string num_json(double v, int digits = 4) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", digits, v);
  return buf;
}

}  // namespace

// ------------------------------------------------------------------- names

const char* request_kind_name(RequestKind k) {
  switch (k) {
    case RequestKind::kSync: return "sync";
    case RequestKind::kSetSource: return "source";
  }
  return "?";
}

const char* request_state_name(RequestState s) {
  switch (s) {
    case RequestState::kQueued: return "queued";
    case RequestState::kRunning: return "running";
    case RequestState::kDone: return "done";
    case RequestState::kFailed: return "failed";
    case RequestState::kUnknown: return "unknown";
  }
  return "?";
}

bool request_finished(RequestState s) {
  return s == RequestState::kDone || s == RequestState::kFailed ||
         s == RequestState::kUnknown;
}

const char* event_kind_name(EventKind k) {
  switch (k) {
    case EventKind::kSyncFailed: return "sync-failed";
    case EventKind::kFirstSync: return "first-sync";
    case EventKind::kCameraLost: return "camera-lost";
  }
  return "?";
}

bool event_kind_from_name(const std::string& name, EventKind* out) {
  if (name == "sync-failed") { *out = EventKind::kSyncFailed; return true; }
  if (name == "first-sync") { *out = EventKind::kFirstSync; return true; }
  if (name == "camera-lost") { *out = EventKind::kCameraLost; return true; }
  return false;
}

std::string default_control_socket_path() {
  const char* home = std::getenv("HOME");
  const std::string base = home && *home ? home : "/tmp";
  return base + "/Library/Application Support/octomancer/octomancer-sync.sock";
}

// ----------------------------------------------------------------- parsing

Command parse_command(const std::string& line) {
  Command cmd;
  const std::string s = trim(line);
  const size_t sp = s.find(' ');
  cmd.verb = sp == std::string::npos ? s : s.substr(0, sp);
  const Tokens t =
      tokenize(sp == std::string::npos ? std::string() : s.substr(sp + 1));

  for (const auto& kv : t.pairs) {
    if (kv.first == "camera") {
      const std::string name = unescape(kv.second);
      if (!name.empty()) cmd.cameras.push_back(name);
    }
  }
  int64_t v = 0;
  if (t.find("value") != nullptr) {
    if (!t.num("value", &v)) {
      cmd.ok = false;
      cmd.error = "value must be a whole number";
      return cmd;
    }
    cmd.has_value = true;
    cmd.value = v;
  }
  if (t.find("id") != nullptr) {
    if (!t.num("id", &v)) {
      cmd.ok = false;
      cmd.error = "id must be a whole number";
      return cmd;
    }
    cmd.has_id = true;
    cmd.id = v;
  }
  if (t.find("since") != nullptr) {
    if (!t.num("since", &v)) {
      cmd.ok = false;
      cmd.error = "since must be a whole number";
      return cmd;
    }
    cmd.has_since = true;
    cmd.since = v;
  }
  return cmd;
}

// --------------------------------------------------------------- rendering

std::string render_error(const std::string& message) {
  return banner() + "error " + escape(message) + "\n";
}

std::string render_status(const Status& s) {
  std::string out = banner();

  out += "daemon";
  put(&out, "version", s.daemon.version);
  put(&out, "started", s.daemon.started_wall, 3);
  put(&out, "now", s.daemon.now_wall, 3);
  put(&out, "poll", s.daemon.poll_s, 1);
  put_bool(&out, "dry_run", s.daemon.dry_run);
  put(&out, "socket", s.daemon.socket_path);
  put(&out, "queued", static_cast<long long>(s.queued));
  out += "\n";

  if (s.bench.has) {
    out += "bench";
    put(&out, "source", s.bench.source);
    put(&out, "boxes", static_cast<long long>(s.bench.boxes));
    put(&out, "offset", s.bench.offset_s, 4);
    put(&out, "spread", s.bench.spread_s, 4);
    put_bool(&out, "daemon", s.bench.daemon_reachable);
    out += "\n";
  }

  for (const CameraStatus& c : s.cameras) {
    out += "camera";
    put(&out, "id", c.id);
    put(&out, "name", c.name);
    put_bool(&out, "present", c.present);
    put_bool(&out, "connected", c.connected);
    if (c.has_error) put(&out, "error", c.error_s, 4);
    if (!c.timecode.empty()) put(&out, "tc", c.timecode);
    if (c.has_fps) put(&out, "fps", static_cast<long long>(c.fps));
    put_bool(&out, "recording", c.recording);
    if (c.has_source) put(&out, "source", static_cast<long long>(c.source));
    if (!c.action.empty()) put(&out, "action", c.action);
    if (c.has_last_write) put(&out, "last_write", c.last_write_wall, 3);
    put(&out, "writes", static_cast<long long>(c.writes));
    if (c.has_lead) put(&out, "lead", c.lead_s, 4);
    if (c.has_drift) put(&out, "drift_ppm", c.drift_ppm, 2);
    out += "\n";
  }

  out += "end\n";
  return out;
}

std::string render_status_json(const Status& s) {
  std::string out = "{";
  out += "\"version\":" + json_string(s.daemon.version);
  out += ",\"started\":" + num_json(s.daemon.started_wall, 3);
  out += ",\"now\":" + num_json(s.daemon.now_wall, 3);
  out += ",\"poll_s\":" + num_json(s.daemon.poll_s, 1);
  out += ",\"dry_run\":";
  out += s.daemon.dry_run ? "true" : "false";
  out += ",\"queued\":" + std::to_string(s.queued);

  if (s.bench.has) {
    out += ",\"bench\":{\"source\":" + json_string(s.bench.source);
    out += ",\"boxes\":" + std::to_string(s.bench.boxes);
    out += ",\"offset_s\":" + num_json(s.bench.offset_s, 4);
    out += ",\"spread_s\":" + num_json(s.bench.spread_s, 4);
    out += ",\"daemon_reachable\":";
    out += s.bench.daemon_reachable ? "true" : "false";
    out += "}";
  }

  out += ",\"cameras\":[";
  bool first = true;
  for (const CameraStatus& c : s.cameras) {
    if (!first) out += ",";
    first = false;
    out += "{\"id\":" + json_string(c.id);
    out += ",\"name\":" + json_string(c.name);
    out += ",\"present\":";
    out += c.present ? "true" : "false";
    out += ",\"connected\":";
    out += c.connected ? "true" : "false";
    if (c.has_error) out += ",\"error_s\":" + num_json(c.error_s, 4);
    if (!c.timecode.empty()) out += ",\"timecode\":" + json_string(c.timecode);
    if (c.has_fps) out += ",\"fps\":" + std::to_string(c.fps);
    out += ",\"recording\":";
    out += c.recording ? "true" : "false";
    if (c.has_source) out += ",\"timecode_source\":" + std::to_string(c.source);
    if (!c.action.empty()) out += ",\"action\":" + json_string(c.action);
    if (c.has_last_write) {
      out += ",\"last_write\":" + num_json(c.last_write_wall, 3);
    }
    out += ",\"writes\":" + std::to_string(c.writes);
    if (c.has_lead) out += ",\"lead_s\":" + num_json(c.lead_s, 4);
    if (c.has_drift) out += ",\"drift_ppm\":" + num_json(c.drift_ppm, 2);
    out += "}";
  }
  out += "]}";
  return out;
}

std::string render_result(const RequestResult& r) {
  std::string out = banner();
  out += "result";
  put(&out, "id", static_cast<long long>(r.id));
  put(&out, "state", std::string(request_state_name(r.state)));
  put_bool(&out, "finished", request_finished(r.state));
  if (!r.message.empty()) put(&out, "message", r.message);
  if (r.updated_wall > 0.0) put(&out, "updated", r.updated_wall, 3);
  out += "\nend\n";
  return out;
}

std::string render_events(const std::vector<Event>& events, int64_t next_seq) {
  std::string out = banner();
  out += "events";
  put(&out, "next", static_cast<long long>(next_seq));
  out += "\n";
  for (const Event& e : events) {
    out += "event";
    put(&out, "seq", static_cast<long long>(e.seq));
    put(&out, "kind", std::string(event_kind_name(e.kind)));
    put(&out, "id", e.camera_id);
    put(&out, "name", e.camera_name);
    put(&out, "wall", e.wall, 3);
    if (!e.message.empty()) put(&out, "message", e.message);
    out += "\n";
  }
  out += "end\n";
  return out;
}

bool parse_status(const std::string& text, Status* out, std::string* err) {
  std::vector<std::pair<std::string, Tokens>> lines;
  if (!split_reply(text, err, &lines)) return false;

  *out = Status();
  for (const auto& line : lines) {
    const Tokens& t = line.second;
    if (line.first == "daemon") {
      t.str("version", &out->daemon.version);
      t.real("started", &out->daemon.started_wall);
      t.real("now", &out->daemon.now_wall);
      t.real("poll", &out->daemon.poll_s);
      t.flag("dry_run", &out->daemon.dry_run);
      t.str("socket", &out->daemon.socket_path);
      int64_t q = 0;
      if (t.num("queued", &q)) out->queued = static_cast<int>(q);
    } else if (line.first == "bench") {
      out->bench.has = true;
      t.str("source", &out->bench.source);
      int64_t boxes = 0;
      if (t.num("boxes", &boxes)) out->bench.boxes = static_cast<int>(boxes);
      t.real("offset", &out->bench.offset_s);
      t.real("spread", &out->bench.spread_s);
      t.flag("daemon", &out->bench.daemon_reachable);
    } else if (line.first == "camera") {
      CameraStatus c;
      t.str("id", &c.id);
      t.str("name", &c.name);
      t.flag("present", &c.present);
      t.flag("connected", &c.connected);
      c.has_error = t.real("error", &c.error_s);
      t.str("tc", &c.timecode);
      int64_t fps = 0;
      if (t.num("fps", &fps)) {
        c.has_fps = true;
        c.fps = static_cast<int>(fps);
      }
      t.flag("recording", &c.recording);
      c.has_source = t.num("source", &c.source);
      t.str("action", &c.action);
      c.has_last_write = t.real("last_write", &c.last_write_wall);
      int64_t writes = 0;
      if (t.num("writes", &writes)) c.writes = static_cast<int>(writes);
      c.has_lead = t.real("lead", &c.lead_s);
      c.has_drift = t.real("drift_ppm", &c.drift_ppm);
      out->cameras.push_back(c);
    }
  }
  return true;
}

bool parse_result(const std::string& text, RequestResult* out,
                  std::string* err) {
  std::vector<std::pair<std::string, Tokens>> lines;
  if (!split_reply(text, err, &lines)) return false;
  for (const auto& line : lines) {
    if (line.first != "result") continue;
    const Tokens& t = line.second;
    *out = RequestResult();
    t.num("id", &out->id);
    std::string state;
    t.str("state", &state);
    // An unknown state name from a newer daemon is reported as unknown rather
    // than guessed at, which leaves a client polling rather than declaring a
    // result it cannot read.
    out->state = RequestState::kUnknown;
    if (state == "queued") out->state = RequestState::kQueued;
    else if (state == "running") out->state = RequestState::kRunning;
    else if (state == "done") out->state = RequestState::kDone;
    else if (state == "failed") out->state = RequestState::kFailed;
    t.str("message", &out->message);
    t.real("updated", &out->updated_wall);
    return true;
  }
  if (err) *err = "reply carried no result";
  return false;
}

bool parse_events(const std::string& text, std::vector<Event>* out,
                  int64_t* next_seq, std::string* err) {
  std::vector<std::pair<std::string, Tokens>> lines;
  if (!split_reply(text, err, &lines)) return false;
  out->clear();
  for (const auto& line : lines) {
    const Tokens& t = line.second;
    if (line.first == "events") {
      t.num("next", next_seq);
    } else if (line.first == "event") {
      Event e;
      t.num("seq", &e.seq);
      std::string kind;
      t.str("kind", &kind);
      if (!event_kind_from_name(kind, &e.kind)) continue;  // a newer daemon's
      t.str("id", &e.camera_id);
      t.str("name", &e.camera_name);
      t.real("wall", &e.wall);
      t.str("message", &e.message);
      out->push_back(e);
    }
  }
  return true;
}

// ------------------------------------------------------------------ Control

Control::Control() = default;

void Control::set_daemon(const DaemonStatus& d) {
  std::lock_guard<std::mutex> lock(mu_);
  daemon_ = d;
}

void Control::set_bench(const BenchStatus& b) {
  std::lock_guard<std::mutex> lock(mu_);
  bench_ = b;
}

void Control::publish_camera(const CameraStatus& cam) {
  std::lock_guard<std::mutex> lock(mu_);
  for (CameraStatus& c : cameras_) {
    if (c.id == cam.id) {
      c = cam;
      return;
    }
  }
  cameras_.push_back(cam);
}

void Control::set_present(const std::string& id, bool present) {
  std::lock_guard<std::mutex> lock(mu_);
  for (CameraStatus& c : cameras_) {
    if (c.id == id) {
      c.present = present;
      if (!present) c.connected = false;
      return;
    }
  }
}

int64_t Control::queue(const Request& req) {
  std::lock_guard<std::mutex> lock(mu_);
  Entry e;
  e.req = req;
  e.req.id = next_request_id_++;
  e.req.queued_wall = wall_now();
  e.result.id = e.req.id;
  e.result.state = RequestState::kQueued;
  e.result.updated_wall = e.req.queued_wall;
  history_.push_back(e);
  while (history_.size() > kMaxHistory) history_.pop_front();
  return e.req.id;
}

bool Control::take_request(Request* out) {
  std::lock_guard<std::mutex> lock(mu_);
  for (Entry& e : history_) {
    if (e.result.state != RequestState::kQueued) continue;
    e.result.state = RequestState::kRunning;
    e.result.updated_wall = wall_now();
    *out = e.req;
    return true;
  }
  return false;
}

void Control::finish(int64_t id, bool ok, const std::string& message) {
  std::lock_guard<std::mutex> lock(mu_);
  Entry* e = find_locked(id);
  if (e == nullptr) return;
  e->result.state = ok ? RequestState::kDone : RequestState::kFailed;
  e->result.message = message;
  e->result.updated_wall = wall_now();
}

void Control::requeue_running() {
  std::lock_guard<std::mutex> lock(mu_);
  for (Entry& e : history_) {
    if (e.result.state == RequestState::kRunning) {
      e.result.state = RequestState::kQueued;
      e.result.updated_wall = wall_now();
    }
  }
}

void Control::emit(EventKind kind, const std::string& camera_id,
                   const std::string& camera_name,
                   const std::string& message) {
  std::lock_guard<std::mutex> lock(mu_);
  Event e;
  e.seq = next_event_seq_++;
  e.kind = kind;
  e.camera_id = camera_id;
  e.camera_name = camera_name;
  e.message = message;
  e.wall = wall_now();
  events_.push_back(e);
  while (events_.size() > kMaxEvents) events_.pop_front();
}

int Control::queued_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  int n = 0;
  for (const Entry& e : history_) {
    if (e.result.state == RequestState::kQueued) ++n;
  }
  return n;
}

Control::Entry* Control::find_locked(int64_t id) {
  for (Entry& e : history_) {
    if (e.req.id == id) return &e;
  }
  return nullptr;
}

Status Control::snapshot_locked() const {
  Status s;
  s.daemon = daemon_;
  s.daemon.now_wall = wall_now();
  s.bench = bench_;
  s.cameras = cameras_;
  for (const Entry& e : history_) {
    if (e.result.state == RequestState::kQueued) ++s.queued;
  }
  return s;
}

std::string Control::handle(const std::string& line) {
  const Command cmd = parse_command(line);
  if (!cmd.ok) return render_error(cmd.error);
  std::lock_guard<std::mutex> lock(mu_);
  return handle_locked(cmd);
}

std::string Control::handle_locked(const Command& cmd) {
  if (cmd.verb.empty() || cmd.verb == "status" || cmd.verb == "cameras") {
    return render_status(snapshot_locked());
  }
  if (cmd.verb == "json") {
    return render_status_json(snapshot_locked()) + "\n";
  }
  if (cmd.verb == "ping") {
    return banner() + "pong\nend\n";
  }
  if (cmd.verb == "sync" || cmd.verb == "source") {
    Request req;
    req.kind = cmd.verb == "sync" ? RequestKind::kSync : RequestKind::kSetSource;
    req.cameras = cmd.cameras;
    if (req.kind == RequestKind::kSetSource) {
      if (!cmd.has_value) {
        return render_error("source needs value=0 or value=1");
      }
      // Refused here rather than at the camera: the camera clamps anything
      // above 1 to 1, so a client asking for 2 would be told it succeeded and
      // then find 1. Better to say no.
      if (cmd.value != 0 && cmd.value != 1) {
        return render_error("timecode source is 0 (time of day) or 1 (clip)");
      }
      req.source = cmd.value;
    }
    Entry e;
    e.req = req;
    e.req.id = next_request_id_++;
    e.req.queued_wall = wall_now();
    e.result.id = e.req.id;
    e.result.state = RequestState::kQueued;
    e.result.updated_wall = e.req.queued_wall;
    history_.push_back(e);
    while (history_.size() > kMaxHistory) history_.pop_front();
    return render_result(e.result);
  }
  if (cmd.verb == "result") {
    if (!cmd.has_id) return render_error("result needs id=N");
    const Entry* e = const_cast<Control*>(this)->find_locked(cmd.id);
    if (e == nullptr) {
      RequestResult r;
      r.id = cmd.id;
      r.state = RequestState::kUnknown;
      r.message = "no such request";
      return render_result(r);
    }
    return render_result(e->result);
  }
  if (cmd.verb == "events") {
    const int64_t since = cmd.has_since ? cmd.since : 0;
    std::vector<Event> picked;
    for (const Event& e : events_) {
      if (e.seq > since) picked.push_back(e);
    }
    return render_events(picked, next_event_seq_);
  }
  return render_error("unknown command: " + cmd.verb);
}

}  // namespace octo
