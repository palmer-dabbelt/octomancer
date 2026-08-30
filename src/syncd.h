// The sync daemon: the tight loop, with nothing in it that waits.
//
// This is what doc/box-notes.md calls the sync daemon, and the point of it is
// that there is one of it. The same object runs as a process on this Mac and
// as firmware on the Nordic; it drives a radio, hears Tentacle boxes, decides
// whether a camera's clock needs setting, sets it, and answers whoever is
// asking over the control protocol. It drives one rather than owning one on
// purpose: the loop, the roster, the camera and the configuration are all
// handed to it, and the host program is what constructs them -- which is what
// lets the whole thing be driven by a fake camera on a fake clock with no
// radio in the building. See tests/test_syncd.cc. A design where those were separate
// programs could not run on a box with one radio and one loop at all, and
// running a different architecture on the Mac would mean the interesting code
// existed twice and was debugged once.
//
// Nothing here touches a radio, a socket, a file or a clock it was not handed.
// What it touches instead:
//
//   * src/loop.h for time and for scheduling. Never mono_now(): a test drives
//     an hour of drift by moving a variable, and a daemon that read the real
//     clock behind the loop's back would make that a lie.
//   * src/camasync.h for the camera, so a test can answer as a camera would.
//   * src/registry.h for the roster, fed from outside by whatever heard it.
//   * src/camsync.h for every decision about whether to write.
//   * src/boxmsg.h for the control protocol, one line per message.
//
// So the whole of the interesting behaviour is exercisable with no radio, no
// camera and no wall-clock time, which is the argument doc/box-notes.md makes
// at length and the reason tests/test_syncd.cc can pin a write landing on a
// second boundary without a second passing.
//
// **Everything is one thread.** Adverts arrive through src/scanbridge.h,
// camera completions arrive on the loop, control lines arrive on the loop. No
// method here may be called from anywhere else.
//
// ## The shape of a cycle
//
// A cycle is the old octomancer-sync run_cycle() with its waits turned into
// states. It is worth listing them, because the states are exactly the places
// the blocking version used to sleep:
//
//     idle --> bench --> connect --> subscribe --> observe --> decide
//                                                                |
//                          finish <-- verify <-- write <-- align +
//
// `align` is the one that looks like a wait and is not: the RTC field holds
// whole seconds, so the write is timed to land on a second boundary, and the
// daemon arms a timer for that instant rather than sleeping until it.
//
// A cycle can end at any state. Every ending goes through finish_cycle(),
// which is the only place that reports an outcome, releases the camera and
// schedules the next look -- so a cycle that gives up early cannot leave the
// daemon without a next cycle, which is the failure that shows up as a clock
// nobody corrected all night.
//
// ## What stops a stale answer being believed
//
// doc/box-notes.md lists four ways this model goes wrong, and the one that
// applies hardest here is a completion arriving after the work it belonged to
// was abandoned -- the camera went away, or somebody asked for a sync while a
// cycle was in flight. Every completion the daemon hands out therefore carries
// two things by value: a liveness flag, so a completion that outlives the
// daemon does nothing rather than writing into freed memory, and the cycle
// number it was issued for, so a completion from a cycle that has been
// abandoned is dropped rather than allowed to advance the current one.
#ifndef OCTO_SYNCD_H
#define OCTO_SYNCD_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "bmd.h"
#include "boxmsg.h"
#include "camasync.h"
#include "camconf.h"
#include "camsync.h"
#include "loop.h"
#include "registry.h"
#include "scanner.h"

namespace octo {

// Somewhere to send one line of the box protocol.
//
// A pointer to one of these is the peer's identity, which is why there is no
// id: a unix socket client, a USB CDC console and a GATT characteristic are
// three different objects and none of them has a number the others would
// recognise. src/boxsock.h implements it over a socket.
class MsgPeer {
 public:
  virtual ~MsgPeer() = default;
  virtual void send(const std::string& line) = 0;
};

// The bench this cycle will sync against: the median across the timecode boxes
// that are live, decoded, and not switched off in the configuration.
//
// The median is computed here rather than taken from Snapshot::bench_offset,
// which is across every live box heard. Listening is passive and costs
// nothing, so the registry listens to all of them; what a person has dismissed
// in cameras.conf is a different question and belongs to whoever is about to
// act on the answer. If the two disagree, the page showing the first is lying:
// the number it displays is not the number anything acted on.
struct BenchView {
  bool ok = false;
  double offset = 0.0;
  double spread = 0.0;
  int boxes = 0;
  // Heard and then ignored, because somebody switched them off. Kept as a
  // number rather than dropped silently: "no boxes to sync to" and "the only
  // box in the room is disabled" are different situations, and a person
  // reading the log should not have to guess which one they are in.
  int skipped = 0;
};

BenchView measure_bench(const Snapshot& snap, const CamConf* conf);

struct SyncdOptions {
  SyncOptions sync;

  // Whether the Tentacle bench is the reference. False means this host's own
  // clock is, which is what a bench with no boxes in it is reduced to and what
  // --source=mac asks for deliberately.
  bool tentacle_bench = true;

  // Keep the camera connected between cycles. Timecode only arrives over a
  // live connection, so a daemon that holds one for twenty seconds an hour is
  // blind for the remaining fifty-nine minutes and every cycle opens by
  // waiting for a reading it could already have had.
  bool hold = true;

  // Which camera to follow. Empty means whichever one is found, which is the
  // right answer on a rig with one camera and the wrong one on a rig with two.
  std::string camera;

  // Whether a camera nobody has named may be written to, when there is no
  // cameras.conf to ask. The file is the answer on the Mac and it defaults to
  // off for a reason src/camconf.h argues at length; the box has no
  // filesystem, so until permission arrives over the control protocol it is
  // this, with the same default the file has.
  bool default_writes = false;

  // How often to volunteer a bench line to whoever is listening. This is also
  // the heartbeat that proves the link is alive to a console watching over a
  // serial cable, which is why it is not gated on anything having changed.
  double announce_period = 30.0;
  bool announce = true;
};

// What a cycle did, in the form a log or a UI wants it.
//
// A struct rather than a formatted line because there are three consumers with
// three formats -- the JSONL log on the Mac, the control protocol, and the
// console -- and rendering it three times from one struct is better than
// parsing one rendering back into the other two.
struct CycleReport {
  std::string action;   // camsync's action name, or a skip: / write: reason
  std::string message;  // one line for a person
  bool reached_camera = false;
  bool wrote = false;
  bool verified = false;

  BenchView bench;
  bool has_error = false;
  double error_before = 0.0;
  bool has_error_after = false;
  double error_after = 0.0;

  std::string camera_id;
  std::string timecode;
  int fps = 0;
  bool recording = false;
  bool has_timecode_source = false;
  int64_t timecode_source = 0;

  // Whether the residual after the write is a fair measurement of the apply
  // delay. A write that missed by more than half a second missed because the
  // whole-second bias was wrong, and feeding that into a sub-second lead has
  // it chasing a whole second it can never reach.
  bool timing_usable = false;
  bool has_write = false;
  bmd::Civil wrote_value;
  int bias = 0;
  double lead = 0.0;
  double write_latency = 0.0;

  double next_poll = 0.0;
  std::string next_poll_reason;
};

class SyncDaemon {
 public:
  // `loop` and `registry` outlive the daemon. `registry` is fed from outside:
  // whatever owns the radio calls observe_advert, because on the Mac that is a
  // bridge from another thread and on the box it is the HCI scanner, and the
  // daemon should not have to know which.
  SyncDaemon(Loop* loop, Registry* registry, SyncdOptions opt);
  ~SyncDaemon();

  SyncDaemon(const SyncDaemon&) = delete;
  SyncDaemon& operator=(const SyncDaemon&) = delete;

  // Null is a supported state and not a degraded one: a daemon with no camera
  // still hears boxes, still serves the roster, and still says why it is not
  // syncing anything. That is what runs on this Mac today, where the only
  // async camera backend is the dongle's.
  void set_camera(AsyncCamera* camera);

  // Read, never written -- see src/camconf.h. Null means every default, which
  // is writes off for cameras and enabled on for boxes.
  void set_config(const CamConf* conf);

  // Wall-clock seconds since the epoch. Injectable because a test needs to
  // control it and because the box has no wall clock at boot; the default is
  // wall_now(). Monotonic time always comes from the loop.
  void set_wall_clock(std::function<double()> wall);

  // Seed the learned figures from wherever they were kept between runs. The
  // bias costs one of the rationed writes to acquire and the lead needs
  // several, so a restart that threw them away would throw away a night.
  void set_state(const SyncState& state);
  const SyncState& state() const { return state_; }

  // Both of these hand over a SyncState to be modified, and that is
  // deliberate rather than sloppy. The camera database -- learned bias,
  // measured apply delay, write history -- is a file, so it lives on the host
  // and cannot live on the box; but what it knows has to reach the state this
  // daemon schedules against. Binding to a body and finishing a cycle are the
  // two moments when nothing is in flight and the state can be changed
  // safely, so they are the two places it is offered.
  using CycleHandler =
      std::function<void(const CycleReport& report, SyncState* state)>;
  using BindHandler = std::function<void(const std::string& id,
                                         const std::string& name,
                                         SyncState* state)>;
  using SayHandler = std::function<void(const std::string& line)>;
  // Called at the end of every cycle, for whoever is keeping the log. There is
  // no log on the box; there is one on the Mac and it is the scientific value
  // of the whole project, which is why this is a handler and not a file.
  void on_cycle(CycleHandler handler);
  // Called when the daemon binds to a camera it was not bound to before, which
  // is when a host with a database should recall what it knows about that
  // body.
  void on_bind(BindHandler handler);
  // Console chatter, for a person watching. Never load-bearing.
  void on_say(SayHandler handler);

  // Being told what time it is.
  //
  // Only a host with no clock of its own installs this. A Mac knows the time
  // and a `time` message to one is a mistake worth reporting rather than
  // obeying -- so a daemon with no handler answers `err reason=have-clock`
  // instead of silently accepting a correction it would never apply.
  //
  // The box is the other case: no network, no battery-backed RTC, nobody to
  // ask. Its offsets are measured against a wall clock it can only be given,
  // which is why this is part of the protocol and not a build option. See
  // firmware/src/boxclock.h.
  using TimeHandler = std::function<void(double wall)>;
  void on_settime(TimeHandler handler);

  void start();
  void stop();
  bool started() const { return started_; }

  // --- what the radio heard, on the loop's thread -------------------------
  void observe_advert(const Advert& advert);
  void observe_camera(const Sighting& sighting);
  void set_radio_state(const std::string& state);

  // --- the control protocol ----------------------------------------------
  //
  // A peer is opened, told things, and closed. Opening one sends the greeting;
  // everything else is a reply to a line or an unsolicited announcement.
  void peer_opened(MsgPeer* peer);
  void peer_closed(MsgPeer* peer);
  void peer_line(MsgPeer* peer, const std::string& line);

  // The status line, which is also what `status` returns. Public because the
  // console and the tests both want it without a peer to send it to.
  Message status_message() const;

  // Ask for a cycle now. `force` overrules the gates that mean "there is no
  // need" and none of the ones that mean "must not" -- see gate_is_advisory.
  void request_sync(const std::string& camera, bool force);
  // Ask for 4.7 to be written, which is the one errand that does not care what
  // the clock says.
  void request_source(const std::string& camera, int64_t value);

  // Whether a cycle is in flight. Exposed because "the daemon is busy" is the
  // honest answer to a request that has been queued rather than run.
  bool busy() const;

 private:
  enum class Phase {
    kStopped,
    kIdle,       // waiting for the next cycle
    kScanning,
    kConnecting,
    kSubscribing,
    kObserving,  // waiting for the camera to volunteer a timecode
    kSourceWrite,
    kAligning,   // waiting for the second boundary to write on
    kWriting,
    kVerifying,  // waiting for a fresh reading after a write
  };

  // Everything one cycle carries. Cleared at the start of each, so a value
  // left over from the last one cannot be read as this one's.
  struct Cycle {
    uint64_t seq = 0;
    bool forced = false;
    bool set_source = false;
    int64_t source_value = 0;
    std::string want_camera;

    // What is being reported when this cycle ends, filled in as it goes. Every
    // early return then leaves a partial picture rather than none, which is
    // what makes "the camera is here but the bench is not" visible to a client
    // instead of looking like a daemon that stopped knowing things.
    CycleReport report;

    BenchView bench;
    double offset = 0.0;
    bool may_write = false;

    // The camera this cycle is trying, which is not yet the camera it is bound
    // to: binding happens when the connection succeeds.
    std::string try_id;
    std::string try_name;
    bool tried_scan = false;
    bool looked = false;

    int bias = 0;
    double lead = 0.0;
    double write_started = 0.0;
  };

  // --- the cycle, one member per state ------------------------------------
  void begin_cycle();
  void step_connect();
  void step_scan();
  void on_scan(const ScanResult& result);
  void connect_to(const std::string& id, const std::string& name);
  void step_subscribe();
  void step_observe();
  void evaluate();
  void step_source_write();
  void step_align();
  void step_write();
  void step_verify();
  void judge();
  void finish_cycle(const std::string& action, const std::string& message);

  void on_view(const CameraView& view);
  void on_camera_gone();

  // --- scheduling ---------------------------------------------------------
  void schedule_next(double seconds, const char* reason);
  // Cancels whatever this step was waiting for and arms a new deadline. There
  // is at most one: a state machine waiting on two things at once is one that
  // will be advanced twice.
  void arm_step(double seconds, std::function<void()> fn);
  void cancel_step();

  // A completion that knows which cycle issued it. See the header comment.
  AsyncCamera::DoneHandler guard_done(void (SyncDaemon::*fn)(bool,
                                                            const std::string&));
  bool current(uint64_t seq) const;

  void done_connect(bool ok, const std::string& err);
  void done_subscribe(bool ok, const std::string& err);
  void done_write(bool ok, const std::string& err);
  void done_source(bool ok, const std::string& err);

  // --- talking ------------------------------------------------------------
  void say(const std::string& line);
  void send(MsgPeer* peer, const Message& msg);
  void announce(const Message& msg);
  void handle(MsgPeer* peer, const Message& msg);
  void drain_events();
  void tick_announce();
  // Announce the camera appearing or going away, once per change. A held
  // connection counts as present: a camera stops advertising while something
  // is connected to it, so the radio's silence about it is this daemon's own
  // doing and reporting it as absence would be reporting our own behaviour as
  // the camera's.
  void update_camera_presence(const Snapshot& snap);
  Message device_message(const DeviceSnapshot& dev) const;

  double wall() const;
  double mono() const;
  // The reading in the view arrived at some earlier instant and has been
  // sitting there since. Charging the camera for that wait is what made every
  // measurement before it was corrected come out negative.
  double error_from(const CameraView& view, int fps) const;

  Loop* loop_ = nullptr;
  Registry* registry_ = nullptr;
  AsyncCamera* camera_ = nullptr;
  const CamConf* conf_ = nullptr;
  SyncdOptions opt_;
  SyncState state_;
  std::function<double()> wall_;
  CycleHandler on_cycle_;
  BindHandler on_bind_;
  SayHandler on_say_;
  TimeHandler on_time_;

  bool started_ = false;
  Phase phase_ = Phase::kStopped;
  uint64_t next_seq_ = 1;
  Cycle cur_;
  // Set while a cycle is in flight and applied to the next one. A request is
  // never dropped for arriving at a bad moment; it waits.
  bool pending_sync_ = false;
  bool pending_force_ = false;
  bool pending_source_ = false;
  int64_t pending_source_value_ = 0;
  std::string pending_camera_;

  // Consecutive looks that found no camera, which is what the reacquisition
  // backoff is priced in. Reset the moment the camera is seen again, however
  // it is seen: the next disappearance is fresh news and does not inherit the
  // last one's patience.
  int misses_ = 0;

  // What was last announced about the camera being on the air, so that a
  // change is announced once rather than a state being repeated. A held
  // connection counts as present: a camera stops advertising while something
  // is connected to it, so the radio's silence about it is this daemon's own
  // doing.
  bool camera_present_ = false;

  // What the last cycle concluded and when the next one is due, for the
  // status line. A client asking what the daemon is doing is usually asking
  // one of these two things.
  std::string last_action_;
  double next_cycle_at_ = 0.0;

  TimerId cycle_timer_ = kNoTimer;
  TimerId step_timer_ = kNoTimer;
  TimerId announce_timer_ = kNoTimer;

  std::vector<MsgPeer*> peers_;
  std::vector<MsgPeer*> quiet_;  // peers that asked not to be announced to

  // Checked by every completion before it touches anything. A shared_ptr
  // rather than a member flag because the completion may outlive the object
  // that owns the flag.
  std::shared_ptr<bool> alive_;
};

}  // namespace octo

#endif  // OCTO_SYNCD_H
