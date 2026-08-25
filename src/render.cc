#include "render.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "timeutil.h"

namespace octo {

namespace {

struct Style {
  const char* dim;
  const char* bold;
  const char* red;
  const char* yellow;
  const char* green;
  const char* off;
};

Style style_for(bool color) {
  if (color) return {"\033[2m", "\033[1m", "\033[31m", "\033[33m", "\033[32m", "\033[0m"};
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

}  // namespace

std::string render_human(const Snapshot& s, bool color) {
  const Style st = style_for(color);
  std::string out;

  const char* radio_colour = s.radio == "poweredOn" ? st.green : st.red;
  out += fmt("%soctomancer%s  %d box%s, %d live  ", st.bold, st.off, s.devices,
             s.devices == 1 ? "" : "es", s.live);
  out += fmt("radio %s%s%s  up %s  %lld adverts\n", radio_colour,
             s.radio.c_str(), st.off, format_age(s.uptime).c_str(),
             static_cast<long long>(s.adverts_total));

  if (s.radio == "unauthorized") {
    out += fmt("%s  Bluetooth is not permitted for this program. Approve it in\n"
               "  System Settings > Privacy & Security > Bluetooth.%s\n",
               st.red, st.off);
  } else if (s.radio == "poweredOff") {
    out += fmt("%s  Bluetooth is switched off.%s\n", st.red, st.off);
  }

  if (s.has_bench) {
    // Spread is the number that says whether the bench agrees with itself. If
    // the boxes disagree, the median they vote on is not a time at all.
    const char* spread_colour = s.bench_spread > 0.100 ? st.yellow : st.off;
    out += fmt("bench %s vs this Mac,  spread %s%s%s across %d live box%s\n",
               offset_text(s.bench_offset).c_str(), spread_colour,
               offset_text(s.bench_spread).c_str(), st.off, s.live,
               s.live == 1 ? "" : "es");
  } else if (s.devices > 0) {
    out += fmt("%sno box heard from recently -- nothing current to compare%s\n",
               st.dim, st.off);
  }
  if (s.camera.reported && !s.camera.seen) {
    out += fmt("%sno Blackmagic camera heard yet -- switch it on, or enable"
               " Bluetooth in its setup menu%s\n", st.dim, st.off);
  } else if (s.camera.seen) {
    // The camera is not a Tentacle and gets no row in the table below: nothing
    // about its clock is visible from an advertisement. All that is known is
    // whether it is on the air, which is exactly what decides whether
    // octomancer-sync has anything to do.
    const char* cam_colour = s.camera.present ? st.green : st.dim;
    out += fmt("camera %s%s%s%s%s  %s for %s,  %lld session%s\n", cam_colour,
               s.camera.present ? "on the air" : "off the air", st.off,
               s.camera.name.empty() ? "" : " -- ", s.camera.name.c_str(),
               s.camera.present ? "up" : "quiet",
               format_age(s.camera.since).c_str(),
               static_cast<long long>(s.camera.sessions),
               s.camera.sessions == 1 ? "" : "s");
  }
  if (s.clock_steps > 0) {
    out += fmt("%sthis Mac's clock stepped %lld time%s since start; drift"
               " history was reset each time%s\n",
               st.dim, static_cast<long long>(s.clock_steps),
               s.clock_steps == 1 ? "" : "s", st.off);
  }
  out += "\n";

  if (s.device.empty()) {
    out += fmt("%sno Tentacle boxes seen yet%s\n", st.dim, st.off);
    return out;
  }

  out += fmt("%s%-14s %6s %5s  %-16s %10s %10s %11s  %s%s\n", st.dim, "BOX",
             "AGE", "RSSI", "TIMECODE", "OFFSET", "MEDIAN", "DRIFT",
             "RESOLUTION", st.off);

  for (const DeviceSnapshot& d : s.device) {
    const char* row = d.alerting ? st.red : (d.live ? "" : st.dim);
    std::string drift = fmt("%s--%s", st.dim, st.off);
    if (d.has_drift) {
      drift = fmt("%+.1fppm", d.drift_ppm);
    } else if (d.samples > 0) {
      // Not a drift figure: how long this box has been watched so far. The
      // tilde is there so nobody reads it as a measurement.
      drift = fmt("%s~%s%s", st.dim, format_age(d.drift_span).c_str(), st.off);
    }

    out += fmt("%s%-14.14s %6s %5d  %-16s %10s %10s %11s  %s%s\n", row,
               d.name.c_str(), format_age(d.age).c_str(), d.rssi,
               d.has_time ? d.display.c_str() : "--",
               d.has_time ? offset_text(d.offset).c_str() : "--",
               d.has_time ? offset_text(d.median_offset).c_str() : "--",
               drift.c_str(), d.resolution.c_str(), st.off);
  }

  if (s.alerting > 0) {
    out += "\n";
    for (const DeviceSnapshot& d : s.device) {
      if (!d.alerting) continue;
      out += fmt("%s!! %s is %s off this Mac -- re-jam it in the Tentacle"
                 " app%s\n",
                 st.red, d.name.c_str(), offset_text(d.median_offset).c_str(),
                 st.off);
    }
  }

  // The drift column is empty for a long time, and the honest reason is worth
  // stating rather than leaving someone to wonder if it is broken.
  bool any_drift = false;
  for (const DeviceSnapshot& d : s.device) any_drift = any_drift || d.has_drift;
  if (!any_drift && s.live > 0) {
    out += fmt("\n%sDrift is parts per million: measuring it needs a long lever"
               " arm, not more\nsamples. The column fills in once a box has"
               " been watched long enough.%s\n",
               st.dim, st.off);
  }
  return out;
}

}  // namespace octo
