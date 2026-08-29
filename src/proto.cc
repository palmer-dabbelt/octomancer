#include "proto.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

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

std::string json_number(double v, int digits = 4) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", digits, v);
  return buf;
}

}  // namespace

std::string render_text(const Snapshot& s) {
  std::string out;
  out += "octomancer " + std::to_string(kProtocolVersion) + "\n";

  out += "snapshot";
  put(&out, "wall", s.wall, 3);
  put(&out, "uptime", s.uptime, 1);
  put(&out, "radio", s.radio);
  put(&out, "adverts", static_cast<long long>(s.adverts_total));
  put(&out, "undecodable", static_cast<long long>(s.undecodable_total));
  put(&out, "clock_steps", static_cast<long long>(s.clock_steps));
  put(&out, "devices", static_cast<long long>(s.devices));
  put(&out, "live", static_cast<long long>(s.live));
  put(&out, "alerting", static_cast<long long>(s.alerting));
  put(&out, "alert_threshold", s.alert_threshold, 1);
  put(&out, "has_bench", static_cast<long long>(s.has_bench ? 1 : 0));
  if (s.has_bench) {
    put(&out, "bench_offset", s.bench_offset);
    put(&out, "bench_spread", s.bench_spread);
  }
  out += "\n";

  // Always emitted, even before a camera has ever been heard. A reader has to
  // be able to tell "this daemon is watching and there is no camera" from
  // "this daemon is too old to be watching", and the difference between those
  // two is a twenty-second scan every cycle.
  if (s.camera.reported) {
    out += "camera";
    put(&out, "seen", static_cast<long long>(s.camera.seen ? 1 : 0));
    put(&out, "id", s.camera.id);
    put(&out, "name", s.camera.name);
    put(&out, "present", static_cast<long long>(s.camera.present ? 1 : 0));
    put(&out, "rssi", static_cast<long long>(s.camera.rssi));
    put(&out, "age", s.camera.age, 2);
    put(&out, "since", s.camera.since, 2);
    put(&out, "sessions", static_cast<long long>(s.camera.sessions));
    put(&out, "adverts", static_cast<long long>(s.camera.adverts));
    put(&out, "up_wall", s.camera.up_wall, 3);
    out += "\n";
  }

  for (const DeviceSnapshot& d : s.device) {
    out += "device";
    put(&out, "id", d.id);
    put(&out, "name", d.name);
    put(&out, "rssi", static_cast<long long>(d.rssi));
    put(&out, "adverts", static_cast<long long>(d.adverts));
    put(&out, "decoded", static_cast<long long>(d.decoded));
    put(&out, "age", d.age, 2);
    put(&out, "first_seen", d.first_seen_wall, 3);
    put(&out, "live", static_cast<long long>(d.live ? 1 : 0));
    put(&out, "has_time", static_cast<long long>(d.has_time ? 1 : 0));
    if (d.has_time) {
      put(&out, "sod", d.sod, 6);
      put(&out, "offset", d.offset, 6);
      put(&out, "median", d.median_offset, 6);
      put(&out, "samples", static_cast<long long>(d.samples));
    }
    put(&out, "display", d.display);
    put(&out, "resolution", d.resolution);
    put(&out, "fps", static_cast<long long>(d.fps));
    if (!d.note.empty()) put(&out, "note", d.note);
    put(&out, "has_drift", static_cast<long long>(d.has_drift ? 1 : 0));
    if (d.has_drift) {
      put(&out, "drift_ppm", d.drift_ppm, 2);
      put(&out, "drift_span", d.drift_span, 1);
    }
    put(&out, "alerting", static_cast<long long>(d.alerting ? 1 : 0));
    if (d.alerting) put(&out, "alert_since", d.alert_since_wall, 3);
    out += "\n";
  }

  out += "end\n";
  return out;
}

std::string render_json(const Snapshot& s) {
  std::string out = "{";
  out += "\"protocol\":" + std::to_string(kProtocolVersion);
  out += ",\"wall\":" + json_number(s.wall, 3);
  out += ",\"uptime\":" + json_number(s.uptime, 1);
  out += ",\"radio\":" + json_string(s.radio);
  out += ",\"adverts\":" + std::to_string(s.adverts_total);
  out += ",\"undecodable\":" + std::to_string(s.undecodable_total);
  out += ",\"clock_steps\":" + std::to_string(s.clock_steps);
  out += ",\"devices\":" + std::to_string(s.devices);
  out += ",\"live\":" + std::to_string(s.live);
  out += ",\"alerting\":" + std::to_string(s.alerting);
  out += ",\"alert_threshold\":" + json_number(s.alert_threshold, 1);
  if (s.has_bench) {
    out += ",\"bench_offset\":" + json_number(s.bench_offset);
    out += ",\"bench_spread\":" + json_number(s.bench_spread);
  } else {
    out += ",\"bench_offset\":null,\"bench_spread\":null";
  }
  if (s.camera.reported) {
    out += ",\"camera\":{";
    out += "\"seen\":" + std::string(s.camera.seen ? "true" : "false");
    out += ",\"id\":" + json_string(s.camera.id);
    out += ",\"name\":" + json_string(s.camera.name);
    out += ",\"present\":" + std::string(s.camera.present ? "true" : "false");
    out += ",\"rssi\":" + std::to_string(s.camera.rssi);
    out += ",\"age\":" + json_number(s.camera.age, 2);
    out += ",\"since\":" + json_number(s.camera.since, 2);
    out += ",\"sessions\":" + std::to_string(s.camera.sessions);
    out += ",\"adverts\":" + std::to_string(s.camera.adverts);
    out += ",\"up_wall\":" + json_number(s.camera.up_wall, 3);
    out += "}";
  } else {
    out += ",\"camera\":null";
  }
  out += ",\"box\":[";
  bool first = true;
  for (const DeviceSnapshot& d : s.device) {
    if (!first) out += ",";
    first = false;
    out += "{";
    out += "\"id\":" + json_string(d.id);
    out += ",\"name\":" + json_string(d.name);
    out += ",\"rssi\":" + std::to_string(d.rssi);
    out += ",\"adverts\":" + std::to_string(d.adverts);
    out += ",\"decoded\":" + std::to_string(d.decoded);
    out += ",\"age\":" + json_number(d.age, 2);
    out += ",\"live\":" + std::string(d.live ? "true" : "false");
    if (d.has_time) {
      out += ",\"sod\":" + json_number(d.sod, 6);
      out += ",\"offset\":" + json_number(d.offset, 6);
      out += ",\"median_offset\":" + json_number(d.median_offset, 6);
      out += ",\"samples\":" + std::to_string(d.samples);
    } else {
      out += ",\"sod\":null,\"offset\":null,\"median_offset\":null,\"samples\":0";
    }
    out += ",\"display\":" + json_string(d.display);
    out += ",\"resolution\":" + json_string(d.resolution);
    out += ",\"fps\":" + std::to_string(d.fps);
    out += ",\"note\":" + json_string(d.note);
    if (d.has_drift) {
      out += ",\"drift_ppm\":" + json_number(d.drift_ppm, 2);
      out += ",\"drift_span\":" + json_number(d.drift_span, 1);
    } else {
      out += ",\"drift_ppm\":null,\"drift_span\":null";
    }
    out += ",\"alerting\":" + std::string(d.alerting ? "true" : "false");
    out += "}";
  }
  out += "]}";
  return out;
}

namespace {

bool split_kv(const std::string& token, std::string* key, std::string* value) {
  const size_t eq = token.find('=');
  if (eq == std::string::npos) return false;
  *key = token.substr(0, eq);
  *value = unescape(token.substr(eq + 1));
  return true;
}

double to_double(const std::string& s) { return std::strtod(s.c_str(), nullptr); }
long long to_int(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }

}  // namespace

bool parse_text(const std::string& text, Snapshot* out, std::string* err) {
  std::istringstream in(text);
  std::string line;
  bool saw_header = false, saw_snapshot = false, saw_end = false;
  *out = Snapshot();

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    std::istringstream fields(line);
    std::string verb;
    fields >> verb;

    if (verb == "octomancer") {
      int version = 0;
      fields >> version;
      // Refuse a protocol we do not know rather than misreading it. The
      // version is why unknown *keys* can be ignored safely: a breaking change
      // arrives as a new number, not as a surprising field.
      if (version != kProtocolVersion) {
        if (err) *err = "unsupported protocol version " + std::to_string(version);
        return false;
      }
      saw_header = true;
      continue;
    }
    if (verb == "error") {
      std::string rest;
      std::getline(fields, rest);
      if (err) *err = "daemon:" + rest;
      return false;
    }
    if (verb == "end") {
      saw_end = true;
      break;
    }

    if (verb == "snapshot") {
      saw_snapshot = true;
      std::string token, key, value;
      while (fields >> token) {
        if (!split_kv(token, &key, &value)) continue;
        if (key == "wall") out->wall = to_double(value);
        else if (key == "uptime") out->uptime = to_double(value);
        else if (key == "radio") out->radio = value;
        else if (key == "adverts") out->adverts_total = to_int(value);
        else if (key == "undecodable") out->undecodable_total = to_int(value);
        else if (key == "clock_steps") out->clock_steps = to_int(value);
        else if (key == "devices") out->devices = static_cast<int>(to_int(value));
        else if (key == "live") out->live = static_cast<int>(to_int(value));
        else if (key == "alerting") out->alerting = static_cast<int>(to_int(value));
        else if (key == "alert_threshold") out->alert_threshold = to_double(value);
        else if (key == "has_bench") out->has_bench = to_int(value) != 0;
        else if (key == "bench_offset") out->bench_offset = to_double(value);
        else if (key == "bench_spread") out->bench_spread = to_double(value);
      }
      continue;
    }

    if (verb == "camera") {
      out->camera.reported = true;
      // A daemon that emits the line but not the field is one from the brief
      // window where the line only appeared once a camera had been heard;
      // treating that as "seen" is what it meant.
      out->camera.seen = true;
      std::string token, key, value;
      while (fields >> token) {
        if (!split_kv(token, &key, &value)) continue;
        if (key == "seen") out->camera.seen = to_int(value) != 0;
        else if (key == "id") out->camera.id = value;
        else if (key == "name") out->camera.name = value;
        else if (key == "present") out->camera.present = to_int(value) != 0;
        else if (key == "rssi") out->camera.rssi = static_cast<int>(to_int(value));
        else if (key == "age") out->camera.age = to_double(value);
        else if (key == "since") out->camera.since = to_double(value);
        else if (key == "sessions") out->camera.sessions = to_int(value);
        else if (key == "adverts") out->camera.adverts = to_int(value);
        else if (key == "up_wall") out->camera.up_wall = to_double(value);
      }
      continue;
    }

    if (verb == "device") {
      DeviceSnapshot d;
      std::string token, key, value;
      while (fields >> token) {
        if (!split_kv(token, &key, &value)) continue;
        if (key == "id") d.id = value;
        else if (key == "name") d.name = value;
        else if (key == "rssi") d.rssi = static_cast<int>(to_int(value));
        else if (key == "adverts") d.adverts = to_int(value);
        else if (key == "decoded") d.decoded = to_int(value);
        else if (key == "age") d.age = to_double(value);
        else if (key == "first_seen") d.first_seen_wall = to_double(value);
        else if (key == "live") d.live = to_int(value) != 0;
        else if (key == "has_time") d.has_time = to_int(value) != 0;
        else if (key == "sod") d.sod = to_double(value);
        else if (key == "offset") d.offset = to_double(value);
        else if (key == "median") d.median_offset = to_double(value);
        else if (key == "samples") d.samples = static_cast<int>(to_int(value));
        else if (key == "display") d.display = value;
        else if (key == "resolution") d.resolution = value;
        else if (key == "fps") d.fps = static_cast<int>(to_int(value));
        else if (key == "note") d.note = value;
        else if (key == "has_drift") d.has_drift = to_int(value) != 0;
        else if (key == "drift_ppm") d.drift_ppm = to_double(value);
        else if (key == "drift_span") d.drift_span = to_double(value);
        else if (key == "alerting") d.alerting = to_int(value) != 0;
        else if (key == "alert_since") d.alert_since_wall = to_double(value);
        // Anything else is from a newer daemon: ignore it and carry on.
      }
      out->device.push_back(std::move(d));
      continue;
    }
    // An unknown verb is likewise a newer daemon talking; skip the line.
  }

  if (!saw_header) {
    if (err) *err = "no protocol header";
    return false;
  }
  if (!saw_snapshot || !saw_end) {
    if (err) *err = "truncated snapshot";
    return false;
  }
  return true;
}

}  // namespace octo
