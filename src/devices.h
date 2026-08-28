// One list of the devices in the room, merged from the two daemons that know
// about them.
//
// Nothing else reconciles those two. octomancerd listens passively and reports
// timecode boxes in registry.h's Snapshot; octomancer-sync connects to cameras
// and reports them in control.h's Status. A person looking at the bench does
// not care which program heard what -- they want one line per device, and they
// want the numbers on those lines to mean the same thing. Deciding what that
// line says is a decision, it involves no radio, and so it lives here on the
// testable side of the seam rather than being written twice: once in
// src/octomancer.cc for the terminal and once in ui/main.mm for the window.
//
// **What an offset means here.** Every offset either daemon reports is
// measured against this Mac's clock, and this Mac's clock is the least
// interesting one in the building -- it is the thing being compared with, not
// the thing anybody is shooting against. So the number on each row is the
// device's distance from the *canonical* time: the median across the live,
// enabled timecode boxes. Because both terms of `d.median_offset -
// canonical_offset` are quoted against this Mac, the Mac cancels out entirely,
// and what is left is box-versus-bench. That is the whole reason the column is
// worth reading: two boxes half a second from a laptop that has not seen an
// NTP server all week are still in perfect agreement with each other, and this
// view says so.
//
// The canonical offset is recomputed here from the enabled boxes rather than
// taken from BenchStatus, because BenchStatus is octomancer-sync's answer to a
// slightly different question -- it does not know which boxes a person has
// switched off, and it may disagree about which are live. The view reports
// which source it ended up using so a caller can say so out loud.
#ifndef OCTO_DEVICES_H
#define OCTO_DEVICES_H

#include <string>
#include <vector>

#include "camconf.h"
#include "control.h"
#include "registry.h"

namespace octo {

enum class DeviceKind { kTentacle, kCamera };

// What we know about a device's radio link right now. The distinction that
// matters is "held" versus "off the air": a camera whose link is held stops
// advertising, so absence of advertisements is success, not failure. Rendering
// those two the same way would have somebody power-cycling a camera that is
// being talked to at that very moment.
enum class LinkState { kUnknown, kHeld, kOnTheAir, kOffTheAir };

const char* link_state_name(LinkState s);

// Whether this link counts as something we are currently hearing from, which
// is what decides whether a row is drawn bright or dim.
bool link_is_live(LinkState s);

// What a device with 'warn if out of sync' set is telling us. Red is a
// measurement we do not like; yellow is the absence of a measurement.
// Ordered quiet to loud, so the worst of a set is simply the largest.
enum class WarnLevel { kNone, kUnsure, kOutOfSync };

const char* warn_level_name(WarnLevel w);

// How far from the canonical time is too far. A jammed bench sits within a
// few milliseconds of itself, and a camera an hour after its last write is
// tens of milliseconds out -- which is normal, expected, and exactly what the
// re-write cycle exists to mop up. A tenth of a second is past both of those,
// and it is about two frames at 24, which is far enough that somebody would
// see it. Anything tighter would go red every time a camera got warm.
constexpr double kWarnOffset = 0.100;

// How long silence is allowed to last before we admit we do not know. Long
// enough that advertisement duty cycling and a weak signal do not trip it: a
// timecode box at -84 dBm can genuinely go three minutes between packets, and
// a light that flickers whenever somebody stands in front of the cart is a
// light nobody reads. Short enough that a device switched off, or carried out
// of the room, is noticed within a setup break rather than in the rushes.
constexpr double kWarnSilence = 300.0;

struct DeviceRow {
  DeviceKind kind = DeviceKind::kTentacle;
  std::string id;
  std::string name;
  // Always true in a view built today, because a disabled device gets counted
  // in `hidden` instead of getting a row. It is here for the Configuration
  // page, which has to list the devices somebody switched off in order to
  // offer switching them back on.
  bool enabled = true;
  LinkState link = LinkState::kUnknown;

  // Against the canonical time, never against this Mac. Unset when there is no
  // canonical time to be against, because a distance from a time that does not
  // exist is not a small number -- it is not a number at all.
  //
  // Also unset for a device we are not currently hearing. Its last reading was
  // taken against a canonical time that has moved on since, so subtracting
  // today's canonical time from it measures mostly the drift that piled up
  // while nobody was listening: a box quiet for two hours reads tens of
  // milliseconds out on that arithmetic alone, and the column would be
  // reporting the length of the silence as though it were a sync error. The
  // last raw reading survives in `median_offset_s`, which is quoted against
  // this Mac and so does not rot the same way.
  bool has_offset = false;
  double offset_s = 0.0;
  // Why the offset is missing, which is not the same question as whether it
  // is. Set when the device did tell us a time and we are declining to quote
  // the distance because we are not hearing it now; clear when it has never
  // said one at all. A short dropout and a device that will not say what time
  // it thinks it is both leave the column blank, and they deserve opposite
  // reactions.
  bool offset_is_stale = false;

  bool has_age = false;   // seconds since we last heard from it
  double age_s = 0.0;

  // Whether somebody asked to be told about this device, and what it is
  // currently telling them. A row that nobody asked about is always kNone, so
  // these two are read together and never separately.
  bool warn = false;
  WarnLevel warn_level = WarnLevel::kNone;

  // --- verbose only, below here ---------------------------------------

  bool has_rssi = false;
  int rssi = 0;
  std::string timecode;    // empty when the device has not said
  std::string resolution;
  bool has_drift = false;
  double drift_ppm = 0.0;
  // The raw median offset against this Mac, which is what `offset_s` was
  // derived from. Worth keeping for the verbose view: it is the number that
  // moves when the Mac's clock is the thing that is wrong.
  bool has_median = false;
  double median_offset_s = 0.0;
  bool alerting = false;
  // The one thing worth saying about this row, or empty. Not a status field:
  // if there is nothing to say, nothing is said.
  std::string note;
  bool contributes = false;  // did this row vote on the canonical time
};

struct DeviceView {
  // Timecode boxes first, then cameras; within each kind, the ones we hear
  // from before the ones we are not. Order within those groups is the order
  // the daemons gave, which is stable across polls.
  std::vector<DeviceRow> rows;

  bool has_canonical = false;
  // The canonical time versus this Mac. Shown once, in the header, because it
  // is a fact about the Mac rather than about any device.
  double canonical_offset_s = 0.0;
  double canonical_spread_s = 0.0;
  int contributing = 0;           // how many boxes voted
  std::string canonical_source;   // "octomancerd", "octomancer-sync", "nothing"
  // Known devices left out of `rows` because they are disabled. Counted rather
  // than dropped silently: "3 devices hidden" is honest, and a bench that
  // quietly lists fewer boxes than are in the room is not.
  int hidden = 0;
  // Enabled timecode boxes we are not hearing. They still get a row, but they
  // did not vote and are not in the spread -- a box that has gone quiet cannot
  // say what time it thinks it is *now*, and the last thing it said is not an
  // answer to that question. Counted for the same reason `hidden` is: so the
  // header can explain why fewer boxes are voting than are on the page.
  int silent = 0;

  // The worst thing any warned device is saying, and how many are saying
  // each. This is what a one-character indicator is made of: `worst_warning`
  // picks the colour, the counts and the rows say what is behind it. Only
  // devices in `rows` are considered -- see build_device_view for why.
  WarnLevel worst_warning = WarnLevel::kNone;
  int warned_out_of_sync = 0;
  int warned_unsure = 0;
};

// Whatever the caller managed to collect. Every pointer may be null: a daemon
// that did not answer is an ordinary Tuesday, not an error, and two missing
// daemons make an empty view rather than a crash.
struct DeviceSources {
  const Snapshot* bench = nullptr;   // null when octomancerd did not answer
  const Status* cameras = nullptr;   // null when octomancer-sync did not answer
  const CamConf* conf = nullptr;     // null means everything is enabled
  double now_wall = 0.0;             // 0 means "call wall_now() yourself"
};

DeviceView build_device_view(const DeviceSources& from);

// The terminal rendering, in the manner of render.cc's box table. `color=false`
// produces exactly the same bytes minus the escape sequences, which is what
// makes it testable.
std::string render_devices(const DeviceView& v, bool verbose, bool color);

}  // namespace octo

#endif  // OCTO_DEVICES_H
