#include "camconf.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace octo {

namespace {

const char kHeader[] =
    "# octomancer camera configuration.\n"
    "#\n"
    "# Written by `octomancer writes ...` and by Octomancer.app. The daemon\n"
    "# only ever reads this file, so anything set here stays set.\n"
    "#\n"
    "# Edit it by hand if you like -- comments and ordering are preserved --\n"
    "# and then run `octomancer reload` so the running daemon picks it up.\n"
    "#\n"
    "#   default writes=off\n"
    "#   default enabled=on\n"
    "#   default warn=off\n"
    "#   camera <ble-id> writes=on warn=on name=<label>\n"
    "#   box    <ble-id> enabled=off warn=on name=<label>\n"
    "#\n"
    "# `writes` is permission to change anything on that camera: its clock,\n"
    "# and its timecode source. Off means octomancer will read it and report\n"
    "# on it, and never touch it. New cameras are off until named here.\n"
    "#\n"
    "# `enabled` says whether a timecode box is used at all. New ones are\n"
    "# on: listening to one is passive and costs nothing, so unlike writing\n"
    "# to a camera it needs nobody's permission first. Turn one off here to\n"
    "# stop seeing a timecode box that is not part of this shoot.\n"
    "#\n"
    "# `warn` asks to be told when that device's clock has wandered off the\n"
    "# bench, and to be told separately when it has been quiet long enough\n"
    "# that nobody can say. It goes on either kind of line. New devices are\n"
    "# off: a warning about kit that is not on this shoot is a warning\n"
    "# people learn to ignore, so name the ones that matter and only those\n"
    "# ever light up.\n"
    "\n";

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
  return s.substr(b, e - b);
}

// on/off/true/false/yes/no/1/0, because a configuration file people type into
// should not have opinions about which of those they meant.
bool parse_bool(const std::string& raw, bool* out) {
  std::string v;
  for (char c : raw) v.push_back(static_cast<char>(::tolower(c)));
  if (v == "on" || v == "true" || v == "yes" || v == "1" || v == "enabled") {
    *out = true;
    return true;
  }
  if (v == "off" || v == "false" || v == "no" || v == "0" || v == "disabled") {
    *out = false;
    return true;
  }
  return false;
}

bool make_parents(const std::string& path, std::string* err) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) return true;
  const std::string dir = path.substr(0, slash);
  if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
    if (err) *err = "cannot create " + dir + ": " + strerror(errno);
    return false;
  }
  return true;
}

// Split "key=value" tokens off the rest of a line, leaving the tokens in
// order so a rewrite can put back what it did not change.
struct Field {
  std::string key;
  std::string value;
};

std::vector<Field> fields_of(const std::string& rest) {
  std::vector<Field> out;
  std::istringstream in(rest);
  std::string tok;
  while (in >> tok) {
    const size_t eq = tok.find('=');
    Field f;
    if (eq == std::string::npos) {
      f.key = tok;
    } else {
      f.key = tok.substr(0, eq);
      f.value = tok.substr(eq + 1);
    }
    out.push_back(f);
  }
  return out;
}

// Spaces in a camera name would break the tokenizer, and camera names are
// user-set. Underscores are not a great answer but they are an honest one, and
// the field is only a label -- matching is always on the id.
std::string safe_label(const std::string& name) {
  std::string out;
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    out.push_back(u > 0x20 && u < 0x7f && c != '=' ? c : '_');
  }
  return out;
}

}  // namespace

std::string default_camera_config_path() {
  const char* home = std::getenv("HOME");
  const std::string base = home && *home ? home : "/tmp";
  return base + "/.octomancer/cameras.conf";
}

bool CamConf::load(const std::string& path, std::string* err) {
  path_ = path;
  loaded_ = false;
  exists_ = false;
  default_writes_ = false;
  default_box_enabled_ = true;
  default_warn_ = false;
  cameras_.clear();
  boxes_.clear();

  if (path_.empty()) {
    loaded_ = true;  // explicitly no configuration: everything is off
    return true;
  }

  std::ifstream in(path_);
  if (!in) {
    // Not an error. A missing file says exactly what an empty one would.
    loaded_ = true;
    return true;
  }
  exists_ = true;

  std::ostringstream all;
  all << in.rdbuf();
  if (!parse(all.str(), err)) return false;
  loaded_ = true;
  return true;
}

bool CamConf::reload(std::string* err) { return load(path_, err); }

bool CamConf::parse(const std::string& text, std::string* err) {
  std::istringstream in(text);
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    const std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;

    const size_t sp = t.find_first_of(" \t");
    const std::string verb = sp == std::string::npos ? t : t.substr(0, sp);
    const std::string rest =
        sp == std::string::npos ? std::string() : t.substr(sp + 1);
    const std::vector<Field> fields = fields_of(rest);

    if (verb == "default") {
      for (const Field& f : fields) {
        bool* into = nullptr;
        if (f.key == "writes") into = &default_writes_;
        if (f.key == "enabled") into = &default_box_enabled_;
        if (f.key == "warn") into = &default_warn_;
        if (into == nullptr) continue;
        if (!parse_bool(f.value, into)) {
          if (err) {
            *err = path_ + ":" + std::to_string(lineno) + ": " + f.key +
                   " must be on or off, not '" + f.value + "'";
          }
          return false;
        }
      }
      continue;
    }

    if (verb == "camera") {
      if (fields.empty()) {
        if (err) {
          *err = path_ + ":" + std::to_string(lineno) +
                 ": camera needs an identifier";
        }
        return false;
      }
      CameraConfig cfg;
      cfg.id = fields[0].value.empty() ? fields[0].key : fields[0].value;
      cfg.writes_enabled = default_writes_;
      cfg.warn = default_warn_;
      for (size_t i = 1; i < fields.size(); ++i) {
        const Field& f = fields[i];
        if (f.key == "writes") {
          if (!parse_bool(f.value, &cfg.writes_enabled)) {
            if (err) {
              *err = path_ + ":" + std::to_string(lineno) +
                     ": writes must be on or off, not '" + f.value + "'";
            }
            return false;
          }
        } else if (f.key == "warn") {
          if (!parse_bool(f.value, &cfg.warn)) {
            if (err) {
              *err = path_ + ":" + std::to_string(lineno) +
                     ": warn must be on or off, not '" + f.value + "'";
            }
            return false;
          }
        } else if (f.key == "name") {
          cfg.name = f.value;
        }
        // Anything else is left alone rather than rejected: a file written by
        // a newer version must not stop an older daemon from starting.
      }
      cameras_.push_back(cfg);
      continue;
    }

    if (verb == "box") {
      if (fields.empty()) {
        if (err) {
          *err = path_ + ":" + std::to_string(lineno) +
                 ": box needs an identifier";
        }
        return false;
      }
      BoxConfig cfg;
      cfg.id = fields[0].value.empty() ? fields[0].key : fields[0].value;
      cfg.enabled = default_box_enabled_;
      cfg.warn = default_warn_;
      for (size_t i = 1; i < fields.size(); ++i) {
        const Field& f = fields[i];
        if (f.key == "enabled") {
          if (!parse_bool(f.value, &cfg.enabled)) {
            if (err) {
              *err = path_ + ":" + std::to_string(lineno) +
                     ": enabled must be on or off, not '" + f.value + "'";
            }
            return false;
          }
        } else if (f.key == "warn") {
          if (!parse_bool(f.value, &cfg.warn)) {
            if (err) {
              *err = path_ + ":" + std::to_string(lineno) +
                     ": warn must be on or off, not '" + f.value + "'";
            }
            return false;
          }
        } else if (f.key == "name") {
          cfg.name = f.value;
        }
        // Unknown keys survive here for the same reason they do on a camera
        // line: a newer version's file must still start an older daemon.
      }
      boxes_.push_back(cfg);
      continue;
    }

    // Same reasoning: an unknown directive is ignored, not fatal.
  }
  return true;
}

const CameraConfig* CamConf::find(const std::string& id) const {
  for (const CameraConfig& c : cameras_) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

bool CamConf::writes_enabled(const std::string& id) const {
  const CameraConfig* c = find(id);
  return c != nullptr ? c->writes_enabled : default_writes_;
}

const BoxConfig* CamConf::find_box(const std::string& id) const {
  for (const BoxConfig& b : boxes_) {
    if (b.id == id) return &b;
  }
  return nullptr;
}

bool CamConf::box_enabled(const std::string& id) const {
  const BoxConfig* b = find_box(id);
  return b != nullptr ? b->enabled : default_box_enabled_;
}

// The one question that crosses the two id spaces, because the person asking
// it is looking at one bench and does not think of it as two lists. A camera
// answers first only because that is an order, not a precedence: an id in
// both lists is not a thing that happens.
bool CamConf::warn_enabled(const std::string& id) const {
  const CameraConfig* c = find(id);
  if (c != nullptr) return c->warn;
  const BoxConfig* b = find_box(id);
  if (b != nullptr) return b->warn;
  return default_warn_;
}

bool CamConf::any_writes_enabled() const {
  if (default_writes_) return true;
  for (const CameraConfig& c : cameras_) {
    if (c.writes_enabled) return true;
  }
  return false;
}

bool CamConf::set_writes(const std::string& id, const std::string& name,
                         bool enabled, std::string* err) {
  return set_flag("camera", "writes", id, name, enabled, err);
}

bool CamConf::set_box_enabled(const std::string& id, const std::string& name,
                              bool enabled, std::string* err) {
  return set_flag("box", "enabled", id, name, enabled, err);
}

bool CamConf::set_camera_warn(const std::string& id, const std::string& name,
                              bool warn, std::string* err) {
  return set_flag("camera", "warn", id, name, warn, err);
}

bool CamConf::set_box_warn(const std::string& id, const std::string& name,
                           bool warn, std::string* err) {
  return set_flag("box", "warn", id, name, warn, err);
}

// Read what is there, so a rewrite can put back every line it has no reason
// to touch. This file belongs to a person: comments, spacing and settings from
// a version that has not been written yet all survive being edited from here.
bool CamConf::read_lines(std::vector<std::string>* lines, bool* had_file,
                         std::string* err) {
  if (path_.empty()) {
    if (err) *err = "no configuration file to write to";
    return false;
  }
  if (!make_parents(path_, err)) return false;
  *had_file = false;
  std::ifstream in(path_);
  if (in) {
    *had_file = true;
    std::string line;
    while (std::getline(in, line)) lines->push_back(line);
  }
  return true;
}

// Written to a temporary and renamed, so an interrupted write cannot leave a
// half-file that the daemon would then refuse to parse.
bool CamConf::write_lines(const std::vector<std::string>& lines, bool had_file,
                          std::string* err) {
  const std::string tmp = path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      if (err) *err = "cannot write " + tmp + ": " + strerror(errno);
      return false;
    }
    if (!had_file) out << kHeader;
    for (const std::string& line : lines) out << line << "\n";
    if (!out) {
      if (err) *err = "short write to " + tmp;
      return false;
    }
  }
  if (::rename(tmp.c_str(), path_.c_str()) != 0) {
    if (err) *err = "cannot replace " + path_ + ": " + strerror(errno);
    ::unlink(tmp.c_str());
    return false;
  }
  std::string ignored;
  return load(path_, &ignored);
}

// Drop every line this file holds about one device.
//
// Not the same operation as switching it off, and the difference is the whole
// point of having both. `enabled=off` is a decision somebody made and wants
// remembered; this is somebody saying they never want to be asked about the
// device again, and a remembered "off" would be exactly that -- a row on the
// device page, forever, for a box sold years ago. So the line goes.
//
// Which also means the defaults come back. A box removed here reappears the
// next time one advertises, switched on, because that is what a box nobody
// has an opinion about does. Somebody who wants it gone and staying gone
// wants it switched off instead, and that is the honest answer to give them.
bool CamConf::forget_camera(const std::string& id, std::string* err) {
  return forget_device("camera", id, err);
}

bool CamConf::forget_box(const std::string& id, std::string* err) {
  return forget_device("box", id, err);
}

bool CamConf::forget_device(const char* verb, const std::string& id,
                            std::string* err) {
  std::vector<std::string> lines;
  bool had_file = false;
  if (!read_lines(&lines, &had_file, err)) return false;
  if (!had_file) return true;  // nothing on file to forget

  std::vector<std::string> kept;
  kept.reserve(lines.size());
  for (const std::string& line : lines) {
    const std::string t = trim(line);
    bool drop = false;
    if (!t.empty() && t[0] != '#') {
      const size_t sp = t.find_first_of(" \t");
      if (sp != std::string::npos && t.substr(0, sp) == verb) {
        // Same rule as set_flag: a camera line and a box line are different
        // id spaces, so the verb has to match before the id means anything.
        std::vector<Field> fields = fields_of(t.substr(sp + 1));
        if (!fields.empty()) {
          const std::string this_id =
              fields[0].value.empty() ? fields[0].key : fields[0].value;
          drop = this_id == id;
        }
      }
    }
    if (!drop) kept.push_back(line);
  }
  if (kept.size() == lines.size()) return true;  // nothing matched
  return write_lines(kept, had_file, err);
}

bool CamConf::set_flag(const char* verb, const char* key,
                       const std::string& id, const std::string& name,
                       bool enabled, std::string* err) {
  std::vector<std::string> lines;
  bool had_file = false;
  if (!read_lines(&lines, &had_file, err)) return false;

  const std::string value = enabled ? "on" : "off";
  bool replaced = false;
  for (std::string& line : lines) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    const size_t sp = t.find_first_of(" \t");
    if (sp == std::string::npos) continue;
    // A camera line and a box line are different device classes over different
    // id spaces, so matching the id is not enough: the verb has to match too,
    // or turning a box off would rewrite a camera that happened to share it.
    if (t.substr(0, sp) != verb) continue;

    std::vector<Field> fields = fields_of(t.substr(sp + 1));
    if (fields.empty()) continue;
    const std::string this_id =
        fields[0].value.empty() ? fields[0].key : fields[0].value;
    if (this_id != id) continue;

    // Rebuild this line, keeping fields we do not own in their original
    // order and only replacing the value of our own key. That rule was
    // written for settings from a future version, but it is what makes a line
    // able to carry two of ours at once: `warn=` is somebody else's field as
    // far as `writes=` is concerned, so switching one on never disturbs the
    // other. Anything that changes this loop has to keep that true.
    std::string rebuilt = std::string(verb) + " " + this_id;
    bool wrote_flag = false;
    bool wrote_name = false;
    for (size_t i = 1; i < fields.size(); ++i) {
      const Field& f = fields[i];
      if (f.key == key) {
        rebuilt += " " + std::string(key) + "=" + value;
        wrote_flag = true;
      } else if (f.key == "name") {
        rebuilt += " name=" + (name.empty() ? f.value : safe_label(name));
        wrote_name = true;
      } else {
        rebuilt += " " + f.key + (f.value.empty() ? "" : "=" + f.value);
      }
    }
    if (!wrote_flag) rebuilt += " " + std::string(key) + "=" + value;
    if (!wrote_name && !name.empty()) rebuilt += " name=" + safe_label(name);
    line = rebuilt;
    replaced = true;
    break;
  }

  if (!replaced) {
    std::string added =
        std::string(verb) + " " + id + " " + std::string(key) + "=" + value;
    if (!name.empty()) added += " name=" + safe_label(name);
    lines.push_back(added);
  }

  return write_lines(lines, had_file, err);
}

}  // namespace octo
