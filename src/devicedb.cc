#include "devicedb.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "jsonlog.h"
#include "logscan.h"

namespace octo {

std::string DeviceDb::default_path() {
  const char* home = std::getenv("HOME");
  const std::string base = home && *home ? home : "/tmp";
  return base + "/.octomancer/devices.json";
}

bool DeviceDb::load(const std::string& path, std::string* err) {
  devices_.clear();
  std::ifstream in(path);
  if (!in) return true;  // no file yet is the ordinary first run

  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    if (line.empty()) continue;
    // The file carries a one-line header for whoever opens it in an editor,
    // and cameras.conf sets the precedent that '#' is a comment.
    if (line[0] == '#') continue;
    LogRecord rec;
    if (!parse_record(line, &rec)) {
      if (err) {
        *err = path + ":" + std::to_string(lineno) +
               ": not a flat JSON object";
      }
      devices_.clear();
      return false;
    }
    if (rec.text("t") != "device") continue;  // room for other record types

    RememberedDevice d;
    d.id = rec.text("id");
    if (d.id.empty()) {
      if (err) *err = path + ":" + std::to_string(lineno) + ": no id";
      devices_.clear();
      return false;
    }
    d.name = rec.text("name");
    d.first_seen_wall = rec.number("first_seen");
    d.last_seen_wall = rec.number("last_seen");
    d.has_time = rec.flag("has_time");
    d.offset = rec.number("offset");
    d.median_offset = rec.number("median");
    d.resolution = rec.text("resolution");
    d.fps = static_cast<int>(rec.number("fps"));
    d.rssi = static_cast<int>(rec.number("rssi"));
    devices_.push_back(d);
  }
  return true;
}

bool DeviceDb::save(const std::string& path,
                    const std::vector<RememberedDevice>& devices,
                    std::string* err) const {
  make_parents(path);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      if (err) *err = "cannot write " + tmp + ": " + strerror(errno);
      return false;
    }
    out << "# octomancerd's device roster. Rewritten whole on every save.\n";
    for (const RememberedDevice& d : devices) {
      std::ostringstream line;
      line.setf(std::ios::fixed);
      line << "{\"t\":\"device\"";
      line << ",\"id\":\"" << json_escape(d.id) << "\"";
      line << ",\"name\":\"" << json_escape(d.name) << "\"";
      line.precision(3);
      line << ",\"first_seen\":" << d.first_seen_wall;
      line << ",\"last_seen\":" << d.last_seen_wall;
      line << ",\"has_time\":" << (d.has_time ? "true" : "false");
      line.precision(6);
      line << ",\"offset\":" << d.offset;
      line << ",\"median\":" << d.median_offset;
      line << ",\"resolution\":\"" << json_escape(d.resolution) << "\"";
      line << ",\"fps\":" << d.fps;
      line << ",\"rssi\":" << d.rssi;
      line << "}\n";
      out << line.str();
    }
    if (!out) {
      if (err) *err = "cannot write " + tmp;
      return false;
    }
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    if (err) {
      *err = "cannot replace " + path + ": " + strerror(errno);
    }
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace octo
