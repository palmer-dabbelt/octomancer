// octomancer-sync -- keep a Blackmagic camera's clock on Tentacle time.
//
// Runs until interrupted. Each cycle it works out how far this Mac is from the
// Tentacle bench, connects to the camera, and corrects its clock if that is
// both needed and allowed. The decisions are all in camsync.h, which has no
// radio in it and is tested; this file is the plumbing around them.
//
// This is a separate binary from octomancerd on purpose. octomancerd is
// strictly passive -- it never connects to a device and never writes to one --
// so it can be left running under launchd without any chance of disturbing a
// recording. Setting a clock is an action, and an action belongs in a program
// somebody chose to run.
//
// The Tentacle offset comes from octomancerd when it is running, because it
// already keeps an hour of history per box and takes proper medians across it.
// Failing that, this listens for itself.
#include <getopt.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "bmd.h"
#include "camera.h"
#include "camconf.h"
#include "camdb.h"
#include "proclock.h"
#include "camsync.h"
#include "client.h"
#include "control.h"
#include "jsonlog.h"
#include "registry.h"
#include "radio.h"
#include "scanner.h"
#include "server.h"
#include "timeutil.h"

namespace {

volatile sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

enum class Mode { kSync, kScanOnly, kWatch, kPacket, kRtcTest, kPoke };

enum class Source { kTentacle, kMac };

struct Options {
  octo::SyncOptions sync;
  Mode mode = Mode::kSync;
  Source source = Source::kTentacle;
  std::string camera;  // name hint or BLE identifier
  std::string socket_path = octo::default_socket_path();
  std::string log_path = "octomancer-sync.jsonl";
  std::string console_path;
  octo::Rotation rotation;
  // How often to ask octomancerd whether the camera is on the air. This is a
  // socket read, not a scan: it costs nothing and it is what turns "the camera
  // was switched on" into something noticed in seconds rather than at the next
  // poll.
  double presence_poll = 5.0;
  double watch_seconds = 20.0;
  // Hand-written packets for --poke, in the order given.
  std::vector<std::string> pokes;
  double poke_watch = 4.0;
  // Where the per-camera database lives. Empty disables it.
  std::string camdb_path = octo::default_camera_db_path();
  // Whether a path was named on the command line. The look-but-do-not-touch
  // modes skip the default database so a hand-run probe cannot collide with a
  // running daemon, but an explicit --camera-db means the caller wants it.
  bool camdb_explicit = false;
  octo::CamDbOptions camdb;
  // Whether --rtc-bias was given. An explicit bias on the command line beats a
  // learned one, or a user debugging a body could never override what the
  // database had convinced itself of.
  bool has_rtc_bias = false;
  bool once = false;
  bool show_all = false;
  bool use_daemon = true;

  // The control socket this daemon answers on. Separate from socket_path,
  // which is octomancerd's and is read, not served.
  std::string control_path = octo::default_control_socket_path();
  bool serve_control = true;
  std::string lock_path = octo::default_lock_path("octomancer-sync");

  // Read, never written, by this program. See camconf.h.
  std::string camconf_path = octo::default_camera_config_path();
};

// ------------------------------------------------------------------ output

void say(const char* f, ...) __attribute__((format(printf, 1, 2)));

void say(const char* f, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);

  const double wall = octo::wall_now();
  const time_t secs = static_cast<time_t>(wall);
  struct tm tm_local;
  ::localtime_r(&secs, &tm_local);
  char stamp[16];
  std::strftime(stamp, sizeof stamp, "%H:%M:%S", &tm_local);
  std::printf("%s  %s\n", stamp, buf);
  std::fflush(stdout);
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));

std::string fmt(const char* f, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

// Box names are user-set and arrive from the air, so they are assumed hostile:
// a name with a quote or a backslash in it must not be able to produce a log
// line that parses as something else. The escaping itself lives in jsonlog,
// so the camera database and the JSONL cannot disagree about it.
std::string json_escape(const std::string& in) { return octo::json_escape(in); }

// A JSONL record built up field by field, so a cycle that fails halfway still
// logs everything it had learned before it failed.
class Record {
 public:
  void add(const std::string& key, const std::string& json_value) {
    if (!fields_.empty()) fields_ += ',';
    fields_ += '"' + key + "\":" + json_value;
  }
  void str(const std::string& key, const std::string& value) {
    add(key, '"' + json_escape(value) + '"');
  }
  void num(const std::string& key, double value, int digits = 4) {
    add(key, fmt("%.*f", digits, value));
  }
  void integer(const std::string& key, long long value) {
    add(key, fmt("%lld", value));
  }
  void boolean(const std::string& key, bool value) {
    add(key, value ? "true" : "false");
  }
  bool has_action() const { return has_action_; }
  void action(const std::string& value) {
    if (has_action_) return;
    has_action_ = true;
    str("action", value);
  }
  const std::string& fields() const { return fields_; }

 private:
  std::string fields_;
  bool has_action_ = false;
};

// ------------------------------------------------------------ Tentacle side

struct Bench {
  bool ok = false;
  double offset = 0.0;
  double spread = 0.0;
  int boxes = 0;
  std::string source;
  std::string boxes_json;
};

std::string boxes_to_json(const std::vector<octo::DeviceSnapshot>& devices) {
  std::string out = "{";
  bool first = true;
  for (const octo::DeviceSnapshot& d : devices) {
    if (!d.live || !d.has_time) continue;
    if (!first) out += ',';
    first = false;
    out += '"' + json_escape(d.name.empty() ? d.id : d.name) + "\":";
    out += fmt("{\"offset_s\":%.4f,\"adverts\":%d,\"rssi\":%d,"
               "\"resolution\":\"%s\"}",
               d.median_offset, d.samples, d.rssi,
               json_escape(d.resolution).c_str());
  }
  out += '}';
  return out;
}

// Listen for ourselves, when octomancerd is not running. This is the same
// decoder and the same median arithmetic the daemon uses -- it is just given a
// few seconds of history instead of an hour, which is why the daemon is
// preferred when it is there.
Bench listen_for_bench(const Options& opt) {
  Bench bench;
  octo::Policy policy;
  policy.window = std::max(opt.sync.listen * 2.0, 30.0);
  octo::Registry registry(policy, octo::mono_now());

  auto scanner = octo::make_ble_scanner(
      [&registry](const octo::Advert& a) {
        registry.observe(a.id, a.name, a.rssi, a.data.data(), a.data.size(),
                         a.mono, a.wall);
      },
      [&registry](const std::string& state) { registry.set_radio(state); });
  if (!scanner) return bench;

  std::string err;
  if (!scanner->start(&err)) return bench;
  std::this_thread::sleep_for(std::chrono::duration<double>(opt.sync.listen));
  scanner->stop();

  const octo::Snapshot snap = registry.snapshot(octo::mono_now(), octo::wall_now());
  if (!snap.has_bench) return bench;
  bench.ok = true;
  bench.offset = snap.bench_offset;
  bench.spread = snap.bench_spread;
  bench.boxes = snap.live;
  bench.source = "scan";
  bench.boxes_json = boxes_to_json(snap.device);
  return bench;
}

Bench read_bench(const Options& opt) {
  if (opt.use_daemon) {
    octo::Snapshot snap;
    std::string err;
    if (octo::fetch(opt.socket_path, &snap, &err) && snap.has_bench) {
      Bench bench;
      bench.ok = true;
      bench.offset = snap.bench_offset;
      bench.spread = snap.bench_spread;
      bench.boxes = snap.live;
      bench.source = "octomancerd";
      bench.boxes_json = boxes_to_json(snap.device);
      return bench;
    }
  }
  return listen_for_bench(opt);
}

// What octomancerd can see of the camera without connecting to it.
//
// `known` is false when there is nothing to ask -- no daemon running, or one
// too old to watch for cameras -- and then this program falls back to finding
// out the expensive way, with a scan.
struct Presence {
  bool known = false;
  bool present = false;
  uint64_t sessions = 0;
  double since = 0.0;
  std::string id;
  std::string name;
};

Presence read_presence(const Options& opt) {
  Presence p;
  if (!opt.use_daemon) return p;
  octo::Snapshot snap;
  std::string err;
  if (!octo::fetch(opt.socket_path, &snap, &err)) return p;
  if (!snap.camera.reported) return p;
  p.known = true;
  p.present = snap.camera.present;
  p.sessions = snap.camera.sessions;
  p.since = snap.camera.since;
  p.id = snap.camera.id;
  p.name = snap.camera.name;
  return p;
}

// ------------------------------------------------------------ camera side

// Take up where the last run left off for this body.
//
// The two learned figures cost real time to acquire -- a bias adjustment costs
// one of the rationed writes, and the lead needs several before its median
// means anything -- so a daemon restart used to throw away a night's work. The
// drift estimate is deliberately *not* seeded: it is a statement about a clock
// that has since been switched off and on again.
void seed_from_db(octo::CamDb* db, const Options& opt, octo::SyncState* state,
                  const std::string& id, const std::string& name) {
  if (db == nullptr || !db->enabled()) return;

  std::string err;
  const octo::CameraRecord* rec = db->find(id);
  const bool known = rec != nullptr;
  if (!db->note_seen(id, name, 0, !known, &err)) {
    say("  camera database: %s", err.c_str());
  }
  if (!known) {
    say("  first time seeing this body -- learning its RTC bias and send lead"
        " from scratch");
    return;
  }

  if (rec->has_bias && !opt.has_rtc_bias && rec->bias != state->rtc_bias) {
    say("  recalled: RTC bias %+ds for this body (%llu writes on record)",
        rec->bias, static_cast<unsigned long long>(rec->writes));
    state->rtc_bias = rec->bias;
  }
  if (rec->has_lead && opt.sync.adapt_lead) {
    state->lead.has = true;
    state->lead.lead_s = rec->lead_s;
    state->lead.samples =
        static_cast<int>(rec->recent_apply_delays(opt.sync.lead_window).size());
    say("  recalled: send lead %.0fms for this body (median of %d writes)",
        rec->lead_s * 1000.0, state->lead.samples);
  }
}

// Connect, scanning only when we have to. Once the identifier is known,
// CoreBluetooth can usually connect straight to it; scanning for 20 seconds
// every cycle would otherwise dominate the poll interval and keep the radio
// busy for no reason.
bool connect_camera(octo::CameraLink* link, octo::SyncState* state,
                    const Options& opt, octo::CamDb* db,
                    const std::string& want, std::string* picked_name) {
  // `want` names the camera this cycle is for: the configured one for a
  // scheduled cycle, or whatever a client asked for. An id that is already
  // what we are bound to is the fast path; anything else has to be looked for,
  // because the only way to tell a name from a body is to see it advertise.
  const bool bound_is_wanted =
      want.empty() || (!state->camera_id.empty() && want == state->camera_id);
  if (!state->camera_id.empty() && bound_is_wanted) {
    std::string err;
    if (link->connect(state->camera_id, opt.sync.connect_timeout, &err)) {
      return true;
    }
    say("direct connect to %s failed (%s) -- rescanning",
        state->camera_id.substr(0, 8).c_str(), err.c_str());
  }

  say("scanning %.0fs for Blackmagic cameras...", opt.sync.scan_timeout);
  const octo::ScanResult found = link->scan(opt.sync.scan_timeout, want, false);

  const octo::CameraDevice* pick = nullptr;
  for (const octo::CameraDevice& dev : found.cameras) {
    if (want.empty()) {
      pick = &dev;
      break;
    }
    std::string name = dev.name, hint = want;
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    std::transform(hint.begin(), hint.end(), hint.begin(), ::tolower);
    std::string id = dev.id;
    std::transform(id.begin(), id.end(), id.begin(), ::tolower);
    if (name.find(hint) != std::string::npos || id == hint) {
      pick = &dev;
      break;
    }
  }

  if (pick == nullptr) {
    std::printf("  no Blackmagic cameras found.\n");
    if (found.total == 0) {
      std::printf("  * no LE devices at all -- Bluetooth may still be blocked"
                  " for this process\n");
    } else {
      std::printf("  * saw %d other LE devices, so the radio and permissions"
                  " are fine\n", found.total);
    }
    std::printf("  * enable Bluetooth in the camera's setup menu\n");
    std::printf("  * a camera already connected to another app won't"
                " advertise\n");
    std::printf("  * an already-bonded camera may need to be un-paired from"
                " macOS first\n");
    std::fflush(stdout);
    return false;
  }

  std::printf("  %-38s %-22s rssi=%-5d (%s)\n", pick->id.c_str(),
              pick->name.empty() ? "(no name)" : pick->name.c_str(), pick->rssi,
              pick->by_service_uuid ? "service uuid" : "name guess");
  std::fflush(stdout);

  const std::string id = pick->id;
  if (picked_name != nullptr) *picked_name = pick->name;
  std::string err;
  if (!link->connect(id, opt.sync.connect_timeout, &err)) {
    say("connect failed: %s", err.c_str());
    return false;
  }
  const bool new_body = state->camera_id != id;
  if (!state->camera_id.empty() && new_body) {
    // A different body. Nothing measured about the last one's clock says
    // anything about this one's.
    octo::forget_drift(state);
  }
  state->camera_id = id;
  if (new_body) seed_from_db(db, opt, state, id, pick->name);
  return true;
}

// Write the RTC so the value lands on a second boundary. Returns the latency
// of the GATT write itself, which is the part that cannot be compensated for.
bool aligned_write(octo::CameraLink* link, const Options& opt, double offset,
                   int bias, double lead, octo::bmd::Civil* wrote,
                   double* latency, std::string* err) {
  (void)opt;
  const double wait = octo::aligned_wait(octo::wall_now(), offset, bias, lead);
  if (wait > 0.0) {
    std::this_thread::sleep_for(std::chrono::duration<double>(wait));
  }

  const double send_at = octo::wall_now();
  const octo::bmd::Civil when = octo::aligned_value(send_at, offset, bias);
  const std::vector<uint8_t> packet = octo::bmd::rtc_packet(when, 0);

  const double t0 = octo::mono_now();
  const bool ok = link->write_control(packet, 10.0, err);
  *latency = octo::mono_now() - t0;
  *wrote = when;
  return ok;
}

// ------------------------------------------------------------------ a cycle

// Why this cycle is running, and what it found out.
//
// A scheduled cycle carries an empty errand. One asked for over the control
// socket carries what was asked and collects the answer, because the client
// that asked is on another thread and will come back for it.
struct Errand {
  bool force_sync = false;   // overrule the advisory gates
  bool set_source = false;   // write 4.7 instead of the clock
  int64_t source_value = 0;

  // Filled in as the cycle goes and published however the cycle ends. Every
  // early return then leaves a partial picture rather than none, which is what
  // makes "the camera is here but the bench is not" visible to a client.
  octo::CameraStatus status;
  bool have_status = false;
  octo::BenchStatus bench;

  // Which camera this is for. Empty means whichever one the daemon was
  // configured to follow.
  std::string camera;

  bool ok = false;
  std::string outcome;

  void note(bool good, std::string text) {
    ok = good;
    outcome = std::move(text);
  }
};

void run_cycle(octo::CameraLink* link, octo::SyncState* state,
               const Options& opt, octo::JsonLog* log, octo::CamDb* db,
               const octo::CamConf& conf, octo::PollPlan* plan,
               Errand* errand) {
  Record rec;
  // A cycle that never reaches the camera learns nothing about when to look
  // again, so the floor stands until one does.
  *plan = octo::PollPlan();
  plan->seconds = opt.sync.poll;

  const Bench bench = read_bench(opt);
  const double offset = (opt.source == Source::kMac) ? 0.0 : bench.offset;

  errand->bench.has = true;
  errand->bench.source = opt.source == Source::kMac ? "mac" : "tentacle";
  errand->bench.boxes = bench.boxes;
  errand->bench.offset_s = bench.offset;
  errand->bench.spread_s = bench.spread;
  errand->bench.daemon_reachable = bench.source == "octomancerd";

  if (opt.source == Source::kTentacle) {
    if (!bench.ok) {
      say("no Tentacle boxes heard -- nothing to sync to");
      rec.action("skip:no-tentacle");
      rec.integer("tentacles", 0);
      log->record("cycle", rec.fields());
      return;
    }
    rec.integer("tentacles", bench.boxes);
    rec.num("tentacle_offset_s", bench.offset);
    rec.num("tentacle_spread_s", bench.spread);
    rec.str("bench_source", bench.source);
    if (!bench.boxes_json.empty() && bench.boxes_json != "{}") {
      rec.add("boxes", bench.boxes_json);
    }
    if (bench.spread > opt.sync.bench_spread) {
      say("WARNING: Tentacle boxes disagree by %.3fs -- not all jammed to the"
          " same source", bench.spread);
      rec.boolean("bench_disagreement", true);
    }
  } else {
    rec.str("bench_source", "this Mac");
  }

  const std::string target_camera =
      errand->camera.empty() ? opt.camera : errand->camera;
  std::string picked_name;
  if (!connect_camera(link, state, opt, db, target_camera, &picked_name)) {
    if (opt.source == Source::kTentacle) {
      say("Tentacles at %+.3fs, but no camera found", offset);
    } else {
      say("no camera found");
    }
    rec.action("skip:no-camera");
    errand->note(false, "no camera found");
    log->record("cycle", rec.fields());
    return;
  }
  rec.str("camera_id", state->camera_id);

  errand->have_status = true;
  errand->status.id = state->camera_id;
  errand->status.name = picked_name;
  errand->status.present = true;
  errand->status.connected = true;
  errand->status.action = "connected";

  std::string err;
  if (!link->subscribe(opt.sync.camera_wait, &err)) {
    say("connected, but %s", err.c_str());
    rec.action("skip:no-characteristics");
    errand->status.action = "no-characteristics";
    errand->status.connected = false;
    errand->note(false, err);
    link->disconnect();
    log->record("cycle", rec.fields());
    return;
  }

  octo::CameraView view = link->await_state(opt.sync.camera_wait);
  const int fps = view.has_fps ? view.fps : opt.sync.fps;
  rec.integer("camera_fps", fps);
  if (view.has_transport) rec.integer("camera_transport", view.transport);

  errand->status.has_fps = view.has_fps;
  errand->status.fps = fps;
  errand->status.has_source = view.has_timecode_source;
  errand->status.source = view.timecode_source;
  errand->status.recording =
      view.has_transport && view.transport == octo::bmd::kTransportRecord;

  // Permission, read once and applied to everything this cycle could do.
  // "Writes are disabled" has to cover the timecode source as well as the
  // clock, or the setting would be a promise the program does not keep. The
  // clock path gets this through Conditions, so decide() stays the one place
  // that gate is applied; the timecode source does not go through decide() and
  // is refused here.
  const bool may_write = conf.writes_enabled(state->camera_id);
  errand->status.writes_enabled = may_write;
  rec.boolean("writes_enabled", may_write);

  // Writing 4.7 is the one errand that does not care what the clock says, so
  // it is answered here -- before the timecode is required, because in the
  // mode this exists to escape the camera's timecode is exactly what has
  // stopped being informative.
  if (errand->set_source) {
    if (!may_write) {
      say("  gate: writes are disabled for this camera");
      rec.action("skip:writes-disabled");
      errand->status.action = "skip:writes-disabled";
      errand->note(false,
                   "writes are disabled for this camera -- enable it with"
                   " `octomancer writes on`");
      link->disconnect();
      plan->seconds = opt.sync.poll;
      log->record("cycle", rec.fields());
      return;
    }
    const std::vector<uint8_t> packet = octo::bmd::build_packet(
        octo::bmd::kGroupOutput, octo::bmd::kParamTimecodeSource,
        octo::bmd::kTypeInt8, octo::bmd::kOpAssign,
        {static_cast<uint8_t>(errand->source_value)});
    std::string werr;
    if (!link->write_control(packet, 10.0, &werr)) {
      say("  timecode source write rejected: %s", werr.c_str());
      rec.action("source:rejected");
      errand->note(false, werr);
    } else {
      // The camera echoes a change back on the Incoming Control
      // characteristic, so the write is not believed until the echo says so.
      // A GATT ack only proves the bytes were taken.
      std::this_thread::sleep_for(std::chrono::duration<double>(1.5));
      const octo::CameraView after = link->view();
      const bool echoed = after.has_timecode_source &&
                          after.timecode_source == errand->source_value;
      rec.integer("timecode_source_set", errand->source_value);
      rec.boolean("timecode_source_echoed", echoed);
      if (after.has_timecode_source) {
        errand->status.has_source = true;
        errand->status.source = after.timecode_source;
      }
      rec.action(echoed ? "source:ok" : "source:unverified");
      say("  timecode source -> %lld%s",
          static_cast<long long>(errand->source_value),
          echoed ? " (echoed back)" : " (no echo; unverified)");
      errand->note(echoed,
                   echoed ? fmt("timecode source is now %lld",
                                static_cast<long long>(errand->source_value))
                          : "the camera did not echo the change back");
    }
    link->disconnect();
    plan->seconds = opt.sync.poll;
    log->record("cycle", rec.fields());
    return;
  }

  if (!view.has_timecode) {
    say("camera connected but sent no timecode");
    rec.action("skip:no-timecode");
    errand->status.action = "no-timecode";
    errand->status.connected = false;
    errand->note(false, "the camera sent no timecode");
    link->disconnect();
    log->record("cycle", rec.fields());
    return;
  }

  // The reading in `view` arrived at view.timecode_mono and has been sitting
  // there ever since. Charging the camera for that wait is what made every
  // measurement before this one come out negative.
  const double now_mono = octo::mono_now();
  const double tc_age = octo::reading_age_s(now_mono, view.timecode_mono);
  const double centre =
      opt.sync.centre_frames ? octo::frame_centre_s(fps) : 0.0;
  const double cam = octo::bmd::timecode_sod(view.timecode, fps) + centre;
  const double want =
      octo::local_seconds_of_day(octo::wall_now() - tc_age) + offset;
  const double error = octo::wrap_delta(cam - want);

  errand->status.timecode = octo::bmd::format_timecode(view.timecode);
  errand->status.has_error = true;
  errand->status.error_s = error;

  rec.str("camera_tc", octo::bmd::format_timecode(view.timecode));
  rec.num("camera_sod", cam);
  rec.num("target_sod", want);
  rec.num("error_s", error);
  rec.num("tc_age_s", tc_age, 4);
  rec.num("frame_centre_s", centre, 4);

  const octo::Drift drift = octo::observe(opt.sync, state, error, now_mono);
  if (drift.restarted) {
    say("camera's clock jumped %+.3fs -- that is a power cycle, not drift;"
        " forgetting what was measured about the old one", drift.restart_step);
    rec.boolean("camera_restarted", true);
    rec.num("restart_step_s", drift.restart_step, 3);
  }
  if (drift.has_step) {
    rec.num("drift_ppm", drift.step_ppm, 2);
    rec.num("drift_dt_s", drift.step_dt, 1);
    rec.boolean("drift_shown", drift.step_shown);
  }
  if (drift.has_anchor) {
    rec.num("anchor_span_s", drift.anchor_span, 1);
    rec.num("anchor_drift_ppm", drift.anchor_ppm, 2);
    rec.boolean("anchor_drift_shown", drift.anchor_shown);
  }

  octo::Conditions cond;
  cond.recording =
      view.has_transport && view.transport == octo::bmd::kTransportRecord;
  cond.has_timecode_source = view.has_timecode_source;
  cond.timecode_source = view.timecode_source;
  cond.writes_enabled = may_write;
  rec.boolean("recording", cond.recording);
  if (cond.has_timecode_source) {
    rec.integer("timecode_source", cond.timecode_source);
  }

  std::string drift_note;
  if (drift.anchor_shown) {
    drift_note = fmt("  drift %+.1f ppm over %s", drift.anchor_ppm,
                     octo::format_span(drift.anchor_span).c_str());
  } else if (drift.step_shown) {
    drift_note = fmt("  drift %+.1f ppm", drift.step_ppm);
  }

  if (opt.source == Source::kTentacle) {
    say("tentacles %+.3fs (%d boxes, spread %.3fs) | camera %s err %+.3fs%s",
        offset, bench.boxes, bench.spread,
        octo::bmd::format_timecode(view.timecode).c_str(), error,
        drift_note.c_str());
  } else {
    say("this Mac | camera %s err %+.3fs%s",
        octo::bmd::format_timecode(view.timecode).c_str(), error,
        drift_note.c_str());
  }

  // Worked out from whatever the cycle ends up knowing, so a write's effect on
  // the rate limit is already in the state by the time this is asked.
  auto plan_next = [&](double latest_error) {
    *plan = octo::next_poll(opt.sync, *state, latest_error, fps,
                            octo::mono_now());
    rec.num("next_poll_s", plan->seconds, 1);
    rec.str("next_poll_reason", plan->reason);
    if (plan->until_actionable > 0.0) {
      rec.num("actionable_in_s", plan->until_actionable, 1);
    }
    if (state->drift.has) {
      rec.num("drift_estimate_ppm", state->drift.ppm, 2);
      rec.num("drift_estimate_span_s", state->drift.span, 1);
    }
    if (!plan->message.empty()) say("%s", plan->message.c_str());
  };

  octo::Decision decision =
      octo::decide(opt.sync, *state, error, fps, cond, now_mono);
  rec.num("tolerance_s", decision.tolerance, 6);
  if (decision.since_write > 0.0) {
    rec.num("since_write_s", decision.since_write, 1);
  }

  // Someone asked for this by hand. That overrules the gates that mean "there
  // is no need" and none of the ones that mean "must not" -- see
  // gate_is_advisory. The original verdict is logged either way, so a forced
  // write does not quietly erase the fact that the daemon would not have made
  // it on its own.
  if (errand->force_sync && decision.action != octo::Action::kWrite) {
    rec.str("gate_overridden", octo::action_name(decision.action));
    if (octo::gate_is_advisory(decision.action)) {
      say("  forced: overruling %s", octo::action_name(decision.action));
      decision.action = octo::Action::kWrite;
    }
  }

  errand->status.action = octo::action_name(decision.action);

  if (decision.action != octo::Action::kWrite) {
    say("%s", decision.message.c_str());
    rec.action(octo::action_name(decision.action));
    // A gate is a refusal, not a breakdown, so a forced request is told what
    // stopped it and counted as answered rather than as failed. The one
    // exception is the timecode source, where the honest answer is that this
    // camera cannot be helped until that is changed.
    std::string why = octo::action_name(decision.action);
    bool answered = true;
    if (decision.action == octo::Action::kSkipTimecodeSource) {
      why = fmt("timecode source is %lld, so the clock cannot be set --"
                " switch it to time-of-day first",
                static_cast<long long>(cond.timecode_source));
      answered = false;
    } else if (decision.action == octo::Action::kSkipWritesDisabled) {
      why = "writes are disabled for this camera -- enable it with"
            " `octomancer writes on`";
      answered = false;
    }
    errand->note(answered, why);
    link->disconnect();
    plan_next(error);
    log->record("cycle", rec.fields());
    return;
  }

  const int bias = state->rtc_bias;
  const double lead = octo::effective_lead(opt.sync, *state);
  octo::bmd::Civil wrote;
  double latency = 0.0;
  if (!aligned_write(link, opt, offset, bias, lead, &wrote, &latency, &err)) {
    say("  write rejected: %s", err.c_str());
    rec.action("write:rejected");
    rec.str("error", err);
    state->failures += 1;
    link->disconnect();
    log->record("cycle", rec.fields());
    return;
  }

  state->has_last_write = true;
  state->last_write_mono = octo::mono_now();
  rec.str("write_utc", fmt("%02d:%02d:%02d", wrote.hour, wrote.minute,
                           wrote.second));
  rec.num("write_latency_s", latency);
  rec.integer("rtc_bias", bias);
  rec.num("lead_s", lead, 6);
  say("  wrote RTC %02d:%02d:%02d UTC (bias %+ds, %.0fms lead, %.0fms latency)",
      wrote.hour, wrote.minute, wrote.second, bias, lead * 1000.0,
      latency * 1000.0);

  // A GATT ack proves the characteristic took the bytes and nothing more, so
  // verify against the camera's own clock. The notifications never stopped, so
  // just drop the stale reading and let a fresh one arrive.
  link->forget_timecode();
  std::this_thread::sleep_for(
      std::chrono::duration<double>(opt.sync.verify_wait));
  view = link->await_state(opt.sync.camera_wait);
  if (!view.has_timecode) {
    say("  could not verify: no timecode after the write");
    rec.action("write:unverified");
    state->failures += 1;
    link->disconnect();
    log->record("cycle", rec.fields());
    return;
  }

  const double cam2 = octo::bmd::timecode_sod(view.timecode, fps) +
                      (opt.sync.centre_frames ? octo::frame_centre_s(fps) : 0.0);
  const double age2 = octo::reading_age_s(octo::mono_now(), view.timecode_mono);
  const double err2 = octo::wrap_delta(
      cam2 - (octo::local_seconds_of_day(octo::wall_now() - age2) + offset));
  rec.num("error_after_s", err2);
  rec.num("tc_age_after_s", age2, 4);
  rec.str("camera_tc_after", octo::bmd::format_timecode(view.timecode));

  const octo::WriteOutcome outcome =
      octo::judge_write(opt.sync, state, error, err2, octo::mono_now());
  rec.boolean("verified", outcome.verified);
  say("%s", outcome.message.c_str());

  errand->status.has_error = true;
  errand->status.error_s = err2;
  errand->status.has_last_write = true;
  errand->status.last_write_wall = octo::wall_now();
  errand->status.writes += 1;
  errand->status.timecode = octo::bmd::format_timecode(view.timecode);
  errand->note(outcome.verified,
               outcome.verified
                   ? fmt("corrected to within %+.0fms", err2 * 1000.0)
                   : fmt("the write did not take: still %+.3fs out", err2));

  switch (outcome.verdict) {
    case octo::Verdict::kOk:
      rec.action("write:ok");
      if (outcome.bias_changed) {
        rec.integer("rtc_bias_next", outcome.bias_after);
        say("  learned: RTC bias %+ds -> %+ds", outcome.bias_before,
            outcome.bias_after);
      }
      break;
    case octo::Verdict::kAdapting:
      rec.action("write:adapting");
      rec.integer("rtc_bias_next", outcome.bias_after);
      break;
    case octo::Verdict::kNoEffect:
      rec.action("write:no-effect");
      say("  something else may be driving this camera's timecode");
      break;
  }

  // What this write says about how long the camera takes to act.
  //
  // Re-derived from the body's whole recorded history rather than from this
  // one observation: at 24fps a single residual carries +/-21ms of frame
  // quantisation, so chasing each one individually would make the lead jitter
  // by more than the tolerance it is trying to reach.
  rec.num("apply_delay_s", octo::observed_apply_delay(lead, err2), 6);
  if (db != nullptr && db->enabled() && !state->camera_id.empty()) {
    octo::WriteSample sample;
    sample.wall = octo::wall_now();
    sample.error_before_s = error;
    sample.error_after_s = err2;
    sample.lead_used_s = lead;
    sample.latency_s = latency;
    sample.fps = fps;
    sample.bias = bias;
    sample.verified = outcome.verified;
    sample.timing_ok = outcome.timing_usable;
    // Only claim the current basis when the reader is actually running on it.
    sample.measure_epoch = opt.sync.centre_frames ? octo::kMeasureEpoch : 0;

    std::string derr;
    if (!db->record_write(state->camera_id, sample, &derr)) {
      say("  camera database: %s", derr.c_str());
    }

    const octo::CameraRecord* known = db->find(state->camera_id);
    if (known != nullptr && opt.sync.adapt_lead) {
      const size_t window = static_cast<size_t>(
          opt.sync.lead_window > 0 ? opt.sync.lead_window : 1);
      const octo::LeadEstimate est =
          octo::estimate_lead(known->recent_apply_delays(window), opt.sync);
      if (est.has) {
        const double before = octo::effective_lead(opt.sync, *state);
        if (std::fabs(est.lead_s - before) > 1e-4) {
          say("  learned: send lead %.0fms -> %.0fms (median of %d writes)",
              before * 1000.0, est.lead_s * 1000.0, est.samples);
        }
        state->lead = est;
      }
    }

    if (db->learn(state->camera_id, true, state->rtc_bias, state->lead.has,
                  state->lead.lead_s, state->drift.has, state->drift.ppm,
                  state->drift.span)) {
      if (!db->record_params(state->camera_id, &derr)) {
        say("  camera database: %s", derr.c_str());
      }
    }
  }

  link->disconnect();
  plan_next(err2);
  log->record("cycle", rec.fields());
}

// ------------------------------------------------------------ probe modes

int mode_scan_only(octo::CameraLink* link, const Options& opt) {
  std::printf("scanning %.0fs...\n", opt.sync.scan_timeout);
  std::fflush(stdout);
  const octo::ScanResult found =
      link->scan(opt.sync.scan_timeout, opt.camera, opt.show_all);

  if (opt.show_all) {
    std::printf("  -- every LE device seen (%d) --\n", found.total);
    for (const octo::CameraDevice& d : found.all) {
      std::printf("     %-38s %-28s rssi=%d\n", d.id.c_str(),
                  d.name.empty() ? "(no name)" : d.name.substr(0, 28).c_str(),
                  d.rssi);
    }
    std::printf("  -- end --\n");
  }

  for (const octo::CameraDevice& d : found.cameras) {
    std::printf("  %-38s %-22s rssi=%-5d (%s)\n", d.id.c_str(),
                d.name.empty() ? "(no name)" : d.name.c_str(), d.rssi,
                d.by_service_uuid ? "service uuid" : "name guess");
  }
  if (found.cameras.empty()) {
    std::printf("  no Blackmagic cameras found (%d LE devices seen, %d of them"
                " Tentacles).\n", found.total, found.tentacles);
  }
  return found.cameras.empty() ? 1 : 0;
}

// Connect and watch, without writing anything. This is how you check that a
// camera is reachable and that its timecode is actually running before
// blaming the daemon for not correcting it.
int mode_watch(octo::CameraLink* link, octo::SyncState* state,
               const Options& opt, octo::CamDb* db) {
  if (!connect_camera(link, state, opt, db, opt.camera, nullptr)) return 1;
  std::string err;
  if (!link->subscribe(opt.sync.camera_wait, &err)) {
    std::fprintf(stderr, "octomancer-sync: %s\n", err.c_str());
    link->disconnect();
    return 1;
  }

  std::printf("connected. watching %.0fs -- nothing will be written.\n",
              opt.watch_seconds);
  const double deadline = octo::mono_now() + opt.watch_seconds;
  octo::bmd::Timecode last;
  bool have_last = false;
  while (octo::mono_now() < deadline && !g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const octo::CameraView view = link->view();
    if (!view.has_timecode) continue;
    if (have_last && view.timecode.seconds == last.seconds &&
        view.timecode.frames == last.frames) {
      continue;
    }
    last = view.timecode;
    have_last = true;

    const int fps = view.has_fps ? view.fps : opt.sync.fps;
    const double age = octo::reading_age_s(octo::mono_now(), view.timecode_mono);
    const double cam = octo::bmd::timecode_sod(last, fps) +
                       (opt.sync.centre_frames ? octo::frame_centre_s(fps) : 0.0);
    const double mac = octo::local_seconds_of_day(octo::wall_now() - age);
    say("%s   this Mac %s   diff %+.3fs   age %.0fms   %dfps%s",
        octo::bmd::format_timecode(last).c_str(),
        octo::format_sod(mac).c_str(), octo::wrap_delta(cam - mac),
        age * 1000.0, fps,
        (view.has_transport && view.transport == octo::bmd::kTransportRecord)
            ? "   RECORDING"
            : "");
  }

  const octo::CameraView view = link->view();
  if (!view.state.empty()) {
    std::printf("\nwhat the camera volunteered:\n");
    for (const auto& entry : view.state) {
      const octo::bmd::Value& v = entry.second;
      std::string vals;
      for (size_t i = 0; i < v.ints.size(); ++i) {
        vals += fmt("%s%lld", i ? ", " : "", static_cast<long long>(v.ints[i]));
      }
      for (size_t i = 0; i < v.reals.size(); ++i) {
        vals += fmt("%s%.4f", i ? ", " : "", v.reals[i]);
      }
      if (!v.text.empty()) vals += v.text;
      std::printf("  %d.%-3d %s\n", entry.first.first, entry.first.second,
                  vals.c_str());
    }
  }
  link->disconnect();
  return have_last ? 0 : 4;
}

// Prove the RTC write lands, by writing a deliberately wrong clock.
//
// The first version of this test wrote the *correct* time, and concluded from
// the timecode not moving that group 7.0 was unimplemented. It was not: the
// camera was already there. A test whose pass and fail states look identical
// is not a test, so this one aims somewhere the camera demonstrably is not.
// Parse "ff 08 00 ff 09 04 03 00 12 33 22 11" into bytes. Whitespace and
// colons are ignored so a packet can be pasted from any of our own logs.
bool parse_hex(const std::string& in, std::vector<uint8_t>* out,
               std::string* err) {
  out->clear();
  int hi = -1;
  for (char c : in) {
    if (c == ' ' || c == ':' || c == ',' || c == '\t') continue;
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else { *err = std::string("not hex: '") + c + "'"; return false; }
    if (hi < 0) { hi = v; continue; }
    out->push_back(static_cast<uint8_t>((hi << 4) | v));
    hi = -1;
  }
  if (hi >= 0) { *err = "odd number of hex digits"; return false; }
  if (out->empty()) { *err = "empty packet"; return false; }
  return true;
}

// Send hand-written packets and watch what the camera does about it.
//
// This exists to answer questions whose answer is probably "nothing happens",
// which is exactly the shape of experiment that does not deserve a permanent
// flag. It is here to be used for one session and then argued about.
int mode_poke(octo::CameraLink* link, octo::SyncState* state,
              const Options& opt, octo::CamDb* db) {
  std::vector<std::vector<uint8_t>> packets;
  for (const std::string& hex : opt.pokes) {
    std::vector<uint8_t> bytes;
    std::string perr;
    if (!parse_hex(hex, &bytes, &perr)) {
      std::fprintf(stderr, "octomancer-sync: --poke %s: %s\n", hex.c_str(),
                   perr.c_str());
      return 2;
    }
    packets.push_back(bytes);
  }

  if (!connect_camera(link, state, opt, db, opt.camera, nullptr)) return 1;
  std::string err;
  if (!link->subscribe(opt.sync.camera_wait, &err)) {
    std::fprintf(stderr, "octomancer-sync: %s\n", err.c_str());
    link->disconnect();
    return 1;
  }
  octo::CameraView view = link->await_state(opt.sync.camera_wait);

  // A timecode discontinuity mid-take corrupts the take. This is the one gate
  // that matters more than any answer these probes could return.
  if (view.has_transport && view.transport == octo::bmd::kTransportRecord) {
    std::fprintf(stderr, "octomancer-sync: the camera is RECORDING."
                         " Refusing to write anything.\n");
    link->disconnect();
    return 3;
  }

  std::printf("\nwhat the camera volunteered (%d parameters):\n",
              static_cast<int>(view.state.size()));
  for (const auto& entry : view.state) {
    const octo::bmd::Value& v = entry.second;
    std::string vals;
    for (size_t i = 0; i < v.ints.size(); ++i) {
      vals += fmt("%s%lld", i ? ", " : "", static_cast<long long>(v.ints[i]));
    }
    for (size_t i = 0; i < v.reals.size(); ++i) {
      vals += fmt("%s%.4f", i ? ", " : "", v.reals[i]);
    }
    if (!v.text.empty()) vals += v.text;
    std::printf("   %d.%-3d op=%d  [%s]\n", entry.first.first,
                entry.first.second, v.op, vals.c_str());
  }
  std::printf("\n");
  std::fflush(stdout);

  const int fps = view.has_fps ? view.fps : opt.sync.fps;
  int sent = 0;
  for (const std::vector<uint8_t>& packet : packets) {
    if (g_stop) break;
    const octo::CameraView before = link->view();
    say("--> %s", octo::bmd::to_hex(packet).c_str());
    if (before.has_timecode) {
      say("    timecode before: %s",
          octo::bmd::format_timecode(before.timecode).c_str());
    }
    std::string werr;
    const double t0 = octo::mono_now();
    const bool ok = link->write_control(packet, opt.sync.camera_wait, &werr);
    const double latency = octo::mono_now() - t0;
    if (!ok) {
      say("    GATT REFUSED after %.0fms: %s", latency * 1000.0, werr.c_str());
    } else {
      say("    GATT accepted in %.0fms", latency * 1000.0);
    }
    ++sent;

    // Watch long enough for a state echo and a few timecode notifications.
    const double until = octo::mono_now() + opt.poke_watch;
    std::map<std::pair<int, int>, octo::bmd::Value> was = before.state;
    while (octo::mono_now() < until && !g_stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      const octo::CameraView now = link->view();
      for (const auto& entry : now.state) {
        auto old = was.find(entry.first);
        std::string vals;
        for (size_t i = 0; i < entry.second.ints.size(); ++i) {
          vals += fmt("%s%lld", i ? ", " : "",
                      static_cast<long long>(entry.second.ints[i]));
        }
        if (old == was.end()) {
          say("    ECHO new %d.%d = [%s]", entry.first.first,
              entry.first.second, vals.c_str());
          was[entry.first] = entry.second;
        } else if (old->second.ints != entry.second.ints) {
          say("    ECHO %d.%d changed to [%s]", entry.first.first,
              entry.first.second, vals.c_str());
          old->second = entry.second;
        }
      }
    }
    const octo::CameraView after = link->view();
    if (after.has_timecode) {
      const double moved =
          octo::bmd::timecode_sod(after.timecode, fps) -
          octo::bmd::timecode_sod(before.timecode, fps);
      say("    timecode after:  %s   (moved %+.2fs, %.1fs elapsed)",
          octo::bmd::format_timecode(after.timecode).c_str(), moved,
          opt.poke_watch);
      // Anything beyond free-running is the signal we are hunting.
      if (std::fabs(moved - opt.poke_watch) > 0.5) {
        say("    *** THE TIMECODE DID SOMETHING OTHER THAN FREE-RUN ***");
      }
    }
    std::printf("\n");
    std::fflush(stdout);
  }

  say("sent %d packet(s)", sent);
  link->disconnect();
  return 0;
}

int mode_rtc_test(octo::CameraLink* link, octo::SyncState* state,
                  const Options& opt, octo::CamDb* db) {
  if (!connect_camera(link, state, opt, db, opt.camera, nullptr)) return 1;
  std::string err;
  if (!link->subscribe(opt.sync.camera_wait, &err)) {
    std::fprintf(stderr, "octomancer-sync: %s\n", err.c_str());
    link->disconnect();
    return 1;
  }
  octo::CameraView view = link->await_state(opt.sync.camera_wait);
  if (!view.has_timecode) {
    std::fprintf(stderr,
                 "octomancer-sync: no timecode in %.0fs. This needs the camera"
                 " set to Time of Day;\nin Record Run the timecode does not"
                 " follow the clock and there is nothing to see.\n",
                 opt.sync.camera_wait);
    link->disconnect();
    return 4;
  }

  const std::string before = octo::bmd::format_timecode(view.timecode);
  const double offset = -3600.0;  // an hour back: unmistakable, and reversible
  const octo::bmd::Civil when = octo::bmd::utc_civil(octo::wall_now() + offset);
  const std::vector<uint8_t> packet = octo::bmd::rtc_packet(when, 0);
  std::printf("timecode now %s\n", before.c_str());
  std::printf("writing RTC = %04d-%02d-%02d %02d:%02d:%02d UTC"
              " (deliberately an hour slow)\n",
              when.year, when.month, when.day, when.hour, when.minute,
              when.second);
  std::printf("  bytes: %s\n", octo::bmd::to_hex(packet).c_str());
  std::fflush(stdout);

  if (!link->write_control(packet, 10.0, &err)) {
    std::printf("\nthe write was REJECTED: %s\n", err.c_str());
    link->disconnect();
    return 5;
  }

  link->forget_timecode();
  std::this_thread::sleep_for(std::chrono::seconds(3));
  view = link->await_state(opt.sync.camera_wait);
  const std::string after = view.has_timecode
                                ? octo::bmd::format_timecode(view.timecode)
                                : std::string("(none)");
  std::printf("\ntimecode %s -> %s\n", before.c_str(), after.c_str());

  int rc = 5;
  if (view.has_timecode) {
    const double moved = octo::wrap_delta(
        octo::bmd::timecode_sod(view.timecode, opt.sync.fps) -
        octo::local_seconds_of_day(octo::wall_now()));
    if (moved < -1800.0) {
      std::printf("The clock followed the write. RTC (7.0) is implemented and"
                  " the timecode\nfollows it, which is the whole basis for"
                  " syncing this camera over BLE.\n");
      rc = 0;
    } else {
      std::printf("The write was accepted but the clock did not move. Either"
                  " the camera is not\nin Time of Day mode, or something else"
                  " is driving its timecode.\n");
    }
  }
  std::printf("\nPut it back with:  octomancer-sync --once\n");
  link->disconnect();
  return rc;
}

// ------------------------------------------------------------ command line

void usage(FILE* out) {
  std::fprintf(out,
      "usage: octomancer-sync [options]\n"
      "\n"
      "Keep a Blackmagic camera's clock on Tentacle time. Runs until"
      " interrupted.\n"
      "\n"
      "  --camera NAME|ID      which camera (default: the first one found)\n"
      "  --source tentacle|mac what to sync to (default tentacle)\n"
      "  --poll SEC            shortest gap between cycles (default 60). Once\n"
      "                        drift has been measured the real interval\n"
      "                        stretches towards whenever a write could next\n"
      "                        matter, and tightens again as that nears.\n"
      "  --max-poll SEC        longest it may stretch to (default 900)\n"
      "  --poll-slices N       observations to take across that wait (default"
      " 4)\n"
      "  --fixed-poll          do not adapt; use --poll for every cycle\n"
      "  --presence-poll SEC   how often to ask octomancerd whether the camera"
      "\n"
      "                        is on the air (default 5). This is a socket"
      " read,\n"
      "                        not a scan, and it is what makes switching the\n"
      "                        camera on trigger a sync within seconds.\n"
      "  --listen SEC          how long to listen for Tentacles when"
      " octomancerd\n"
      "                        is not running (default 8)\n"
      "  --socket PATH         octomancerd's socket (default %s)\n"
      "  --no-daemon           always listen directly, never ask octomancerd\n"
      "  --control-socket PATH where `octomancer` reaches this daemon\n"
      "  --no-control          do not serve a control socket at all\n"
      "  --lock PATH           the file that keeps a second one of these from\n"
      "                        starting. The modes that never write -- and\n"
      "                        --dry-run among them -- do not take it.\n"
      "  --camera-config PATH  per-camera permissions, read-only to this\n"
      "                        program (default ~/.octomancer/cameras.conf)\n"
      "  --log PATH            JSONL log ('' to disable, default"
      " octomancer-sync.jsonl)\n"
      "  --console PATH        send stdout and stderr here, and rotate them\n"
      "  --log-max-bytes N     rotate past this size, 0 to never (default"
      " 16M)\n"
      "  --log-keep N          generations to keep beside it (default 5)\n"
      "  --once                run a single cycle and exit\n"
      "  --dry-run             decide and log, but never write\n"
      "\n"
      "thresholds\n"
      "  --tolerance-frames N  leave the clock alone within this many frames,\n"
      "                        at whatever rate the camera reports (default"
      " 0.5)\n"
      "  --tolerance SEC       the same threshold in seconds; overrides"
      " --tolerance-frames\n"
      "  --write-tolerance SEC how close a write must land to count as having\n"
      "                        taken (default 1). Looser than --tolerance on\n"
      "                        purpose: judging a write against half a frame"
      " would\n"
      "                        mark every good write a failure, and"
      " --max-failures\n"
      "                        of those in a row stops the daemon writing at"
      " all.\n"
      "  --min-write-interval SEC   never write more often than this (default"
      " 3600),\n"
      "                        so there are long free-running stretches to\n"
      "                        measure drift across\n"
      "  --max-failures N      failed writes before assuming an external"
      " timecode\n"
      "                        source owns the camera (default 3)\n"
      "  --bench-spread SEC    warn if the Tentacle boxes disagree by more"
      " than this\n"
      "\n"
      "the camera's own RTC offset\n"
      "  --rtc-bias SEC        starting guess (default 0; it is learned)\n"
      "  --no-adapt-bias       do not learn it from what writes land on\n"
      "  --max-bias-step SEC   largest single correction (default 120)\n"
      "  --max-adapts N        corrections to try before calling a write"
      " failed\n"
      "\n"
      "timing\n"
      "  --camera-db PATH      per-camera settings (default"
      " ~/.octomancer/per_camera.json)\n"
      "  --no-camera-db        do not remember anything between runs\n"
      "  --db-max-samples N    writes kept per camera (default 1000)\n"
      "  --no-adapt-lead       keep --lead fixed instead of measuring it\n"
      "  --no-centre-frames    read timecode at face value, not frame centre\n"
      "  --poke HEX            write a hand-written packet and watch (repeat"
      " for a sweep)\n"
      "  --poke-watch SEC      how long to watch after each --poke"
      " (default 4)\n"
      "  --lead-window N       writes the measured lead is a median of"
      " (default 9)\n"
      "  --max-lead SEC        clamp on the measured lead (default 0.5)\n"
      "  --lead SEC            how early to send, to cover BLE latency"
      " (default 0.05)\n"
      "  --verify-wait SEC     settle time before checking a write (default"
      " 3)\n"
      "  --camera-wait SEC     how long to wait for camera state (default 6)\n"
      "  --scan-timeout SEC    BLE scan duration (default 20)\n"
      "  --connect-timeout SEC camera connect timeout (default 15)\n"
      "  --min-drift-interval SEC   shortest gap whose drift is worth"
      " believing\n"
      "                        (default 1800; shorter is quantisation, and is\n"
      "                        logged but neither shown nor scheduled"
      " against)\n"
      "  --min-ppm N           floor under the drift used for scheduling\n"
      "                        (default 5; a clock that measured 2 ppm this\n"
      "                        afternoon will not hold it all night)\n"
      "  --restart-step SEC    an error jump larger than this is a power"
      " cycle,\n"
      "                        not drift; everything learned is discarded\n"
      "                        (default 1)\n"
      "  --fps N               fallback frame rate if the camera reports none\n"
      "\n"
      "instead of syncing\n"
      "  --scan-only [--all]   list what is in range and exit\n"
      "  --watch SEC           connect and watch the timecode, writing nothing\n"
      "  --rtc-test            write a deliberately wrong clock, to prove the\n"
      "                        write lands\n"
      "  --packet              print the RTC packet for now and exit, no"
      " Bluetooth\n"
      "\n"
      "  --radio KIND          auto (default), corebluetooth, or dongle\n"
      "  --dongle PORT         the dongle's serial port\n"
      "  --passkey NNNNNN      the passkey the camera displays while pairing;\n"
      "                        needed only over the dongle, which has no bond\n"
      "                        of its own and no screen to prompt on\n"
      "  --hci-trace           log every HCI packet\n"
      "  --version, --help\n",
      octo::default_socket_path().c_str());
}

bool parse_args(int argc, char** argv, Options* opt) {
  enum {
    kCamera = 1000, kSource, kPoll, kListen, kSocket, kNoDaemon, kLog, kOnce,
    kControlSocket, kNoControl, kCameraConfig, kLockFile,
    kDryRun, kToleranceFrames, kTolerance, kWriteTolerance, kMinWriteInterval,
    kMaxFailures, kBenchSpread, kRtcBias, kNoAdaptBias, kMaxBiasStep,
    kMaxAdapts, kLead, kVerifyWait, kCameraWait, kScanTimeout, kConnectTimeout,
    kMinDriftInterval, kFps, kScanOnly, kAll, kWatch, kRtcTest, kPacket,
    kPoke, kPokeWatch,
    kMaxPoll, kPollSlices, kFixedPoll, kPresencePoll, kConsole, kLogMax,
    kLogKeep, kMinPpm, kRestartStep,
    kCameraDb, kNoCameraDb, kDbMaxSamples, kNoAdaptLead, kLeadWindow, kMaxLead,
    kNoCentreFrames,
    kRadio, kDongle, kHciTrace, kPasskey,
    kVersion, kHelp,
  };
  static const struct option longs[] = {
      {"camera", required_argument, nullptr, kCamera},
      {"source", required_argument, nullptr, kSource},
      {"poll", required_argument, nullptr, kPoll},
      {"max-poll", required_argument, nullptr, kMaxPoll},
      {"poll-slices", required_argument, nullptr, kPollSlices},
      {"fixed-poll", no_argument, nullptr, kFixedPoll},
      {"presence-poll", required_argument, nullptr, kPresencePoll},
      {"console", required_argument, nullptr, kConsole},
      {"log-max-bytes", required_argument, nullptr, kLogMax},
      {"log-keep", required_argument, nullptr, kLogKeep},
      {"min-ppm", required_argument, nullptr, kMinPpm},
      {"restart-step", required_argument, nullptr, kRestartStep},
      {"camera-db", required_argument, nullptr, kCameraDb},
      {"no-camera-db", no_argument, nullptr, kNoCameraDb},
      {"db-max-samples", required_argument, nullptr, kDbMaxSamples},
      {"no-adapt-lead", no_argument, nullptr, kNoAdaptLead},
      {"no-centre-frames", no_argument, nullptr, kNoCentreFrames},
      {"lead-window", required_argument, nullptr, kLeadWindow},
      {"max-lead", required_argument, nullptr, kMaxLead},
      {"listen", required_argument, nullptr, kListen},
      {"socket", required_argument, nullptr, kSocket},
      {"no-daemon", no_argument, nullptr, kNoDaemon},
      {"control-socket", required_argument, nullptr, kControlSocket},
      {"no-control", no_argument, nullptr, kNoControl},
      {"lock", required_argument, nullptr, kLockFile},
      {"camera-config", required_argument, nullptr, kCameraConfig},
      {"log", required_argument, nullptr, kLog},
      {"once", no_argument, nullptr, kOnce},
      {"dry-run", no_argument, nullptr, kDryRun},
      {"tolerance-frames", required_argument, nullptr, kToleranceFrames},
      {"tolerance", required_argument, nullptr, kTolerance},
      {"write-tolerance", required_argument, nullptr, kWriteTolerance},
      {"min-write-interval", required_argument, nullptr, kMinWriteInterval},
      {"max-failures", required_argument, nullptr, kMaxFailures},
      {"bench-spread", required_argument, nullptr, kBenchSpread},
      {"rtc-bias", required_argument, nullptr, kRtcBias},
      {"no-adapt-bias", no_argument, nullptr, kNoAdaptBias},
      {"max-bias-step", required_argument, nullptr, kMaxBiasStep},
      {"max-adapts", required_argument, nullptr, kMaxAdapts},
      {"lead", required_argument, nullptr, kLead},
      {"verify-wait", required_argument, nullptr, kVerifyWait},
      {"camera-wait", required_argument, nullptr, kCameraWait},
      {"scan-timeout", required_argument, nullptr, kScanTimeout},
      {"connect-timeout", required_argument, nullptr, kConnectTimeout},
      {"min-drift-interval", required_argument, nullptr, kMinDriftInterval},
      {"fps", required_argument, nullptr, kFps},
      {"scan-only", no_argument, nullptr, kScanOnly},
      {"all", no_argument, nullptr, kAll},
      {"watch", required_argument, nullptr, kWatch},
      {"rtc-test", no_argument, nullptr, kRtcTest},
      {"packet", no_argument, nullptr, kPacket},
      {"poke", required_argument, nullptr, kPoke},
      {"poke-watch", required_argument, nullptr, kPokeWatch},
      {"radio", required_argument, nullptr, kRadio},
      {"dongle", required_argument, nullptr, kDongle},
      {"hci-trace", no_argument, nullptr, kHciTrace},
      {"passkey", required_argument, nullptr, kPasskey},
      {"version", no_argument, nullptr, kVersion},
      {"help", no_argument, nullptr, kHelp},
      {nullptr, 0, nullptr, 0},
  };

  for (;;) {
    const int c = getopt_long(argc, argv, "", longs, nullptr);
    if (c == -1) break;
    switch (c) {
      case kCamera: opt->camera = optarg; break;
      case kSource:
        if (std::strcmp(optarg, "mac") == 0) {
          opt->source = Source::kMac;
        } else if (std::strcmp(optarg, "tentacle") == 0) {
          opt->source = Source::kTentacle;
        } else {
          std::fprintf(stderr, "octomancer-sync: --source must be 'tentacle'"
                               " or 'mac'\n");
          return false;
        }
        break;
      case kPoll: opt->sync.poll = std::atof(optarg); break;
      case kMaxPoll: opt->sync.max_poll = std::atof(optarg); break;
      case kPollSlices: opt->sync.poll_slices = std::atoi(optarg); break;
      case kFixedPoll: opt->sync.adaptive_poll = false; break;
      case kPresencePoll: opt->presence_poll = std::atof(optarg); break;
      case kConsole: opt->console_path = optarg; break;
      case kLogMax: opt->rotation.max_bytes = std::atof(optarg); break;
      case kLogKeep: opt->rotation.keep = std::atoi(optarg); break;
      case kMinPpm: opt->sync.min_assumed_ppm = std::atof(optarg); break;
      case kRestartStep: opt->sync.restart_step = std::atof(optarg); break;
      case kListen: opt->sync.listen = std::atof(optarg); break;
      case kSocket: opt->socket_path = optarg; break;
      case kNoDaemon: opt->use_daemon = false; break;
      case kControlSocket: opt->control_path = optarg; break;
      case kNoControl: opt->serve_control = false; break;
      case kLockFile: opt->lock_path = optarg; break;
      case kCameraConfig: opt->camconf_path = optarg; break;
      case kLog: opt->log_path = optarg; break;
      case kOnce: opt->once = true; break;
      case kDryRun: opt->sync.dry_run = true; break;
      case kToleranceFrames:
        opt->sync.tolerance_frames = std::atof(optarg);
        break;
      case kTolerance:
        opt->sync.tolerance = std::atof(optarg);
        opt->sync.has_tolerance = true;
        break;
      case kWriteTolerance: opt->sync.write_tolerance = std::atof(optarg); break;
      case kMinWriteInterval:
        opt->sync.min_write_interval = std::atof(optarg);
        break;
      case kMaxFailures: opt->sync.max_failures = std::atoi(optarg); break;
      case kBenchSpread: opt->sync.bench_spread = std::atof(optarg); break;
      case kRtcBias:
        opt->sync.rtc_bias = std::atoi(optarg);
        opt->has_rtc_bias = true;
        break;
      case kCameraDb:
        opt->camdb_path = optarg;
        opt->camdb_explicit = true;
        break;
      case kNoCameraDb: opt->camdb_path.clear(); break;
      case kDbMaxSamples:
        opt->camdb.max_samples = static_cast<size_t>(std::atol(optarg));
        break;
      case kNoAdaptLead: opt->sync.adapt_lead = false; break;
      case kNoCentreFrames: opt->sync.centre_frames = false; break;
      case kLeadWindow: opt->sync.lead_window = std::atoi(optarg); break;
      case kMaxLead: opt->sync.max_lead = std::atof(optarg); break;
      case kNoAdaptBias: opt->sync.adapt_bias = false; break;
      case kMaxBiasStep: opt->sync.max_bias_step = std::atoi(optarg); break;
      case kMaxAdapts: opt->sync.max_adapts = std::atoi(optarg); break;
      case kLead: opt->sync.lead = std::atof(optarg); break;
      case kVerifyWait: opt->sync.verify_wait = std::atof(optarg); break;
      case kCameraWait: opt->sync.camera_wait = std::atof(optarg); break;
      case kScanTimeout: opt->sync.scan_timeout = std::atof(optarg); break;
      case kConnectTimeout:
        opt->sync.connect_timeout = std::atof(optarg);
        break;
      case kMinDriftInterval:
        opt->sync.min_drift_interval = std::atof(optarg);
        break;
      case kFps: opt->sync.fps = std::atoi(optarg); break;
      case kScanOnly: opt->mode = Mode::kScanOnly; break;
      case kAll: opt->show_all = true; break;
      case kWatch:
        opt->mode = Mode::kWatch;
        opt->watch_seconds = std::atof(optarg);
        break;
      case kRtcTest: opt->mode = Mode::kRtcTest; break;
      case kPacket: opt->mode = Mode::kPacket; break;
      case kPoke:
        opt->mode = Mode::kPoke;
        opt->pokes.push_back(optarg);
        break;
      case kPokeWatch: opt->poke_watch = std::atof(optarg); break;
      case kRadio:
        if (!octo::parse_radio_kind(optarg, &octo::radio_options().kind)) {
          std::fprintf(stderr,
                       "%s: --radio must be auto, corebluetooth or dongle\n",
                       "octomancer-sync");
          return false;
        }
        break;
      case kDongle:
        octo::radio_options().device = optarg;
        // Naming a port is asking for it. Falling back to CoreBluetooth when
        // it turns out not to be there would hide a typo.
        octo::radio_options().kind = octo::RadioKind::kDongle;
        break;
      case kHciTrace: octo::radio_options().trace = true; break;
      case kPasskey: {
        // The six-digit number a camera displays while pairing. Only the
        // dongle needs it: CoreBluetooth prompts on screen and keeps the bond
        // afterwards, while over HCI this program pairs afresh each time.
        char* end = nullptr;
        long n = std::strtol(optarg, &end, 10);
        if (end == optarg || *end != '\0' || n < 0 || n > 999999) {
          std::fprintf(stderr,
                       "octomancer-sync: --passkey must be six digits\n");
          return false;
        }
        octo::radio_options().passkey = static_cast<int>(n);
        break;
      }
      case kVersion:
        std::printf("octomancer-sync %s\n", OCTO_VERSION);
        std::exit(0);
      case kHelp:
        usage(stdout);
        std::exit(0);
      default:
        usage(stderr);
        return false;
    }
  }

  if (opt->sync.max_poll < opt->sync.poll) {
    std::fprintf(stderr,
                 "octomancer-sync: --max-poll (%.0f) is below --poll (%.0f);"
                 " the ceiling cannot be under the floor.\n",
                 opt->sync.max_poll, opt->sync.poll);
    return false;
  }
  if (opt->sync.poll_slices < 1) {
    std::fprintf(stderr, "octomancer-sync: --poll-slices must be at least 1.\n");
    return false;
  }
  if (opt->sync.max_failures < 1) {
    std::fprintf(stderr, "octomancer-sync: --max-failures must be at least 1,"
                         " or the daemon gives up before it has tried.\n");
    return false;
  }
  if (opt->sync.tolerance_frames <= 0 && !opt->sync.has_tolerance) {
    std::fprintf(stderr, "octomancer-sync: --tolerance-frames must be above"
                         " zero; a zero threshold can never be met.\n");
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  // The environment is read before the flags so a flag can override it. This
  // matters because the agents are started by launchd, where there is no
  // command line to edit -- OCTOMANCER_RADIO and OCTOMANCER_DONGLE are how an
  // unattended agent is pointed at the dongle.
  {
    std::string env_err;
    if (!octo::radio_options_from_env(&env_err)) {
      std::fprintf(stderr, "octomancer-sync: %s\n", env_err.c_str());
      return 2;
    }
  }
  if (!parse_args(argc, argv, &opt)) return 2;

  if (opt.mode == Mode::kPacket) {
    const octo::bmd::Civil when = octo::bmd::utc_civil(octo::wall_now());
    std::printf("%04d-%02d-%02d %02d:%02d:%02d UTC\n  %s\n", when.year,
                when.month, when.day, when.hour, when.minute, when.second,
                octo::bmd::to_hex(octo::bmd::rtc_packet(when, 0)).c_str());
    return 0;
  }

  ::signal(SIGINT, on_signal);
  ::signal(SIGTERM, on_signal);
  ::signal(SIGPIPE, SIG_IGN);

  std::unique_ptr<octo::CameraLink> link = octo::make_camera_link();
  if (!link) {
    std::fprintf(stderr, "octomancer-sync: no CoreBluetooth on this host\n");
    return 1;
  }
  std::string err;
  if (!link->ready(10.0, &err)) {
    std::fprintf(stderr, "octomancer-sync: %s\n", err.c_str());
    return 1;
  }

  octo::SyncState state;
  state.rtc_bias = opt.sync.rtc_bias;

  // A database that will not open is not worth refusing to run over: the
  // daemon still syncs, it just re-learns what it already knew. Say so and
  // carry on, rather than leaving a camera unsynced over a permissions
  // problem in a cache directory.
  // The database has exactly one writer by design, and the daemon is usually
  // it. A probe run started by hand next to a running daemon is a second
  // writer -- harmless for an appended line, but two processes compacting at
  // once rename over each other and one of them loses the history. The modes
  // that only look at a camera have nothing worth recording anyway, so they
  // leave the file alone unless a path was asked for explicitly.
  //
  // --dry-run belongs in that set too. A run that cannot write to a camera
  // has nothing worth recording about one, and dry runs are exactly what
  // somebody does next to a live daemon to see what it would do.
  const bool read_only_mode = opt.mode == Mode::kScanOnly ||
                              opt.mode == Mode::kWatch ||
                              opt.mode == Mode::kPoke ||
                              opt.sync.dry_run;
  if (read_only_mode && !opt.camdb_explicit) opt.camdb_path.clear();

  // One writer. The same set of modes that have nothing worth recording are
  // the ones meant to be run next to a live daemon, so they are also the ones
  // that do not take the lock -- what is being made exclusive is changing a
  // camera and keeping the notebook, not looking at either.
  octo::ProcLock lock;
  if (!read_only_mode && !opt.lock_path.empty()) {
    long holder = 0;
    std::string lock_err;
    if (!lock.acquire(opt.lock_path, &holder, &lock_err)) {
      std::fprintf(stderr, "octomancer-sync: %s\n", lock_err.c_str());
      if (holder > 0) {
        std::fprintf(stderr,
                     "  Two of these connect to the same camera and share one"
                     " database, and neither\n"
                     "  looks broken while they disagree. Stop that one first,"
                     " or use --dry-run\n"
                     "  to watch what it would do without touching anything.\n"
                     "  Lock: %s\n", opt.lock_path.c_str());
      }
      return 1;
    }
  }

  octo::CamDb db;
  if (!db.open(opt.camdb_path, opt.camdb, &err)) {
    std::fprintf(stderr, "octomancer-sync: %s -- continuing without it\n",
                 err.c_str());
  }

  if (opt.mode == Mode::kScanOnly) return mode_scan_only(link.get(), opt);
  if (opt.mode == Mode::kWatch) return mode_watch(link.get(), &state, opt, &db);
  if (opt.mode == Mode::kPoke) return mode_poke(link.get(), &state, opt, &db);
  if (opt.mode == Mode::kRtcTest) {
    return mode_rtc_test(link.get(), &state, opt, &db);
  }

  octo::ConsoleLog console;
  if (!console.open(opt.console_path, opt.rotation, &err)) {
    std::fprintf(stderr, "octomancer-sync: %s\n", err.c_str());
    return 1;
  }

  octo::JsonLog log;
  log.set_rotation(opt.rotation);
  if (!log.open(opt.log_path, &err)) {
    std::fprintf(stderr, "octomancer-sync: %s\n", err.c_str());
    return 1;
  }

  const std::string tol_desc =
      opt.sync.has_tolerance
          ? fmt("%.3fs", opt.sync.tolerance)
          : fmt("%g frame%s", opt.sync.tolerance_frames,
                opt.sync.tolerance_frames == 1.0 ? "" : "s");
  const std::string poll_desc =
      opt.sync.adaptive_poll
          ? fmt("poll every %.0fs to %s, by measured drift", opt.sync.poll,
                octo::format_span(opt.sync.max_poll).c_str())
          : fmt("poll every %.0fs", opt.sync.poll);
  say("octomancer sync starting -- %s, tolerance %s, write at most once per"
      " %s, %s",
      poll_desc.c_str(), tol_desc.c_str(),
      octo::format_span(opt.sync.min_write_interval).c_str(),
      opt.sync.dry_run ? "DRY RUN" : "will write");

  if (db.enabled()) {
    say("remembering per-camera settings in %s (%zu %s on record)",
        db.path().c_str(), db.cameras().size(),
        db.cameras().size() == 1 ? "body" : "bodies");
  } else if (!opt.camdb_path.empty()) {
    say("not remembering per-camera settings -- %s could not be opened",
        opt.camdb_path.c_str());
  }

  Record start;
  start.str("camera_db", opt.camdb_path);
  start.num("poll_s", opt.sync.poll, 1);
  start.num("max_poll_s", opt.sync.max_poll, 1);
  start.boolean("adaptive_poll", opt.sync.adaptive_poll);
  if (opt.sync.has_tolerance) start.num("tolerance_s", opt.sync.tolerance, 6);
  start.num("tolerance_frames", opt.sync.tolerance_frames, 3);
  start.num("write_tolerance_s", opt.sync.write_tolerance, 3);
  start.num("min_write_interval_s", opt.sync.min_write_interval, 1);
  start.integer("rtc_bias_s", opt.sync.rtc_bias);
  start.boolean("dry_run", opt.sync.dry_run);
  start.str("source", opt.source == Source::kMac ? "mac" : "tentacle");
  log.record("start", start.fields());

  // The control socket, and the thread that answers it.
  //
  // A thread rather than a slice of the main loop, because the main loop
  // spends whole seconds inside a blocking write and a control socket that
  // goes deaf for the duration of every cycle is not a control socket. All
  // shared state goes through Control, which holds the only lock in this
  // program.
  // Permission, read from a file this program never writes. A parse error is
  // fatal rather than ignored: carrying on would mean acting on defaults --
  // and the defaults permit nothing -- while a person is looking at a file
  // that says otherwise.
  octo::CamConf conf;
  {
    std::string cerr_msg;
    if (!conf.load(opt.camconf_path, &cerr_msg)) {
      std::fprintf(stderr, "octomancer-sync: %s\n", cerr_msg.c_str());
      return 1;
    }
  }

  const double started_wall = octo::wall_now();
  octo::Control control;
  auto publish_daemon = [&] {
    octo::DaemonStatus ds;
    ds.version = OCTO_VERSION;
    ds.started_wall = started_wall;
    ds.poll_s = opt.sync.poll;
    ds.dry_run = opt.sync.dry_run;
    ds.socket_path = opt.control_path;
    ds.config_path = conf.path();
    ds.any_writes_enabled = conf.any_writes_enabled();
    control.set_daemon(ds);
  };
  publish_daemon();

  // Said out loud, because the alternative is a daemon that looks like it is
  // working and is deliberately doing nothing. This is the expected state on a
  // fresh install: no camera is enabled until somebody enables one.
  if (!conf.any_writes_enabled()) {
    say("NOTHING WILL BE SYNCED: no camera has writes enabled.");
    say("  Enable one with `octomancer writes on --camera <id>`, or in"
        " Octomancer.app.");
    say("  Configuration: %s%s", conf.path().c_str(),
        conf.file_exists() ? "" : " (does not exist yet)");
  } else {
    say("camera configuration: %s", conf.path().c_str());
  }

  std::unique_ptr<octo::Server> control_server;
  std::thread control_thread;
  std::atomic<bool> control_running{false};
  if (opt.serve_control && !opt.control_path.empty()) {
    control_server.reset(new octo::Server(
        [&control](const std::string& line) { return control.handle(line); },
        opt.control_path));
    std::string cerr_msg;
    if (!control_server->start(&cerr_msg)) {
      // Not fatal. Syncing a camera is the job; being asked about it is a
      // convenience, and losing the socket to a stale file or a second daemon
      // is no reason to leave a clock wrong all night.
      say("control socket unavailable (%s) -- carrying on without it",
          cerr_msg.c_str());
      control_server.reset();
    } else {
      control_running = true;
      say("control socket: %s", opt.control_path.c_str());
      control_thread = std::thread([&control_server, &control_running] {
        while (control_running.load()) control_server->serve(200);
      });
    }
  }

  // Publish whatever a cycle learned, and turn the interesting parts of it
  // into events. Everything a client can see about a camera comes through
  // here.
  auto publish = [&](Errand* errand, const Presence& seen) {
    if (errand->bench.has) control.set_bench(errand->bench);
    if (!errand->have_status) return;
    octo::CameraStatus st = errand->status;
    if (st.name.empty()) st.name = seen.name;
    if (state.lead.has) {
      st.has_lead = true;
      st.lead_s = state.lead.lead_s;
    }
    if (state.drift.has) {
      st.has_drift = true;
      st.drift_ppm = state.drift.ppm;
    }
    st.connected = false;  // the cycle has let go of the camera by now
    control.publish_camera(st);
  };

  // Which cameras have been corrected at least once since this daemon
  // started. "For the first time" is a claim about this session, not about the
  // body's whole history -- the database would know the latter, but someone
  // watching a screen means the former.
  std::set<std::string> synced_once;

  auto run_errand = [&](Errand* errand, const Presence& seen) {
    octo::PollPlan p;
    run_cycle(link.get(), &state, opt, &log, &db, conf, &p, errand);
    publish(errand, seen);

    const std::string& id = errand->status.id;
    if (errand->have_status && !id.empty()) {
      if (errand->status.has_last_write) {
        if (errand->ok && synced_once.insert(id).second) {
          control.emit(octo::EventKind::kFirstSync, id, errand->status.name,
                       errand->outcome);
        }
        if (!errand->ok) {
          control.emit(octo::EventKind::kSyncFailed, id, errand->status.name,
                       errand->outcome);
        }
      } else if (!errand->ok && !errand->outcome.empty() &&
                 errand->status.action != "skip:rate-limited" &&
                 errand->status.action != "skip:in-tolerance") {
        // A gate that means "cannot" is worth telling someone about; one that
        // means "no need" is the daemon working correctly.
        control.emit(octo::EventKind::kSyncFailed, id, errand->status.name,
                     errand->outcome);
      }
    }
    return p;
  };

  // The camera comes and goes, and the two ways of finding that out cost
  // wildly different amounts. Asking octomancerd is a socket read; scanning
  // for it is twenty seconds of radio. So the loop below asks the cheap
  // question often and the expensive one only when it has a reason to.
  Presence last = read_presence(opt);
  if (last.known) {
    say("octomancerd is watching for the camera -- %s%s%s",
        last.present ? "on the air now" : "not on the air",
        last.name.empty() ? "" : ", ", last.name.c_str());
  } else if (opt.use_daemon) {
    say("no camera watch from octomancerd -- scanning for the camera each"
        " cycle instead");
  }

  double next_cycle = octo::mono_now();  // the first one runs straight away
  // Even with a presence signal, look properly now and then. The daemon can be
  // wrong about a camera -- one that is connected to another app stops
  // advertising -- and a whole night on a wrong answer is worth avoiding for
  // the price of one scan a quarter of an hour.
  double next_blind_check = 0.0;

  while (!g_stop) {
    console.maybe_rotate();

    // Somebody edited the file and said so. Done here, between cycles, rather
    // than on the socket thread: the values are consulted halfway through
    // deciding things, and swapping them out underneath that would make a
    // cycle act on two different configurations.
    if (control.take_reload()) {
      std::string cerr_msg;
      if (!conf.reload(&cerr_msg)) {
        // The old values stay in force. Refusing to act on a file we could
        // not read beats falling back to defaults that permit nothing and
        // silently stopping.
        say("configuration NOT reloaded: %s", cerr_msg.c_str());
        say("  carrying on with what was loaded before.");
      } else {
        say("configuration reloaded from %s -- %s", conf.path().c_str(),
            conf.any_writes_enabled() ? "writes enabled for at least one camera"
                                      : "NO camera has writes enabled");
        publish_daemon();
        // Every camera already published is carrying the permission it had
        // when it was last talked to. Restate it now, or enabling a camera
        // reads as having done nothing until the next cycle -- which, on a
        // camera that is already in step, is a quarter of an hour away.
        for (const std::string& id : control.camera_ids()) {
          control.set_writes_enabled(id, conf.writes_enabled(id));
        }
      }
    }

    const double now = octo::mono_now();
    const Presence cam = read_presence(opt);

    const bool came_up = cam.known && cam.present &&
                         (!last.known || !last.present ||
                          cam.sessions != last.sessions);
    if (came_up) {
      say("camera came up%s%s -- syncing now",
          cam.name.empty() ? "" : " -- ", cam.name.c_str());
      Record up;
      up.str("state", "up");
      up.str("camera_id", cam.id);
      up.str("camera_name", cam.name);
      up.integer("sessions", static_cast<long long>(cam.sessions));
      log.record("camera", up.fields());
      // Almost every way a camera goes off the air and comes back involves its
      // clock being reset, so nothing measured before this point can be
      // trusted to describe what is running now.
      octo::forget_drift(&state);
      next_cycle = now;
    } else if (cam.known && last.known && last.present && !cam.present) {
      // Expected after every cycle that connects -- a camera stops
      // advertising while something is talking to it -- so this is a log line,
      // not a cause for alarm.
      Record down;
      down.str("state", "down");
      down.str("camera_id", cam.id);
      log.record("camera", down.fields());
      control.set_present(cam.id, false);
      control.emit(octo::EventKind::kCameraLost, cam.id, cam.name,
                   "the camera is no longer on the air");
    }
    last = cam;

    // Anything asked for over the socket comes first: someone is waiting on
    // it, and the schedule is not.
    {
      octo::Request req;
      while (!g_stop && control.take_request(&req)) {
        // A request naming several cameras is several errands. One radio means
        // they run one after another, which is also the order they were asked
        // for.
        std::vector<std::string> targets = req.cameras;
        if (targets.empty()) targets.push_back(std::string());

        bool all_ok = true;
        std::string summary;
        for (const std::string& target : targets) {
          Errand errand;
          errand.camera = target;
          errand.force_sync = req.kind == octo::RequestKind::kSync;
          errand.set_source = req.kind == octo::RequestKind::kSetSource;
          errand.source_value = req.source;
          say("%s requested%s%s", octo::request_kind_name(req.kind),
              target.empty() ? "" : " for ", target.c_str());

          const octo::PollPlan p = run_errand(&errand, cam);
          if (!errand.ok) all_ok = false;
          if (!summary.empty()) summary += "; ";
          const std::string who = errand.status.name.empty()
                                      ? (target.empty() ? std::string("camera")
                                                        : target)
                                      : errand.status.name;
          summary += who + ": " + errand.outcome;
          next_cycle = octo::mono_now() + p.seconds;
        }
        control.finish(req.id, all_ok, summary);
      }
      // A request taken but abandoned -- by a signal arriving mid-drain --
      // would otherwise sit in `running` forever, with a client polling it.
      if (g_stop) control.requeue_running();
    }

    if (now >= next_cycle) {
      const bool blind_due = now >= next_blind_check;
      if (!cam.known || cam.present || blind_due) {
        if (cam.known && !cam.present) {
          // The presence signal can be wrong -- a camera connected to another
          // app stops advertising -- so it is checked directly now and then
          // rather than trusted for a whole night.
          say("octomancerd has not heard the camera; looking directly anyway");
        }
        next_blind_check = now + opt.sync.max_poll;
        Errand errand;
        const octo::PollPlan plan = run_errand(&errand, cam);
        if (opt.once) break;
        next_cycle = octo::mono_now() + plan.seconds;
      } else {
        // Nothing on the air to connect to. The presence check at the top of
        // the loop is what will notice when that changes.
        next_cycle = now + opt.presence_poll;
      }
    }

    // Sleep in slices so a signal is noticed promptly rather than a quarter of
    // an hour later: an operator who hits Ctrl-C expects the radio released
    // now. The wait is capped at the presence interval so a camera coming up
    // is never sat on for longer than that.
    double remain = next_cycle - octo::mono_now();
    if (opt.use_daemon && remain > opt.presence_poll) remain = opt.presence_poll;
    for (double slept = 0.0; slept < remain && !g_stop; slept += 0.25) {
      // A reload is somebody waiting at a terminal for an answer, so it is
      // worth cutting the wait short for. Taking it is still the loop's job,
      // at the top, where nothing is halfway through being decided.
      if (control.reload_pending()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }

  say("stopping");
  if (control_thread.joinable()) {
    control_running = false;
    control_thread.join();
  }
  control_server.reset();
  log.record("stop", "");
  log.close();
  return 0;
}
