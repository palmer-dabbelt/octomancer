#include "fakebench.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "tentacle.h"

namespace octo {
namespace {

// Seconds since local midnight for a wall-clock instant. The boxes report
// time of day, so a fake one has to know what time of day it is for the same
// reason a real one does.
double seconds_of_day(double wall) {
  const time_t whole = static_cast<time_t>(std::floor(wall));
  struct tm local;
  localtime_r(&whole, &local);
  return local.tm_hour * 3600.0 + local.tm_min * 60.0 + local.tm_sec +
         (wall - std::floor(wall));
}

bool visible(double interval, double silent_after, double returns_after,
             double mono) {
  (void)interval;
  if (silent_after >= 0.0 && mono >= silent_after) {
    if (returns_after >= 0.0 && mono >= returns_after) return true;
    return false;
  }
  return true;
}

// Every multiple of `interval` strictly after `since` and no later than `mono`.
// Half-open on purpose: a caller polling twice with the same `mono` must not
// be handed the same advert twice, and one polling late must not lose the
// adverts it slept through.
std::vector<double> ticks(double interval, double since, double mono) {
  std::vector<double> out;
  if (interval <= 0.0) return out;
  // A pathologically small interval would otherwise be asked for millions of
  // adverts across a long window. Clamped rather than capped further down,
  // because clamping changes a number the caller can see in the spec and a cap
  // would silently return fewer adverts than the window contains -- which
  // looks exactly like packet loss to whatever is being tested, and is the
  // sort of thing that gets debugged for an hour in the wrong file.
  if (interval < 0.001) interval = 0.001;
  // A first call (`since` negative) emits one advert immediately rather than
  // waiting a whole interval, so a program that starts and asks once does not
  // conclude the bench is empty.
  double first = since < 0.0 ? 0.0 : std::floor(since / interval) * interval + interval;
  for (double t = first; t <= mono + 1e-9; t += interval) {
    if (since >= 0.0 && t <= since + 1e-9) continue;
    out.push_back(t);
  }
  return out;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return std::string();
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string field;
  std::istringstream in(s);
  while (std::getline(in, field, sep)) out.push_back(trim(field));
  return out;
}

bool to_double(const std::string& s, double* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0') return false;
  *out = v;
  return true;
}

bool to_int(const std::string& s, int* out) {
  double v = 0.0;
  if (!to_double(s, &v)) return false;
  *out = static_cast<int>(v);
  return true;
}

bool parse_kind(const std::string& s, FakeBox::Kind* out) {
  if (s == "frame+us" || s == "frameus") *out = FakeBox::Kind::kFrameMicros;
  else if (s == "frame") *out = FakeBox::Kind::kFrame;
  else if (s == "us" || s == "microsecond") *out = FakeBox::Kind::kMicros;
  else if (s == "static") *out = FakeBox::Kind::kStatic;
  else return false;
  return true;
}

// A stable id for a box named in a spec. CoreBluetooth hands out per-host
// UUIDs, so a fake one should look like a UUID rather than like a name -- a
// program that only works because the id happened to be readable would pass
// here and fail on hardware.
std::string fake_id(const std::string& name) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : name) {
    h ^= c;
    h *= 1099511628211ull;
  }
  char buf[64];
  std::snprintf(buf, sizeof buf, "%08X-0000-4000-8000-%012llX",
                static_cast<unsigned>(h >> 32),
                static_cast<unsigned long long>(h & 0xFFFFFFFFFFFFull));
  return buf;
}

}  // namespace

double box_clock(const FakeBox& box, double mono, double wall0) {
  // Drift is expressed against elapsed run time, which is what makes a fake
  // box's error grow the way a real one's does.
  const double drifted = box.offset_s + box.drift_ppm * 1e-6 * mono;
  return seconds_of_day(wall0 + mono) + drifted;
}

std::vector<Advert> adverts_between(const FakeBench& bench, double since,
                                    double mono, double mono0, double wall0) {
  std::vector<Advert> out;
  for (const FakeBox& box : bench.boxes) {
    for (double t : ticks(box.interval_s, since, mono)) {
      if (!visible(box.interval_s, box.silent_after_s, box.returns_after_s, t)) {
        continue;
      }
      Advert a;
      a.id = box.id;
      a.name = box.name;
      a.rssi = box.rssi;
      a.mono = mono0 + t;
      a.wall = wall0 + t;
      const double sod = box_clock(box, t, wall0);
      switch (box.kind) {
        case FakeBox::Kind::kFrameMicros:
          a.data = encode_timecode(sod, box.fps, true);
          break;
        case FakeBox::Kind::kFrame:
          a.data = encode_timecode(sod, box.fps, false);
          break;
        case FakeBox::Kind::kMicros:
          a.data = encode_micros(sod);
          break;
        case FakeBox::Kind::kStatic:
          a.data = encode_static();
          break;
      }
      out.push_back(a);
    }
  }
  return out;
}

std::vector<Sighting> sightings_between(const FakeBench& bench, double since,
                                        double mono, double mono0,
                                        double wall0) {
  std::vector<Sighting> out;
  if (!bench.has_camera) return out;
  const FakeCamera& c = bench.camera;
  for (double t : ticks(c.interval_s, since, mono)) {
    if (!visible(c.interval_s, c.silent_after_s, c.returns_after_s, t)) continue;
    Sighting s;
    s.id = c.id;
    s.name = c.name;
    s.rssi = c.rssi;
    s.mono = mono0 + t;
    s.wall = wall0 + t;
    out.push_back(s);
  }
  return out;
}

FakeBench FakeBench::standard() {
  FakeBench b;
  struct Spec {
    const char* name;
    double offset;
    double drift;
    FakeBox::Kind kind;
    int rssi;
  };
  // The offsets and drifts are the real bench's, rounded: five boxes about
  // -3.59 s off this Mac, agreeing with each other to about 25 ms, all
  // drifting around -23 ppm. A default that resembled nothing would make the
  // fake radio useless for the thing it is for.
  const Spec specs[] = {
      {"Krysta", -3.5928, -23.1, FakeBox::Kind::kMicros, -70},
      {"BMPCC", -3.5780, -21.9, FakeBox::Kind::kFrameMicros, -49},
      {"F55", -3.6028, -20.9, FakeBox::Kind::kFrameMicros, -78},
      {"FS5", -3.5850, -22.1, FakeBox::Kind::kFrameMicros, -80},
      {"FS7", -3.5983, -22.4, FakeBox::Kind::kFrameMicros, -82},
  };
  for (const Spec& s : specs) {
    FakeBox box;
    box.name = s.name;
    box.id = fake_id(s.name);
    box.offset_s = s.offset;
    box.drift_ppm = s.drift;
    box.kind = s.kind;
    box.rssi = s.rssi;
    b.boxes.push_back(box);
  }
  // The weakest box goes quiet a minute in and comes back a minute later. A
  // bench where nothing ever leaves cannot exercise the paths that decide a
  // box has stopped voting, and those are the ones that decide what the
  // canonical time is.
  b.boxes.back().silent_after_s = 60.0;
  b.boxes.back().returns_after_s = 120.0;

  b.has_camera = true;
  b.camera.id = "A:1EAE18A7";
  b.camera.name = "A:1EAE18A7";
  b.camera.error_s = -0.25;
  b.camera.drift_ppm = 8.0;
  return b;
}

bool FakeBench::parse(const std::string& spec_in, FakeBench* out,
                      std::string* err) {
  if (out == nullptr) return false;
  std::string spec = trim(spec_in);
  if (!spec.empty() && spec[0] == '@') {
    const std::string path = trim(spec.substr(1));
    std::ifstream in(path);
    if (!in) {
      if (err) *err = "cannot read fake bench from " + path;
      return false;
    }
    std::ostringstream all;
    all << in.rdbuf();
    spec = all.str();
    // A file may use newlines where the one-line form uses ';'.
    for (char& c : spec) {
      if (c == '\n') c = ';';
    }
  }
  if (trim(spec).empty()) {
    *out = FakeBench::standard();
    return true;
  }

  FakeBench bench;
  for (const std::string& item : split(spec, ';')) {
    if (item.empty() || item[0] == '#') continue;
    const std::vector<std::string> f = split(item, ',');
    if (f.empty()) continue;

    if (f[0] == "box") {
      if (f.size() < 3) {
        if (err) *err = "box needs at least a name and an offset: " + item;
        return false;
      }
      FakeBox box;
      box.name = f[1];
      box.id = fake_id(f[1]);
      if (!to_double(f[2], &box.offset_s)) {
        if (err) *err = "not a number: " + f[2];
        return false;
      }
      if (f.size() > 3 && !f[3].empty() && !to_int(f[3], &box.fps)) {
        if (err) *err = "not a frame rate: " + f[3];
        return false;
      }
      if (f.size() > 4 && !f[4].empty() && !parse_kind(f[4], &box.kind)) {
        if (err) {
          *err = "not one of frame+us, frame, us, static: " + f[4];
        }
        return false;
      }
      if (f.size() > 5 && !f[5].empty() && !to_double(f[5], &box.drift_ppm)) {
        if (err) *err = "not a drift in ppm: " + f[5];
        return false;
      }
      bench.boxes.push_back(box);
      continue;
    }

    if (f[0] == "cam") {
      if (f.size() < 4) {
        if (err) *err = "cam needs an id, a name and an error: " + item;
        return false;
      }
      bench.has_camera = true;
      bench.camera.id = f[1];
      bench.camera.name = f[2];
      if (!to_double(f[3], &bench.camera.error_s)) {
        if (err) *err = "not a number: " + f[3];
        return false;
      }
      if (f.size() > 4 && !f[4].empty() &&
          !to_int(f[4], &bench.camera.fps)) {
        if (err) *err = "not a frame rate: " + f[4];
        return false;
      }
      continue;
    }

    if (err) *err = "not a box or a cam: " + item;
    return false;
  }

  *out = bench;
  return true;
}

}  // namespace octo
