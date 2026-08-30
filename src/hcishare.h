// One dongle, several jobs.
//
// A Link is a controller, and a controller is a single piece of hardware: one
// scanner, one initiator, one set of event handlers. Everything above it was
// written as though it could have a controller to itself, which was true for
// as long as each program did exactly one thing with the radio. The sync
// daemon does two -- it listens to Tentacle boxes continuously, and it goes
// and connects to a camera -- and so it opened the port twice, once in
// src/scanner_hci.cc and once in src/camera_hci.cc.
//
// That is not refused. A macOS cu.* device hands out a second descriptor
// without complaint, so two HCI hosts ended up reading one byte stream: each
// took the other's command responses for its own, both timed out, and the
// whole thing surfaced as a radio that had powered itself off. It cost an
// evening to find, and the misleading part is that nothing in the failure
// mentions the port. On the box there is no version of this to work around --
// one radio, one daemon, both jobs.
//
// So the ownership moves up. Whoever runs the program opens one Link and hands
// out Users. A User is a subscription, not a link: it can ask for the radio to
// be scanning and receive advertisements while it is, it is told when the
// dongle disappears, and it is destroyed independently of everyone else's.
//
// Three things are genuinely shared and are reconciled here rather than
// fought over:
//
//   * **Scanning.** Reference counted. The radio scans if anybody wants it to,
//     and stops when the last user stops asking. A user that asks while
//     somebody else is already scanning is told "yes" straight away and starts
//     receiving reports; it does not restart the radio.
//
//   * **Active versus passive.** The Tentacle scan is passive on purpose --
//     the clock is in the advertisement, so there is never a reason to
//     announce ourselves by asking for a scan response. The camera scan is
//     active, because a camera keeps its name in the scan response and the
//     name is what a person recognises it by. One controller cannot do both,
//     so the union wins: if any user wants active, the radio scans actively,
//     and the passive user still sees everything it would have seen. The cost
//     is real and worth stating -- while the camera is looking for itself, the
//     box is transmitting scan requests.
//
//   * **Scanning across a connection.** Link::connect stops the scan, because
//     a controller cannot scan and initiate at once, and restores it only if
//     the connection failed. That was right when the only thing that wanted
//     the radio was the thing connecting. It is wrong here: the sync daemon's
//     whole reference clock is the Tentacle broadcast, and going deaf to it
//     for the length of a connect-pair-write is going deaf at exactly the
//     moment the number matters. So connect() is routed through a User, and
//     scanning is put back once the connection is up rather than only when it
//     failed.
//
// Everything else on the controller -- ATT, SMP, advertising, raw commands --
// has exactly one owner in every program that exists today, and is reached
// through User::link(). This does not police that. It is a real limitation
// and the honest place to record it: two things doing ATT on one Link would
// collide, and nothing here would notice.
//
// Nothing in this file has a thread or a lock, for the same reason nothing in
// hcilink.h does; see loop.h.
#ifndef OCTO_HCISHARE_H
#define OCTO_HCISHARE_H

#include <memory>
#include <string>
#include <vector>

#include "hcilink.h"
#include "loop.h"

namespace octo {
namespace hci {

class SharedLink {
 public:
  class User;

  using DoneHandler = Link::DoneHandler;

  // Opens the dongle. As with Link::open, the port either opens or does not
  // and says so straight away; the controller coming up is several round trips
  // later and is reported to each user through User::when_ready.
  static std::unique_ptr<SharedLink> open(Loop* loop, const Link::Options& opts,
                                          std::string* err);

  // Same, over a port the caller supplies -- the same escape hatch Link::attach
  // is, and for the same reason: this is a state machine, and a state machine
  // that can only be exercised through real hardware is one that gets
  // exercised rarely. See tests/test_hcishare.cc.
  static std::unique_ptr<SharedLink> attach(Loop* loop,
                                            std::unique_ptr<Port> port,
                                            const Link::Options& opts);

  ~SharedLink();

  SharedLink(const SharedLink&) = delete;
  SharedLink& operator=(const SharedLink&) = delete;

  // A subscription. `name` appears in error messages and in nothing else; it
  // is there so that "the radio would not scan" says which half asked.
  std::unique_ptr<User> add_user(std::string name);

  // The controller, for the things that have one owner. Prefer User::link(),
  // which is the same pointer reached from the place that has a reason to
  // hold it.
  Link* link() { return link_.get(); }

  // How the controller came up, once it has. Users learn this through
  // when_ready; this is for a caller that wants to log it.
  bool ready() const { return state_ == State::kReady; }
  bool failed() const { return state_ == State::kFailed; }
  const std::string& error() const { return error_; }

  // What the radio is doing, as reconciled from what the users have asked
  // for. For tests and for the logs.
  bool scanning() const;
  bool scanning_active() const { return applied_active_; }
  size_t user_count() const { return users_.size(); }

 private:
  friend class User;

  enum class State { kOpening, kReady, kFailed };

  explicit SharedLink(Loop* loop);
  // The on_ready both constructors hand to the Link, guarded so that a link
  // that outlives this by a turn of the loop cannot call into it.
  Link::DoneHandler ready_cb();
  void install();

  void on_ready(bool ok, const std::string& err);
  void on_closed(const std::string& why);
  void on_advert(const AdvReport& report);
  void on_connected(const Link::Conn& conn);
  void on_disconnected(uint16_t handle, uint8_t reason);

  // Bring the radio into line with what the users want. Called whenever a user
  // starts or stops wanting a scan, whenever one is destroyed, and whenever a
  // connection attempt finishes -- the last because Link::connect turns the
  // scan off underneath us.
  //
  // `done` is the caller's completion, and is called with the outcome of
  // whatever this had to do, including nothing.
  void reconcile(DoneHandler done);
  // One step of that reconciliation. Issues at most one command and calls
  // itself again when it completes, so a change of scan parameters -- which is
  // a stop and a start -- is one queue of work rather than two nested lambdas.
  void pump();
  // Answer everybody waiting on the current reconciliation, and forget them.
  void settle(bool ok, const std::string& err);
  void drop_user(User* user);
  void begin_connect();
  void end_connect();

  // Run `fn` on the next turn of the loop, and not at all if this has been
  // destroyed in the meantime. Same reason Link::defer exists: a completion
  // handler must never run inside the call that registered it.
  void defer(std::function<void()> fn);

  Loop* loop_ = nullptr;
  std::shared_ptr<bool> alive_;
  std::unique_ptr<Link> link_;
  State state_ = State::kOpening;
  std::string error_;

  // Users, in the order they subscribed. Raw pointers: a User is owned by
  // whoever asked for it, and removes itself here when it dies.
  std::vector<User*> users_;

  // What was last asked of the controller, so that reconcile() can tell a
  // change from a no-op. `applied_scanning_` is not the same as
  // link_->scanning(): connect() turns the radio's scan off without anybody
  // here asking, and the difference between the two is precisely what has to
  // be repaired afterwards.
  bool applied_scanning_ = false;
  bool applied_active_ = false;
  bool reconciling_ = false;
  bool connecting_ = false;
  // Set when what the controller is doing can no longer be inferred from
  // applied_*. Link::connect restores a scan on failure by itself, and
  // restores it *passive* -- so after any connection attempt the parameters
  // have to be re-applied rather than assumed. Getting this wrong would leave
  // the camera's active scan silently downgraded, which looks like a camera
  // that stopped advertising its name.
  bool force_ = false;

  // Callers waiting on the reconciliation in flight.
  std::vector<DoneHandler> pending_;
};

// One subscriber's view of the radio.
//
// Destroying it withdraws everything it registered: its handlers stop being
// called, and its share of the scan is released, which may stop the radio.
// That is deliberately the only way to unsubscribe -- an explicit close()
// would be a second way to reach the same state, and this codebase has already
// paid for one object that could be half torn down.
class SharedLink::User {
 public:
  ~User();

  User(const User&) = delete;
  User& operator=(const User&) = delete;

  const std::string& name() const { return name_; }

  // Called when the controller has finished coming up, or has failed to. If
  // that has already happened this still calls back, on the next turn of the
  // loop rather than inside this call. At most one handler; the second
  // replaces the first.
  void when_ready(DoneHandler done);

  // Called when the port dies underneath everybody -- almost always the dongle
  // being unplugged. Every user is told.
  void set_closed_handler(Link::ClosedHandler on_closed);

  // Ask for the radio to be scanning, and receive every advertisement it hears
  // while it is. `done` reports whether the radio is now scanning, which it
  // may already have been.
  //
  // A user that is already scanning and calls this again replaces its handler
  // and its vote on active-versus-passive, which may restart the radio.
  void start_scan(bool active, Link::AdvHandler on_report, DoneHandler done);

  // Stop wanting the radio to scan. Reports success once the radio is in the
  // state this user asked for, which -- if somebody else is still scanning --
  // means the radio is still scanning and this user has simply stopped being
  // told about it.
  void stop_scan(DoneHandler done);

  // Whether this user has asked for a scan, not whether the radio is running
  // one. They differ while a connection is being made.
  bool wants_scan() const { return want_scan_; }

  // Connect outward. Goes through here rather than through link() so that the
  // scan the controller drops on the way in can be put back on the way out;
  // see the note on the third shared thing at the top of this file.
  void connect(const Address& peer, double timeout, Link::ConnectHandler done);

  // Told about every connection on the radio, not only this user's. There is
  // one initiator, so in practice there is one user that cares.
  void set_connection_handlers(Link::ConnectedHandler on_connect,
                               Link::DisconnectedHandler on_disconnect);

  // The controller, for ATT, SMP, advertising and raw commands. Single owner
  // by convention and not by enforcement -- see the top of this file.
  Link* link() { return owner_ ? owner_->link_.get() : nullptr; }

  // True once the SharedLink this belongs to has been destroyed. A User may
  // outlive it: the daemon tears down in whatever order its members were
  // declared, and every call here is a no-op afterwards rather than a crash.
  bool orphaned() const { return owner_ == nullptr; }

 private:
  friend class SharedLink;

  User(SharedLink* owner, Loop* loop, std::string name);
  void detach() { owner_ = nullptr; }

  SharedLink* owner_ = nullptr;
  // Held separately from the owner, because a User may outlive it and still
  // has to be able to report a failure on the loop rather than inside the call
  // that asked.
  Loop* loop_ = nullptr;
  std::string name_;

  DoneHandler on_ready_;
  Link::ClosedHandler on_closed_;
  Link::AdvHandler on_advert_;
  Link::ConnectedHandler on_connected_;
  Link::DisconnectedHandler on_disconnected_;

  bool want_scan_ = false;
  bool want_active_ = false;

  // False once the destructor has run, so that a fan-out already in progress
  // does not call into a user that went away inside one of its siblings'
  // handlers.
  std::shared_ptr<bool> alive_;
};

}  // namespace hci
}  // namespace octo

#endif  // OCTO_HCISHARE_H
