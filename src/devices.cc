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
  // The column headings, and nothing else. They were dim, which is the same
  // ink the table uses for "this number is not to be trusted" -- so the one
  // row on the page that is always true was drawn like the rows that are not.
  // Cyan rather than blue: blue on a dark terminal is where readable colours
  // go to hide.
  const char* head;
  const char* off;
};

Style style_for(bool color) {
  if (color) {
    return {"\033[2m",  "\033[1m", "\033[31m", "\033[33m",
            "\033[32m", "\033[36m", "\033[0m"};
  }
  return {"", "", "", "", "", "", ""};
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

// The same question, asked of one radio's boxes only. A radio's canonical
// time has to be built from what that radio heard and nothing else, or the
// difference between two machines' clocks ends up inside a figure that is
// supposed to be the difference between two timecode boxes.
bool box_votes_for(const CamConf* conf, const DeviceSnapshot& d,
                   const std::string& radio) {
  return d.radio == radio && box_votes(conf, d);
}

// The median, spread and count over one radio's voters. Shared by this
// machine's radio and every dongle, because they deserve exactly the same
// arithmetic -- if they got different arithmetic, the two copies of a box
// could differ for a reason that was ours rather than the room's.
RadioView radio_view(const DeviceSources& from, const std::string& radio) {
  RadioView rv;
  rv.name = radio;
  if (!radio.empty()) {
    // The defaults on the struct describe the local radio, which is the one
    // that needs no describing. Anything else starts as unreachable and
    // clockless until the snapshot says otherwise -- so a daemon too old to
    // send a `radio` line prints "--" and "free" rather than confidently
    // claiming a dongle is local and its boot clock is a time of day.
    rv.way = "none";
    rv.answering = false;
    rv.clock_is_real = false;
  }
  std::vector<double> votes;
  if (from.bench != nullptr) {
    for (const DeviceSnapshot& d : from.bench->device) {
      if (box_votes_for(from.conf, d, radio)) votes.push_back(d.median_offset);
    }
  }
  if (votes.empty()) return rv;
  rv.has_canonical = true;
  rv.contributing = static_cast<int>(votes.size());
  rv.canonical_offset_s = median_offset(votes);
  const auto lo = std::min_element(votes.begin(), votes.end());
  const auto hi = std::max_element(votes.begin(), votes.end());
  rv.canonical_spread_s = *hi - *lo;
  return rv;
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

// The whole warning rule, in the order the questions have to be asked.
//
// Age comes before the reading, and that ordering is the entire point of
// having a yellow at all. A stale offset is not evidence of being in sync --
// it is a measurement of where the device was the last time anybody heard it
// -- and drawing an hour-old reading as though it were current is how
// somebody ends up shooting against a clock that walked off while they were
// not looking. Better to say "we do not know" than to say something
// reassuring that nothing supports.
WarnLevel warn_level_for(const DeviceRow& r) {
  if (!r.warn) return WarnLevel::kNone;
  // A held link is a camera we are talking to continuously, so it is being
  // heard by definition and the last advertisement means nothing. The row
  // already carries age zero for that reason; this guards it a second time so
  // the rule reads correctly on its own.
  if (r.link != LinkState::kHeld && (!r.has_age || r.age_s > kWarnSilence)) {
    return WarnLevel::kUnsure;
  }
  if (r.has_offset) {
    return std::fabs(r.offset_s) > kWarnOffset ? WarnLevel::kOutOfSync
                                               : WarnLevel::kNone;
  }
  // No offset, and the age rule above has already decided the silence is
  // short enough to live with. If the reason is that the device told us a
  // time and we are simply not hearing it this second, that is the dropout we
  // just agreed to tolerate, and lighting it up here would undo the
  // tolerance -- it would mean a box going quiet for a minute is treated
  // exactly like one gone for an hour. Anything else means there is no
  // canonical time, or the device has never said what time it thinks it is,
  // and staying quiet about that would amount to saying it is fine.
  return r.offset_is_stale ? WarnLevel::kNone : WarnLevel::kUnsure;
}

// A label cut to fit, losing the middle rather than the end.
//
// The end is where the information is. A box that has never been named is
// listed by its hardware address, and every box from one manufacturer shares
// the first three bytes -- so cutting from the right turns four different
// boxes into four identical rows of C4:1E:AE:18:A7, which is worse than
// useless: it looks like the table is repeating itself.
//
// Names are cut from the right as before, because a name's beginning is what
// distinguishes it. The rule is therefore about which end carries the
// difference, and identifiers and names disagree about that.
std::string fit_label(const std::string& label, size_t width, bool from_end) {
  if (label.size() <= width) return label;
  if (!from_end) return label.substr(0, width);
  if (width < 5) return label.substr(label.size() - width);
  const size_t keep = width - 2;
  const size_t head = keep / 2;
  return label.substr(0, head) + ".." +
         label.substr(label.size() - (keep - head));
}

// How long ago something happened, preferring a timestamp to a duration.
//
// A snapshot is a photograph: `age` was true at the instant it was taken, and
// every interface here holds one and redraws from it, so an age copied
// straight out is frozen at whatever it was when the daemon answered. A
// device that has gone quiet then reads as though it were still being heard,
// which is the one thing the column exists to rule out.
//
// So the timestamp wins when there is one, and `age` is the fallback -- for a
// device restored from disk, whose last sighting predates this process, and
// for a daemon too old to send the stamp at all.
double age_from(double last_wall, double reported_age, double now) {
  if (last_wall <= 0.0 || now <= 0.0) return reported_age;
  const double age = now - last_wall;
  // A clock stepped backwards, or a roster copied from another machine, can
  // put the last sighting in the future. "Heard in three hours' time" is worse
  // than a slightly stale number.
  return age < 0.0 ? 0.0 : age;
}

// How a radio is reached, as a person reads it. The wire spellings are
// lower-case words chosen for a protocol; these are chosen for a column.
const char* link_way_label(const std::string& way) {
  if (way == "local") return "local";
  if (way == "usb") return "USB";
  if (way == "bluetooth") return "Bluetooth";
  // A radio we know about and are not currently reaching. Not "none", which
  // reads as a property of the radio rather than of our end of the cable.
  return "--";
}

// The marker a warned row carries in the DEVICE column. One character either
// way, and the same character with colour off: the tests compare the coloured
// output against the plain one byte for byte, and somebody piping this into a
// file should not lose the one thing they were watching for.
const char* warn_mark(WarnLevel w) {
  switch (w) {
    case WarnLevel::kOutOfSync: return " !";
    // No mark for "we do not know". The row already says so in every column
    // that could have carried a number -- a device nobody is hearing has no
    // age and no offset -- so the character was a third way of saying the same
    // thing, on the rows least able to spare two characters of name.
    //
    // The colour stays. A camera that is not being synced is the failure this
    // whole program exists to prevent, and it is worth calling out even when
    // the reason is only that nobody can see it. The cost is that piping the
    // output somewhere without colour loses the distinction; "out of sync",
    // which is a measurement rather than the absence of one, keeps its mark
    // for exactly that reason.
    case WarnLevel::kUnsure: break;
    case WarnLevel::kNone: break;
  }
  return "";
}

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

const char* warn_level_name(WarnLevel w) {
  switch (w) {
    case WarnLevel::kOutOfSync: return "out of sync";
    case WarnLevel::kUnsure: return "unsure";
    case WarnLevel::kNone: break;
  }
  return "none";
}

DeviceView build_device_view(const DeviceSources& from) {
  DeviceView v;
  v.canonical_source = "nothing";
  if (from.bench != nullptr) v.radio = from.bench->radio;
  const double now = from.now_wall > 0.0 ? from.now_wall : wall_now();

  // --- the canonical time --------------------------------------------
  //
  // The median across the live, enabled boxes, computed here rather than taken
  // from BenchStatus for the reason the header gives. Spread is their full
  // min-to-max range: it is the number that says whether the bench agrees with
  // itself, and if it does not then the median is not a time at all.
  RadioView here = radio_view(from, std::string());
  here.local = true;
  // Named, not described. "this Mac" is what a page says when it has only one
  // radio and no need to distinguish it; beside a dongle, the useful thing is
  // which machine -- and the VIA column below repeats it on every local row,
  // where a phrase would be four words of furniture.
  here.name = (from.bench != nullptr && !from.bench->host.empty())
                  ? from.bench->host
                  : std::string("this Mac");
  here.way = "local";
  // A local radio is read, not asked, so it has no answer to be waiting for.
  // The thing that goes wrong with it is not silence but refusal, and that is
  // already said in full by DeviceView::radio.
  here.answering = from.bench != nullptr;
  here.clock_is_real =
      from.bench == nullptr ? true : from.bench->wall_is_real;
  if (here.has_canonical) {
    v.has_canonical = true;
    v.contributing = here.contributing;
    v.canonical_offset_s = here.canonical_offset_s;
    v.canonical_spread_s = here.canonical_spread_s;
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

  // This machine's radio is always the first entry, whatever it has managed
  // to hear. A list of radios that omits the one doing the listening reads as
  // a list of exceptions rather than as an inventory.
  v.radios.push_back(here);

  // Then every other radio, each with a canonical time of its own.
  // First-appearance order rather than sorted, so a dongle does not move
  // around the page when a box it can hear goes quiet.
  //
  // Taken from both the rows and the snapshot's own list of links, because
  // those answer different questions: a dongle that is attached and has heard
  // nothing contributes no rows at all, and dropping it here would render it
  // identically to no dongle. Rows first, so a radio that is doing something
  // keeps its place at the top.
  if (from.bench != nullptr) {
    auto known = [&v](const std::string& name) {
      for (const RadioView& rv : v.radios) {
        if (!rv.local && rv.name == name) return true;
      }
      return false;
    };
    for (const DeviceSnapshot& d : from.bench->device) {
      if (d.radio.empty() || known(d.radio)) continue;
      v.radios.push_back(radio_view(from, d.radio));
    }
    for (const RadioLink& l : from.bench->radio_link) {
      if (l.name.empty() || known(l.name)) continue;
      v.radios.push_back(radio_view(from, l.name));
    }
    // How each one is reached, which is a fact about the link rather than
    // about anything it heard, so it arrives separately from the rows.
    for (RadioView& rv : v.radios) {
      if (rv.local) continue;
      for (const RadioLink& l : from.bench->radio_link) {
        if (l.name != rv.name) continue;
        rv.way = l.way;
        rv.answering = l.answering;
        rv.has_age = l.answering;
        rv.age_s = age_from(l.last_wall, l.age, now);
        rv.clock_is_real = l.clock_is_real;
        break;
      }
    }
  }

  // --- the boxes -------------------------------------------------------

  if (from.bench != nullptr) {
    for (const DeviceSnapshot& d : from.bench->device) {
      if (!box_is_enabled(from.conf, d.id)) {
        ++v.hidden;
        continue;
      }
      // Which canonical time this row is quoted against: its own radio's.
      bool row_has_canonical = v.has_canonical;
      double row_canonical = v.canonical_offset_s;
      if (!d.radio.empty()) {
        row_has_canonical = false;
        for (const RadioView& rv : v.radios) {
          if (rv.local || rv.name != d.radio) continue;
          row_has_canonical = rv.has_canonical;
          row_canonical = rv.canonical_offset_s;
          break;
        }
      }
      DeviceRow r;
      r.kind = DeviceKind::kTentacle;
      r.id = d.id;
      r.radio = d.radio;
      r.name = d.name.empty() ? d.id : d.name;
      // Nothing ever connects to a timecode box: the timecode is in the
      // advertisement, so a box is either being heard or it is not, and there
      // is no third state for a link that does not exist.
      r.link = d.live ? LinkState::kOnTheAir : LinkState::kOffTheAir;
      r.has_age = true;
      r.age_s = age_from(d.last_wall, d.age, now);
      // Only while we are hearing it. An old reading minus a current
      // canonical time is not a stale offset, it is a wrong one, and it grows
      // for as long as the box stays quiet. See DeviceRow::has_offset.
      if (row_has_canonical && d.has_time) {
        if (d.live) {
          r.has_offset = true;
          r.offset_s = d.median_offset - row_canonical;
        } else {
          r.offset_is_stale = true;
        }
      }
      if (!d.live) ++v.silent;
      r.has_rssi = d.rssi != 0;
      r.rssi = d.rssi;
      if (d.has_time) r.timecode = d.display;
      r.resolution = d.resolution;
      r.has_drift = d.has_drift;
      r.drift_ppm = d.drift_ppm;
      // A span of zero is not a short wait, it is no measurement at all --
      // a box whose sample window has collapsed to one reading. "~0s" reads
      // as a broken column; a dash reads as an empty one, which is the truth.
      if (!d.has_drift && d.samples > 0 && d.drift_span > 0.0) {
        r.has_drift_span = true;
        r.drift_span_s = d.drift_span;
      }
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
      //
      // Held or on the air only, for the same reason a silent box has no
      // offset: the error from the last cycle is not the camera's error now.
      if (v.has_canonical && c.has_error) {
        if (link_is_live(r.link)) {
          r.has_offset = true;
          r.offset_s = c.error_s;
        } else {
          r.offset_is_stale = true;
        }
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

  // --- cameras that are in the file and nowhere else --------------------
  //
  // Both sources above are records of something being *heard*. So a camera
  // somebody had named, given permission to write to, and asked to be warned
  // about disappeared from this list entirely the moment it stopped
  // advertising -- switched off, carried out of the room, or claimed by
  // another app -- and looked exactly like a camera that had never existed.
  // Nothing said it was missing, and the menu-bar blip stayed grey because
  // there was no row for it to colour.
  //
  // That is this project's recurring failure in another guise: silence and
  // absence rendered identically. A camera in the configuration now always
  // has a row, and warn_level_for turns it yellow on the strength of having
  // no age at all.
  if (from.conf != nullptr) {
    for (const CameraConfig& c : from.conf->cameras()) {
      if (std::find(listed.begin(), listed.end(), c.id) != listed.end()) {
        continue;
      }
      // The block above may have added it from octomancerd's side without it
      // being in `listed`, which is octomancer-sync's list.
      bool already = false;
      for (const DeviceRow& r : v.rows) {
        if (r.kind == DeviceKind::kCamera && r.id == c.id) {
          already = true;
          break;
        }
      }
      if (already) continue;
      if (!camera_is_enabled(from.conf, c.id)) {
        ++v.hidden;
        continue;
      }

      DeviceRow r;
      r.kind = DeviceKind::kCamera;
      r.id = c.id;
      r.name = c.name.empty() ? c.id : c.name;
      // "Off the air" is a claim about the radio, and only octomancer-sync
      // goes looking for cameras. With it not answering, the honest answer is
      // that nobody knows.
      r.link = from.cameras != nullptr ? LinkState::kOffTheAir
                                       : LinkState::kUnknown;
      // Deliberately no age. Nothing in this view has ever heard this camera,
      // so there is no instant to count from, and an age of zero would render
      // as "now" -- the exact opposite of what is true.
      r.note = "in the configuration, not heard";
      v.rows.push_back(r);
    }
  }

  // Timecode boxes first, then cameras; within each, what we hear before
  // what we are not. Stable so that everything else stays in the order the
  // daemon listed it, which does not jump about between polls.
  std::stable_sort(v.rows.begin(), v.rows.end(),
                   [](const DeviceRow& a, const DeviceRow& b) {
                     if (kind_order(a.kind) != kind_order(b.kind)) {
                       return kind_order(a.kind) < kind_order(b.kind);
                     }
                     return link_is_live(a.link) && !link_is_live(b.link);
                   });

  // --- the warnings ----------------------------------------------------
  //
  // Done here, over `rows`, which is to say over the enabled devices only. A
  // device somebody has switched off is one they have said they are not
  // working with today, and it must never light anything up -- otherwise
  // switching a box off would be no relief at all from being told about it,
  // and the only remaining way to stop the light would be to stop looking.
  //
  // With no configuration at all, nothing warns: a view built without one has
  // no way to know what anybody cares about, and guessing would mean deciding
  // on someone's behalf that every device in range is theirs.
  for (DeviceRow& r : v.rows) {
    r.warn = from.conf != nullptr && from.conf->warn_enabled(r.id);
    r.warn_level = warn_level_for(r);
    if (r.warn_level == WarnLevel::kOutOfSync) ++v.warned_out_of_sync;
    if (r.warn_level == WarnLevel::kUnsure) ++v.warned_unsure;
    if (static_cast<int>(r.warn_level) > static_cast<int>(v.worst_warning)) {
      v.worst_warning = r.warn_level;
    }
  }

  // What each radio currently has, counted off the finished rows so that the
  // section and the table can never disagree about how many there are. Live
  // only: a device on the page and off the air is not something a radio is
  // hearing, and counting it would make a room look fuller than it is.
  for (RadioView& rv : v.radios) {
    for (const DeviceRow& r : v.rows) {
      const bool mine = rv.local ? r.radio.empty() : r.radio == rv.name;
      if (!mine || !link_is_live(r.link)) continue;
      if (r.kind == DeviceKind::kCamera) {
        ++rv.live_cameras;
      } else {
        ++rv.live_boxes;
      }
    }
  }
  return v;
}

// --------------------------------------------------------------- rendering

// Why an empty device list is empty, when the radio can answer that. Returns
// "" when the radio is fine or when nothing is known about it -- an empty
// table with a working radio really does mean nothing is on the air, and
// saying anything there would be noise on every quiet bench.
//
// "unknown" is the one worth spelling out at length. It is not a radio
// failure; it is CoreBluetooth never having called back at all, which on macOS
// is what a missing Bluetooth permission looks like. There is no error, no
// prompt and no state, and under launchd there is nobody to prompt.
//
// It used to add "a rebuilt binary loses the approval it had", which is wrong
// and was written from one bad morning rather than from a measurement. The
// daemons embed an Info.plist to give macOS a stable identity for exactly this
// reason, and a rebuild keeps the grant -- checked by watching octomancerd's
// cdhash change across `make install` while the radio kept working.
std::string radio_complaint(const std::string& radio) {
  if (radio.empty() || radio == "poweredOn") return "";
  if (radio == "poweredOff") {
    return "Bluetooth is switched off on this Mac.";
  }
  if (radio == "unauthorized") {
    return "the daemon is not allowed to use Bluetooth -- approve it in"
           " System Settings > Privacy & Security > Bluetooth";
  }
  if (radio == "unsupported") {
    return "this Mac reports no Bluetooth Low Energy radio.";
  }
  if (radio == "resetting") {
    return "the Bluetooth radio is resetting; this usually clears itself.";
  }
  if (radio == "unknown") {
    return "the radio has never reported a state, which on macOS is what a"
           " missing Bluetooth permission looks like -- there is no prompt and"
           " no error. Approve the daemon in System Settings > Privacy &"
           " Security > Bluetooth; running it once from a terminal usually"
           " raises the prompt.";
  }
  return "the radio reports \"" + radio + "\".";
}

std::string render_devices(const DeviceView& v, bool verbose, bool color,
                           bool always_radios) {
  const Style st = style_for(color);
  std::string out;

  // What goes above the table, which in the ordinary case is nothing.
  //
  // The canonical time is the axis every OFFSET below is measured against, and
  // stating it on every run is repeating the same true thing to somebody who
  // came to read the table. It moves to --verbose.
  //
  // Its *absence* does not move, and the asymmetry is the point: a column of
  // dashes with nothing above it is a table that looks broken, so the line
  // explaining that there is nothing to measure against is printed whether or
  // not anybody asked for detail. Saying why something is missing is not
  // verbosity.
  // Only when a second radio has contributed rows. The ordinary table is the
  // one almost everybody sees, and it should not grow a column of blanks for
  // the sake of a case that is not theirs.
  // On the rows, not on the radio list. A dongle that is attached and has
  // heard nothing belongs in the RADIO section -- that is what says it is
  // there -- but it must not add a column of blanks to a table where every
  // row came from this machine.
  bool show_via = false;
  for (const DeviceRow& r : v.rows) {
    if (!r.radio.empty()) { show_via = true; break; }
  }
  // What a row with no radio of its own is labelled with. Blank used to be the
  // answer -- absence meaning "this machine" everywhere else -- but the column
  // only exists when there is more than one radio, and in that situation an
  // empty cell is the one thing on the page that has to be inferred. Every row
  // says where it came from, or the column is not there at all.
  std::string local_name = "this Mac";
  for (const RadioView& rv : v.radios) {
    if (rv.local) { local_name = rv.name; break; }
  }

  // What is wrong with *this Mac's* radio, when we are hearing nothing on it.
  //
  // Both halves matter. It used to be printed only when the *table* came out
  // empty, which was sound until a dongle could contribute rows: a Mac whose
  // radio has been refused then produces a page full of somebody else's
  // boxes, and the one line explaining why none of them are ours went quiet
  // exactly when it became most useful. Found the hard way on the install
  // that lost the grant, after a long detour -- the daemon had been saying it
  // in its .err file the whole time.
  //
  // And it stays conditional on hearing nothing, because a radio that is
  // delivering advertisements is working whatever it last reported about
  // itself. Complaining over a table of live boxes would be the page arguing
  // with itself.
  int heard_here = 0;
  for (const DeviceRow& r : v.rows) {
    if (r.radio.empty() && link_is_live(r.link)) ++heard_here;
  }
  const std::string complaint =
      heard_here == 0 ? radio_complaint(v.radio) : std::string();

  std::string head;
  if (!complaint.empty()) {
    head += fmt("%s%s%s\n", st.dim, complaint.c_str(), st.off);
  }
  if (!v.has_canonical) {
    head += fmt("%sno canonical time -- no enabled timecode box is live, so"
                " there is nothing to measure against%s\n", st.dim, st.off);
  }
  if (verbose) {
    head += fmt("%scanonical source: %s%s\n", st.dim,
                v.canonical_source.c_str(), st.off);
  }
  if (verbose && v.has_canonical && v.silent > 0) {
    // This exists to explain the count in the line above -- why fewer boxes
    // are voting than are on the page -- so it belongs wherever that line is.
    head += fmt("%s%d timecode box%s off the air: listed below, but not voting"
                " on the canonical time and not in the spread%s\n",
                st.dim, v.silent, v.silent == 1 ? "" : "es", st.off);
  }
  if (v.hidden > 0) {
    // Kept out of --verbose for the same reason as the missing canonical time:
    // rows are absent from the table and nothing else on the page says why.
    head += fmt("%s%d device%s hidden: disabled in the configuration%s\n",
                st.dim, v.hidden, v.hidden == 1 ? "" : "s", st.off);
  }
  if (!head.empty()) out += head + "\n";

  // --- the radios ------------------------------------------------------
  //
  // Which radios there are, how each is reached, and what each makes of the
  // room. This replaced four lines of prose that said the same thing badly:
  // the question it exists to answer is "am I actually hearing this through
  // the dongle", and a sentence buried above a table is not where somebody
  // looks for that.
  //
  // No column for a radio's own clock, deliberately. Each radio has a view of
  // the mesh time and syncs against that; the host clock is never used for
  // anything, so its distance from the mesh is a number with no consequence --
  // and on a dongle running from its boot counter it is not even a duration
  // anybody could act on. SPREAD is the figure that survives: it is a
  // difference, so whatever origin a radio invented cancels out of it exactly,
  // and it is directly comparable between two radios that agree on nothing
  // else.
  // Never a heading over nothing: build_device_view always supplies this
  // machine's radio, but a view assembled by hand need not have.
  if (!v.radios.empty() && (always_radios || verbose || v.radios.size() > 1)) {
  // Fourteen, the same width the DEVICE column gets, because a hostname is
  // about as long as a device name and this one held "Palmers-Mini" cut to
  // "Palmers-Mi" -- which reads as a machine nobody has heard of rather than
  // as a truncation.
  out += fmt("%s%-18s %-14s %6s %10s %8s %7s%s\n", st.head, "RADIO", "LINK",
             "AGE", "SPREAD", "TIMECODE", "CAMERAS", st.off);
  for (const RadioView& rv : v.radios) {
    const char* live = rv.answering ? "" : st.dim;
    const std::string age =
        rv.has_age ? format_age(rv.age_s) : std::string("--");
    const std::string spread = rv.has_canonical
                                   ? offset_text(rv.canonical_spread_s)
                                   : std::string("--");
    const char* spread_colour =
        rv.has_canonical && rv.canonical_spread_s > kWarnOffset ? st.yellow
                                                                : live;
    out += fmt("%s%-18s%s %s%-14s%s %s%6s%s %s%10s%s %s%8d%s %s%7d%s\n",
               live, fit_label(rv.name, 18, false).c_str(), st.off,
               live, link_way_label(rv.way), st.off,
               live, age.c_str(), st.off,
               spread_colour, spread.c_str(), st.off,
               live, rv.live_boxes, st.off,
               live, rv.live_cameras, st.off);
  }
  out += "\n";
  }

  if (v.rows.empty()) {
    // The reason, if the radio is it, is already in the header above. Without
    // it the output is identical whether the room is empty, Bluetooth is
    // switched off, or macOS is refusing the daemon the radio -- three
    // problems with three different answers, none of them "check the
    // batteries".
    out += fmt("%sno devices%s\n", st.dim, st.off);
    return out;
  }

  // Every escape sits outside a width specifier, never inside one. Padding a
  // string that already contains an escape pads the escape too, which lines up
  // in one mode and not the other -- and the tests here compare the coloured
  // output against the plain one byte for byte.
  // The brief table is exactly the first five columns of the verbose one, so
  // the two share a prefix rather than each spelling out its own. Two tables
  // that can drift apart are two tables somebody has to learn separately, and
  // the whole point of one renderer is that there is only ever one to learn.
  // Column for column with the RADIO section above, because the two are read
  // together and most of the pairs mean the same kind of thing: RADIO/DEVICE
  // is what it is called, LINK/VIA is where it comes from, AGE is AGE, and
  // SPREAD/OFFSET are both a distance in milliseconds. Lining them up costs
  // nothing and turns two tables into one shape.
  out += fmt("%s%-18s%s", st.head, "DEVICE", st.off);
  if (show_via) out += fmt("%s %-14s%s", st.head, "VIA", st.off);
  out += fmt("%s %6s %10s %8s%s", st.head, "AGE", "OFFSET", "RSSI", st.off);
  if (verbose) {
    out += fmt("%s %-15s %10s %9s %s%s", st.head, "TIMECODE", "MEDIAN", "DRIFT",
               "RATE", st.off);
  }
  out += "\n";

  for (const DeviceRow& r : v.rows) {
    // A warning outranks the alert colour and the dimming, because it is the
    // one thing on this row somebody explicitly asked to be shown.
    const char* name_colour =
        r.warn_level == WarnLevel::kOutOfSync ? st.red
        : r.warn_level == WarnLevel::kUnsure  ? st.yellow
        : r.alerting                          ? st.red
        : link_is_live(r.link)                ? ""
                                              : st.dim;
    // The marker rides inside the DEVICE column rather than in one of its
    // own, so a table that already fills a narrow terminal does not grow two
    // characters wider for the sake of a flag most rows do not carry. It
    // costs a warned row two characters of name, which is the cheaper of the
    // two prices.
    // A row showing an identifier rather than a name is the case that needs
    // the middle taken out; see fit_label.
    // Eighteen, which is what a Bluetooth hardware address needs -- a device
    // nobody has named is listed by its address, and cutting the middle out of
    // one when the column could simply have held it was making the common case
    // pay for the long one. A CoreBluetooth UUID is still far too long and
    // still gets elided; nothing short of forty characters would hold one.
    const bool is_id = r.name == r.id;
    std::string label = fit_label(r.name, 18, is_id);
    const char* mark = warn_mark(r.warn_level);
    if (mark[0] != '\0') {
      label = fit_label(r.name, 16, is_id);
      label += mark;
    }
    // There is no LINK column any more: whether a device is being heard is
    // already in the age and in whether the row is drawn dim, and a column
    // that repeats what the whole row is already saying is a column nobody
    // reads.
    //
    // One state does not survive that reasoning, and it keeps a word here.
    // "Held" means a camera we are talking to continuously, which stops
    // advertising *because* we are connected -- so its age is not a
    // measurement at all, and rendering it as "0s" would be indistinguishable
    // from a camera we happen to have heard from this instant. Those want
    // opposite reactions: one is working, and the other is about to be
    // power-cycled by somebody who read the table.
    const std::string age = r.link == LinkState::kHeld ? std::string("held")
                            : r.has_age ? format_age(r.age_s)
                                        : std::string("--");
    const char* age_colour = r.link == LinkState::kHeld ? st.green
                             : r.has_age && link_is_live(r.link) ? ""
                                                                 : st.dim;
    const std::string off =
        r.has_offset ? offset_text(r.offset_s) : std::string("--");

    // A row nobody is hearing is drawn dim all the way across, and that is one
    // rule rather than a column-by-column decision. Every number on such a row
    // is a memory: the age is how long ago, the signal is how loud it was
    // then, the timecode is what it said at the time. Drawing half of them as
    // though they were current and half as though they were not would read as
    // a bug in the table, and drawing all of them bright reads as a device
    // that is fine.
    //
    // `live` is a prefix, not a replacement, so a column with nothing in it is
    // still dim on a row that is otherwise being heard.
    const char* live = link_is_live(r.link) ? "" : st.dim;

    // Signal is in both views. It is the column that answers "why is this one
    // not being heard" without anybody having to go and look at the box, and
    // that question comes up far too often for the answer to live behind a
    // flag. Right-justified, which pads on the left: a trailing space is
    // invisible until somebody copies a row out of a terminal, and then it is
    // not.
    const std::string rssi = r.has_rssi ? fmt("%d", r.rssi) : "--";
    out += fmt("%s%-18.18s%s", name_colour, label.c_str(), st.off);
    if (show_via) {
      // Every row says which radio heard it, our own included. The column
      // only exists when there is more than one radio, and in that situation a
      // blank cell is the one thing on the page a reader has to infer.
      out += fmt("%s %-14.14s%s", st.dim,
                 r.radio.empty() ? local_name.c_str() : r.radio.c_str(),
                 st.off);
    }
    out += fmt(" %s%6s%s %s%10s%s %s%8s%s",
               age_colour, age.c_str(), st.off,
               r.has_offset ? "" : st.dim, off.c_str(), st.off,
               r.has_rssi && *live == '\0' ? "" : st.dim, rssi.c_str(),
               st.off);

    if (verbose) {
      const std::string tc = r.timecode.empty() ? "--" : r.timecode;
      const std::string med =
          r.has_median ? offset_text(r.median_offset_s) : std::string("--");
      // The tilde is there so nobody reads the span as a drift figure. It
      // says how long the box has been watched, which is the reason the
      // column is empty rather than a measurement of anything.
      const std::string drift =
          r.has_drift       ? fmt("%+.1fppm", r.drift_ppm)
          : r.has_drift_span ? "~" + format_age(r.drift_span_s)
                             : "--";
      const std::string rate = r.resolution.empty() ? "--" : r.resolution;
      out += fmt(" %s%-15.15s%s %s%10s%s %s%9s%s %s%.11s%s",
                 r.timecode.empty() ? st.dim : live, tc.c_str(), st.off,
                 r.has_median ? live : st.dim, med.c_str(), st.off,
                 r.has_drift ? live : st.dim, drift.c_str(), st.off,
                 r.resolution.empty() ? st.dim : live, rate.c_str(), st.off);
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

  // Under the table, because the marker in the column says which row and this
  // says what it means. Names rather than a count: "one device out of sync"
  // only sends somebody looking, and by then the table has already told them.
  //
  // Only the red one without --verbose. Red is an alarm and an alarm that does
  // not say what it is about is not much of one. Yellow is "we do not know
  // where this is", which is a thing the AGE column has already said in
  // numbers on the row itself -- the sentence underneath is a second telling,
  // and a second telling on every run is what makes people stop reading the
  // first.
  if (v.warned_out_of_sync > 0 || (verbose && v.warned_unsure > 0)) {
    out += "\n";
    for (int pass = 0; pass < 2; ++pass) {
      const bool red = pass == 0;
      if (!red && !verbose) continue;
      const WarnLevel want =
          red ? WarnLevel::kOutOfSync : WarnLevel::kUnsure;
      std::string names;
      for (const DeviceRow& r : v.rows) {
        if (r.warn_level != want) continue;
        if (!names.empty()) names += ", ";
        names += r.name;
      }
      if (names.empty()) continue;
      out += fmt("%s%s %s: %s%s\n", red ? st.red : st.yellow,
                 red ? "!" : "?",
                 red ? "out of sync with the bench"
                     : "not heard from recently enough to say",
                 names.c_str(), st.off);
    }
  }
  return out;
}

}  // namespace octo
