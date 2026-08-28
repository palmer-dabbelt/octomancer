// The camera daemon's control surface: what a client may ask for, what it gets
// back, and the queue in between.
//
// None of this touches a radio, on the same reasoning as camsync.h. An action
// a client asks for cannot be carried out on the socket thread: connecting to
// a camera and waiting for a write to verify takes seconds, and the loop that
// owns the camera is usually in the middle of doing exactly that. So a request
// is parked here, the loop collects it when it comes round, and the client
// asks again for the answer. That leaves the interesting parts -- parsing,
// queueing, expiry, rendering -- testable with no camera in the room.
//
// Two threads meet in this file and nowhere else. The daemon loop publishes
// what it has just seen and drains the queue; the socket thread parses
// requests and renders replies. Control is the lock between them, and every
// public method takes it. The free functions below it are pure and take no
// lock, which is what the tests exercise.
//
// The wire format is proto.h's: a version banner, escaped key=value lines, and
// `end`. Readers ignore keys they do not know, so the daemon can grow fields
// without breaking a UI built against an older version.
#ifndef OCTO_CONTROL_H
#define OCTO_CONTROL_H

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace octo {

// ----------------------------------------------------------------- requests

enum class RequestKind {
  kSync,        // correct this camera's clock now, whatever the gates think
  kSetSource,   // write 4.7
};

const char* request_kind_name(RequestKind k);

struct Request {
  int64_t id = 0;
  RequestKind kind = RequestKind::kSync;
  // Empty means every camera the daemon knows about. A name or an id both
  // resolve here; the daemon does the matching, because it is the side that
  // knows what it can see.
  std::vector<std::string> cameras;
  int64_t source = 0;  // kSetSource only
  double queued_wall = 0.0;
};

enum class RequestState {
  kQueued,    // waiting for the loop to come round
  kRunning,   // the loop has it
  kDone,
  kFailed,
  kUnknown,   // never existed, or aged out of the ring
};

const char* request_state_name(RequestState s);
bool request_finished(RequestState s);

struct RequestResult {
  int64_t id = 0;
  RequestState state = RequestState::kUnknown;
  std::string message;
  double updated_wall = 0.0;
};

// ------------------------------------------------------------------- events
//
// The three things worth interrupting someone for. The daemon emits all of
// them unconditionally and a client decides which it cares about -- which of
// these deserves a notification is a preference belonging to whoever is
// looking at the screen, not a setting the daemon should be holding on their
// behalf.

enum class EventKind {
  kSyncFailed,   // a write was attempted and did not take
  kFirstSync,    // this camera has been corrected for the first time this session
  kCameraLost,   // it was on the air, and now it is not
};

const char* event_kind_name(EventKind k);
// Parses what event_kind_name produced. False on anything else.
bool event_kind_from_name(const std::string& name, EventKind* out);

struct Event {
  int64_t seq = 0;
  EventKind kind = EventKind::kSyncFailed;
  std::string camera_id;
  std::string camera_name;
  std::string message;
  double wall = 0.0;
};

// -------------------------------------------------------------------- state

struct CameraStatus {
  std::string id;
  std::string name;

  bool present = false;    // seen on the air recently
  bool connected = false;  // a link is open right now

  // When this camera was last heard advertising, as an absolute wall clock.
  // Absolute rather than an age because a client polls on its own schedule
  // and an age computed by the daemon is stale by the time it is drawn.
  bool has_last_seen = false;
  double last_seen_wall = 0.0;

  // Signal strength at the last advertisement. Worth carrying because a
  // camera that is merely far away and one that has been switched off look
  // identical otherwise.
  bool has_rssi = false;
  int rssi = 0;

  // How many times this camera has come back since the daemon started. A
  // client that remembers the previous number can tell a power cycle from a
  // camera that never left.
  int sessions = 0;

  // Whether the configuration permits changing anything on this camera. Not a
  // property of the camera: see camconf.h.
  bool writes_enabled = false;

  bool has_error = false;
  double error_s = 0.0;    // camera minus bench, the sign the logs use

  std::string timecode;
  bool has_fps = false;
  int fps = 0;
  bool recording = false;

  // 4.7. Absent means the camera has not said, which is not the same as
  // time-of-day; a client should render the difference.
  bool has_source = false;
  int64_t source = 0;

  // The gate that stopped the last cycle, or "write" if it did not stop.
  std::string action;

  bool has_last_write = false;
  double last_write_wall = 0.0;
  int writes = 0;

  bool has_lead = false;
  double lead_s = 0.0;
  bool has_drift = false;
  double drift_ppm = 0.0;
};

struct BenchStatus {
  bool has = false;
  std::string source;   // "tentacle" or "mac"
  int boxes = 0;
  double offset_s = 0.0;
  double spread_s = 0.0;
  bool daemon_reachable = false;
};

struct DaemonStatus {
  std::string version;
  double started_wall = 0.0;
  double now_wall = 0.0;
  double poll_s = 0.0;
  bool dry_run = false;
  std::string socket_path;
  // Where the per-camera configuration was read from, and whether anything in
  // it permits a write at all. A daemon with nothing enabled is running
  // correctly and doing nothing, which is worth being able to see.
  std::string config_path;
  bool any_writes_enabled = false;
};

struct Status {
  DaemonStatus daemon;
  BenchStatus bench;
  std::vector<CameraStatus> cameras;
  int queued = 0;
};

// ------------------------------------------------------------------ parsing

struct Command {
  std::string verb;
  std::vector<std::string> cameras;
  bool has_value = false;
  int64_t value = 0;
  bool has_id = false;
  int64_t id = 0;
  bool has_since = false;
  int64_t since = 0;
  bool ok = true;
  std::string error;
};

// `verb key=value key=value`, values escaped as proto.h describes. Repeating
// camera= adds another camera rather than replacing the first, so one request
// can name several.
Command parse_command(const std::string& line);

// ---------------------------------------------------------------- rendering

std::string render_status(const Status& s);
std::string render_status_json(const Status& s);
std::string render_result(const RequestResult& r);
std::string render_events(const std::vector<Event>& events, int64_t next_seq);
std::string render_error(const std::string& message);

// Parse what render_status produced.
bool parse_status(const std::string& text, Status* out, std::string* err);
bool parse_result(const std::string& text, RequestResult* out, std::string* err);
bool parse_events(const std::string& text, std::vector<Event>* out,
                  int64_t* next_seq, std::string* err);

// ------------------------------------------------------------------ Control

class Control {
 public:
  Control();

  // --- the socket thread ---------------------------------------------
  //
  // One line in, a whole reply out. Everything a client can reach goes
  // through here.
  std::string handle(const std::string& line);

  // --- the daemon loop -----------------------------------------------

  void set_daemon(const DaemonStatus& d);
  void set_bench(const BenchStatus& b);

  // Replace what is known about one camera, matched on id. Cameras are never
  // removed, only marked absent: a camera that was here an hour ago is still
  // worth listing, and dropping it would make `list-cameras` flicker with the
  // advertisement duty cycle.
  void publish_camera(const CameraStatus& cam);
  void set_present(const std::string& id, bool present);

  // Permission is the one part of a camera's published state that does not
  // come from having just talked to it, so it has to be settable without a
  // cycle: a camera enabled at midnight should not read as disabled until the
  // next connection, a quarter of an hour later.
  void set_writes_enabled(const std::string& id, bool enabled);
  std::vector<std::string> camera_ids() const;

  // Take the oldest queued request, marking it running. False if none.
  bool take_request(Request* out);
  void finish(int64_t id, bool ok, const std::string& message);

  // Requeue everything running as queued again, for a loop that is giving up
  // on this pass -- a camera that could not be connected to, say. Without this
  // a request taken and then abandoned would sit in kRunning forever.
  void requeue_running();

  void emit(EventKind kind, const std::string& camera_id,
            const std::string& camera_name, const std::string& message);

  // Whether somebody has asked for the configuration to be re-read since this
  // was last called. A flag rather than a callback: the file is the daemon's
  // to read, on the daemon's thread, at a moment when it is not halfway
  // through deciding something with the old values.
  bool take_reload();

  // Whether one is waiting, without taking it. The loop sleeps in slices and
  // uses this to stop sleeping early, so a reload is acted on in a quarter of
  // a second rather than whenever the next cycle happened to be due.
  bool reload_pending() const;

  // Camera ids somebody has asked to be forgotten since this was last called.
  //
  // The row is dropped from the published status the moment the command
  // arrives -- that half is this object's own state and a page asking to
  // remove a device should not watch it sit there afterwards. What is on disk
  // is the daemon's, written on the daemon's thread, so it is handed over
  // here instead of being reached for from the socket.
  std::vector<std::string> take_forgotten();
  bool forget_pending() const;

  // Queue from inside the daemon rather than from a socket, which is how the
  // command line's one-shot modes reach the same code path.
  int64_t queue(const Request& req);

  int queued_count() const;

 private:
  struct Entry {
    Request req;
    RequestResult result;
  };

  // Bounded: a client that queues and never collects must not grow this
  // without limit, and a result nobody has asked for in a hundred requests is
  // not going to be asked for.
  static constexpr size_t kMaxHistory = 128;
  static constexpr size_t kMaxEvents = 256;

  std::string handle_locked(const Command& cmd);
  Status snapshot_locked() const;
  Entry* find_locked(int64_t id);

  mutable std::mutex mu_;
  DaemonStatus daemon_;
  BenchStatus bench_;
  std::vector<CameraStatus> cameras_;
  std::deque<Entry> history_;
  std::deque<Event> events_;
  int64_t next_request_id_ = 1;
  int64_t next_event_seq_ = 1;
  bool reload_requested_ = false;
  std::vector<std::string> forget_requested_;
};

// Where the camera daemon listens. Deliberately a different socket from
// octomancerd's: they are different programs with different privileges, and
// the one that can write to a camera should be separately reachable and
// separately deniable.
std::string default_control_socket_path();

}  // namespace octo

#endif  // OCTO_CONTROL_H
