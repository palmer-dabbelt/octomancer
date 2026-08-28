#include "devices.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "bmd.h"
#include "timeutil.h"

namespace octo {

namespace {

// These four are lifted from render.cc rather than shared with it. They live
// in an anonymous namespace there -- internal linkage, so there is nothing to
// call from here -- and promoting them to a public interface would mean
// deciding that the box table's private formatting choices are an API, which
// they are not. Copying thirty lines is the cheaper mistake, but the two
// copies must agree: an offset printed one way in `octomancer status` and
// another way on the devices page is a bug report waiting to happen.
struct Style {
  const char* dim;
  const char* bold;
  const char* red;
  const char* yellow;
  const char* green;
  const char* off;
};

Style style_for(bool color) {
  if (color) {
    return {"\033[2m", "\033[1m", "\033[31m",
            "\033[33m", "\033[32m", "\033[0m"};
  }
  return {"", "", "", "", "", ""};
}

std::string fmt(const char* format, ...) __attribute__((format(printf, 1, 2)));

std::string fmt(const char* format, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, format);
  std::vsnprintf(buf, sizeof buf, format, ap);
  va_end(ap);
  return buf;
}

// Offsets here span microseconds to minutes, and a fixed number of decimals
// either drowns the interesting digits or invents precision that is not there.
std::string offset_text(double seconds) {
  const double mag = std::fabs(seconds);
  if (mag < 1.0) return fmt("%+.1fms", seconds * 1000.0);
  if (mag < 60.0) return fmt("%+.3fs", seconds);
  return fmt("%+.1fs", seconds);
}

bool box_is_enabled(const CamConf* conf, const std::string& id) {
  return conf == nullptr || conf->box_enabled(id);
}

bool camera_is_enabled(const CamConf* conf, const std::string& id) {
  return conf == nullptr || conf->writes_enabled(id);
}

// A box votes on the canonical time when somebody has left it enabled, we are
// still hearing it, and it has actually told us a time. The last condition is
// easy to forget: a box heard for the first time a moment ago is live and has
// a median_offset of zero, and letting that zero into the median would drag
// the whole bench towards this Mac's clock for no reason.
bool box_votes(const CamConf* conf, const DeviceSnapshot& d) {
  return d.live && d.has_time && box_is_enabled(conf, d.id);
}

std::string camera_note(const CameraStatus& c) {
  // The precedence the UI already uses, in the order a person would say them
  // out loud: what the camera is doing beats what it is configured to do,
  // which beats how the last cycle ended.
  if (c.recording) return "recording";
  if (c.has_source && c.source != bmd::kTimecodeSourceTimeOfDay) {
    return "timecode does not follow the clock";
  }
  return c.action;
}

int kind_order(DeviceKind k) { return k == DeviceKind::kTentacle ? 0 : 1; }

}  // namespace

const char* link_state_name(LinkState s) {
  switch (s) {
    case LinkState::kHeld: return "held";
    case LinkState::kOnTheAir: return "on the air";
    case LinkState::kOffTheAir: return "off the air";
    case LinkState::kUnknown: break;
  }
  return "unknown";
}

bool link_is_live(LinkState s) {
  return s == LinkState::kHeld || s == LinkState::kOnTheAir;
}

DeviceView build_device_view(const DeviceSources& from) {
  DeviceView v;
  v.canonical_source = "nothing";
  const double now = from.now_wall > 0.0 ? from.now_wall : wall_now();

  // --- the canonical time --------------------------------------------
  //
  // The median across the live, enabled boxes, computed here rather than taken
  // from BenchStatus for the reason the header gives. Spread is their full
  // min-to-max range: it is the number that says whether the bench agrees with
  // itself, and if it does not then the median is not a time at all.
  std::vector<double> votes;
  if (from.bench != nullptr) {
    for (const DeviceSnapshot& d : from.bench->device) {
      if (box_votes(from.conf, d)) votes.push_back(d.median_offset);
    }
  }
  if (!votes.empty()) {
    v.has_canonical = true;
    v.contributing = static_cast<int>(votes.size());
    v.canonical_offset_s = median_offset(votes);
    const auto lo = std::min_element(votes.begin(), votes.end());
    const auto hi = std::max_element(votes.begin(), votes.end());
    v.canonical_spread_s = *hi - *lo;
    v.canonical_source = "octomancerd";
  } else if (from.cameras != nullptr && from.cameras->bench.has) {
    // Nothing to compute from, but the other daemon has a bench of its own and
    // is still correcting cameras against it. Borrowing it is better than
    // showing nothing, as long as the row says whose bench it is.
    v.has_canonical = true;
    v.contributing = from.cameras->bench.boxes;
    v.canonical_offset_s = from.cameras->bench.offset_s;
    v.canonical_spread_s = from.cameras->bench.spread_s;
    v.canonical_source = "octomancer-sync";
  }

  // --- the boxes -------------------------------------------------------

  if (from.bench != nullptr) {
    for (const DeviceSnapshot& d : from.bench->device) {
      if (!box_is_enabled(from.conf, d.id)) {
        ++v.hidden;
        continue;
      }
      DeviceRow r;
      r.kind = DeviceKind::kTentacle;
      r.id = d.id;
      r.name = d.name.empty() ? d.id : d.name;
      // Nothing ever connects to a Tentacle: the timecode is in the
      // advertisement, so a box is either being heard or it is not, and there
      // is no third state for a link that does not exist.
      r.link = d.live ? LinkState::kOnTheAir : LinkState::kOffTheAir;
      r.has_age = true;
      r.age_s = d.age;
      if (v.has_canonical && d.has_time) {
        r.has_offset = true;
        r.offset_s = d.median_offset - v.canonical_offset_s;
      }
      r.has_rssi = d.rssi != 0;
      r.rssi = d.rssi;
      if (d.has_time) r.timecode = d.display;
      r.resolution = d.resolution;
      r.has_drift = d.has_drift;
      r.drift_ppm = d.drift_ppm;
      r.has_median = d.has_time;
      r.median_offset_s = d.median_offset;
      r.alerting = d.alerting;
      // The one thing worth saying about a box, and it is an instruction
      // rather than a status: only the Tentacle app can re-jam it.
      if (d.alerting) r.note = "not jammed";
      r.contributes = box_votes(from.conf, d);
      v.rows.push_back(r);
    }
  }

  // --- the cameras -----------------------------------------------------

  std::vector<std::string> listed;
  if (from.cameras != nullptr) {
    for (const CameraStatus& c : from.cameras->cameras) {
      listed.push_back(c.id);
      if (!camera_is_enabled(from.conf, c.id)) {
        ++v.hidden;
        continue;
      }
      DeviceRow r;
      r.kind = DeviceKind::kCamera;
      r.id = c.id;
      r.name = c.name.empty() ? c.id : c.name;
      r.link = c.connected  ? LinkState::kHeld
               : c.present  ? LinkState::kOnTheAir
                            : LinkState::kOffTheAir;
      // A held link is a camera we are talking to continuously, so the last
      // advertisement is irrelevant -- it stopped advertising *because* we are
      // connected. Ageing it from that advertisement would show a camera in
      // active use drifting towards "not heard for a minute".
      if (r.link == LinkState::kHeld) {
        r.has_age = true;
        r.age_s = 0.0;
      } else if (c.has_last_seen) {
        r.has_age = true;
        r.age_s = std::max(0.0, now - c.last_seen_wall);
      } else if (from.bench != nullptr && from.bench->camera.reported &&
                 from.bench->camera.seen &&
                 (from.bench->camera.id == c.id || c.id.empty())) {
        // An older octomancer-sync did not carry a timestamp. octomancerd is
        // watching the same camera from the same room, so use its age rather
        // than leaving the column blank.
        r.has_age = true;
        r.age_s = from.bench->camera.age;
      }
      // error_s is already camera-minus-bench, so there is no arithmetic to
      // do -- but it is octomancer-sync's *own* bench, which is not
      // necessarily the canonical time computed above: the two daemons can
      // disagree about which boxes are live, and only this one knows which
      // boxes a person switched off. Close enough to show on one line,
      // not close enough to subtract things from.
      if (v.has_canonical && c.has_error) {
        r.has_offset = true;
        r.offset_s = c.error_s;
      }
      r.has_rssi = c.has_rssi;
      r.rssi = c.rssi;
      r.timecode = c.timecode;
      // A camera reports a rate where a box reports a frame size. They go in
      // the same column because they answer the same question -- what is this
      // device counting in -- and neither is ever both.
      if (c.has_fps) r.resolution = fmt("%dfps", c.fps);
      r.has_drift = c.has_drift;
      r.drift_ppm = c.drift_ppm;
      r.note = camera_note(c);
      v.rows.push_back(r);
    }
  }

  // octomancerd hears cameras too, and says only whether one is on the air.
  // That is worth a row when octomancer-sync is not answering, which is
  // exactly the moment somebody is trying to work out whether the camera or
  // the daemon is the thing that is missing.
  if (from.bench != nullptr && from.bench->camera.reported &&
      from.bench->camera.seen &&
      std::find(listed.begin(), listed.end(), from.bench->camera.id) ==
          listed.end() &&
      !(from.bench->camera.id.empty() && !listed.empty())) {
    const CameraSnapshot& c = from.bench->camera;
    if (!camera_is_enabled(from.conf, c.id)) {
      ++v.hidden;
    } else {
      DeviceRow r;
      r.kind = DeviceKind::kCamera;
      r.id = c.id;
      r.name = c.name.empty() ? c.id : c.name;
      // Silence from a camera only means "off the air" if we also know that
      // nobody is holding its link, and the daemon that would hold it is the
      // one that did not answer. So: heard means on the air, and not heard
      // means we do not know.
      r.link = c.present ? LinkState::kOnTheAir : LinkState::kUnknown;
      r.has_age = true;
      r.age_s = c.age;
      r.has_rssi = c.rssi != 0;
      r.rssi = c.rssi;
      v.rows.push_back(r);
    }
  }

  // Tentacles first, then cameras; within each, what we are hearing before
  // what we are not. Stable so that everything else stays in the order the
  // daemon listed it, which does not jump about between polls.
  std::stable_sort(v.rows.begin(), v.rows.end(),
                   [](const DeviceRow& a, const DeviceRow& b) {
                     if (kind_order(a.kind) != kind_order(b.kind)) {
                       return kind_order(a.kind) < kind_order(b.kind);
                     }
                     return link_is_live(a.link) && !link_is_live(b.link);
                   });
  return v;
}

// --------------------------------------------------------------- rendering

std::string render_devices(const DeviceView& v, bool verbose, bool color) {
  const Style st = style_for(color);
  std::string out;

  if (v.has_canonical) {
    const char* spread_colour = v.canonical_spread_s > 0.100 ? st.yellow : "";
    out += fmt("canonical time  %s vs this Mac,  spread %s%s%s across %d"
               " box%s\n",
               offset_text(v.canonical_offset_s).c_str(), spread_colour,
               offset_text(v.canonical_spread_s).c_str(),
               spread_colour[0] == '\0' ? "" : st.off, v.contributing,
               v.contributing == 1 ? "" : "es");
  } else {
    out += fmt("%sno canonical time -- no enabled box is live, so there is"
               " nothing to measure against%s\n", st.dim, st.off);
  }
  if (verbose) {
    out += fmt("%scanonical source: %s%s\n", st.dim,
               v.canonical_source.c_str(), st.off);
  }
  if (v.hidden > 0) {
    out += fmt("%s%d device%s hidden: disabled in the configuration%s\n",
               st.dim, v.hidden, v.hidden == 1 ? "" : "s", st.off);
  }
  out += "\n";

  if (v.rows.empty()) {
    out += fmt("%sno devices%s\n", st.dim, st.off);
    return out;
  }

  // Every escape sits outside a width specifier, never inside one. Padding a
  // string that already contains an escape pads the escape too, which lines up
  // in one mode and not the other -- and the tests here compare the coloured
  // output against the plain one byte for byte.
  if (verbose) {
    out += fmt("%s%-14s %6s %10s %-11s %5s %-12s %10s %9s %s%s\n", st.dim,
               "DEVICE", "AGE", "OFFSET", "LINK", "RSSI", "TIMECODE", "MEDIAN",
               "DRIFT", "RATE", st.off);
  } else {
    out += fmt("%s%-14s %6s %10s %s%s\n", st.dim, "DEVICE", "AGE", "OFFSET",
               "LINK", st.off);
  }

  for (const DeviceRow& r : v.rows) {
    const char* name_colour = r.alerting ? st.red
                              : link_is_live(r.link) ? ""
                                                     : st.dim;
    const std::string age =
        r.has_age ? format_age(r.age_s) : std::string("--");
    const std::string off =
        r.has_offset ? offset_text(r.offset_s) : std::string("--");
    const char* link_colour = link_is_live(r.link) ? st.green : st.dim;

    // The last column on a line is never padded: trailing spaces are invisible
    // until somebody copies a row out of a terminal, and then they are not.
    out += fmt(verbose ? "%s%-14.14s%s %s%6s%s %s%10s%s %s%-11s%s"
                       : "%s%-14.14s%s %s%6s%s %s%10s%s %s%s%s",
               name_colour, r.name.c_str(), st.off,
               r.has_age ? "" : st.dim, age.c_str(), st.off,
               r.has_offset ? "" : st.dim, off.c_str(), st.off,
               link_colour, link_state_name(r.link), st.off);

    if (verbose) {
      const std::string rssi = r.has_rssi ? fmt("%d", r.rssi) : "--";
      const std::string tc = r.timecode.empty() ? "--" : r.timecode;
      const std::string med =
          r.has_median ? offset_text(r.median_offset_s) : std::string("--");
      const std::string drift =
          r.has_drift ? fmt("%+.1fppm", r.drift_ppm) : "--";
      const std::string rate = r.resolution.empty() ? "--" : r.resolution;
      out += fmt(" %s%5s%s %s%-12.12s%s %s%10s%s %s%9s%s %s%.9s%s",
                 r.has_rssi ? "" : st.dim, rssi.c_str(), st.off,
                 r.timecode.empty() ? st.dim : "", tc.c_str(), st.off,
                 r.has_median ? "" : st.dim, med.c_str(), st.off,
                 r.has_drift ? "" : st.dim, drift.c_str(), st.off,
                 r.resolution.empty() ? st.dim : "", rate.c_str(), st.off);
    }
    out += "\n";
  }

  // Notes go under the table rather than in it. "timecode does not follow the
  // clock" is a sentence, and a column wide enough to hold it would push the
  // verbose row past any terminal worth having; a column too narrow to hold it
  // would truncate the only part that says what to do about it.
  if (verbose) {
    bool any = false;
    for (const DeviceRow& r : v.rows) {
      if (r.note.empty()) continue;
      if (!any) out += "\n";
      any = true;
      const char* colour = r.alerting ? st.red : st.dim;
      out += fmt("%s%s -- %s%s\n", colour, r.name.c_str(), r.note.c_str(),
                 st.off);
    }
  }
  return out;
}

}  // namespace octo
