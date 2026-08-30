#include "hcishare.h"

#include <algorithm>
#include <utility>

namespace octo {
namespace hci {
namespace {

const char kGone[] = "the radio is gone";

}  // namespace

// ------------------------------------------------------------- SharedLink

SharedLink::SharedLink(Loop* loop)
    : loop_(loop), alive_(std::make_shared<bool>(true)) {}

SharedLink::~SharedLink() {
  *alive_ = false;
  // Users can outlive this -- the daemon destroys its members in whatever
  // order it declared them -- so tell each one before the link underneath it
  // disappears. Afterwards every call on them is a no-op.
  for (User* u : users_) u->detach();
  users_.clear();
  pending_.clear();
}

Link::DoneHandler SharedLink::ready_cb() {
  SharedLink* raw = this;
  std::shared_ptr<bool> alive = alive_;
  return [raw, alive](bool ok, const std::string& why) {
    if (*alive) raw->on_ready(ok, why);
  };
}

std::unique_ptr<SharedLink> SharedLink::open(Loop* loop,
                                             const Link::Options& opts,
                                             std::string* err) {
  std::unique_ptr<SharedLink> shared(new SharedLink(loop));
  shared->link_ = Link::open(loop, opts, shared->ready_cb(), err);
  if (!shared->link_) return nullptr;
  shared->install();
  return shared;
}

std::unique_ptr<SharedLink> SharedLink::attach(Loop* loop,
                                               std::unique_ptr<Port> port,
                                               const Link::Options& opts) {
  if (!port) return nullptr;
  std::unique_ptr<SharedLink> shared(new SharedLink(loop));
  shared->link_ = Link::attach(loop, std::move(port), opts, shared->ready_cb());
  if (!shared->link_) return nullptr;
  shared->install();
  return shared;
}

void SharedLink::install() {
  SharedLink* raw = this;
  std::shared_ptr<bool> alive = alive_;
  link_->set_closed_handler([raw, alive](const std::string& why) {
    if (*alive) raw->on_closed(why);
  });
  link_->set_connection_handlers(
      [raw, alive](const Link::Conn& conn) {
        if (*alive) raw->on_connected(conn);
      },
      [raw, alive](uint16_t handle, uint8_t reason) {
        if (*alive) raw->on_disconnected(handle, reason);
      });
}

void SharedLink::defer(std::function<void()> fn) {
  std::shared_ptr<bool> alive = alive_;
  loop_->after(0.0, [alive, fn] {
    if (*alive) fn();
  });
}

bool SharedLink::scanning() const { return link_ && link_->scanning(); }

std::unique_ptr<SharedLink::User> SharedLink::add_user(std::string name) {
  std::unique_ptr<User> user(new User(this, loop_, std::move(name)));
  users_.push_back(user.get());
  return user;
}

void SharedLink::drop_user(User* user) {
  users_.erase(std::remove(users_.begin(), users_.end(), user), users_.end());
  // The radio may now be scanning for nobody.
  reconcile(nullptr);
}

void SharedLink::on_ready(bool ok, const std::string& err) {
  state_ = ok ? State::kReady : State::kFailed;
  if (!ok) error_ = err;

  // Snapshot before dispatching: a when_ready handler is entitled to add a
  // user, drop one, or start a scan, and any of those changes users_.
  std::vector<std::pair<User*, std::weak_ptr<bool>>> snap;
  for (User* u : users_) snap.emplace_back(u, u->alive_);
  for (auto& entry : snap) {
    std::shared_ptr<bool> live = entry.second.lock();
    if (!live || !*live) continue;
    DoneHandler done = std::move(entry.first->on_ready_);
    entry.first->on_ready_ = nullptr;
    if (done) done(ok, err);
  }

  // Whatever was asked for while the controller was coming up can happen now,
  // or has to be failed.
  if (ok) {
    pump();
  } else {
    settle(false, err);
  }
}

void SharedLink::on_closed(const std::string& why) {
  state_ = State::kFailed;
  error_ = why;
  settle(false, why);

  std::vector<std::pair<User*, std::weak_ptr<bool>>> snap;
  for (User* u : users_) snap.emplace_back(u, u->alive_);
  for (auto& entry : snap) {
    std::shared_ptr<bool> live = entry.second.lock();
    if (!live || !*live) continue;
    if (entry.first->on_closed_) entry.first->on_closed_(why);
  }
}

void SharedLink::on_advert(const AdvReport& report) {
  // Same snapshot-and-check as everywhere else here: one user's handler is
  // entitled to destroy another user -- the sync daemon releases its camera
  // from inside a view handler -- and iterating users_ directly would then
  // walk off the end of a vector that changed underneath.
  std::vector<std::pair<User*, std::weak_ptr<bool>>> snap;
  for (User* u : users_) {
    if (u->want_scan_ && u->on_advert_) snap.emplace_back(u, u->alive_);
  }
  for (auto& entry : snap) {
    std::shared_ptr<bool> live = entry.second.lock();
    if (!live || !*live) continue;
    if (entry.first->want_scan_ && entry.first->on_advert_) {
      entry.first->on_advert_(report);
    }
  }
}

void SharedLink::on_connected(const Link::Conn& conn) {
  std::vector<std::pair<User*, std::weak_ptr<bool>>> snap;
  for (User* u : users_) snap.emplace_back(u, u->alive_);
  for (auto& entry : snap) {
    std::shared_ptr<bool> live = entry.second.lock();
    if (!live || !*live) continue;
    if (entry.first->on_connected_) entry.first->on_connected_(conn);
  }
}

void SharedLink::on_disconnected(uint16_t handle, uint8_t reason) {
  std::vector<std::pair<User*, std::weak_ptr<bool>>> snap;
  for (User* u : users_) snap.emplace_back(u, u->alive_);
  for (auto& entry : snap) {
    std::shared_ptr<bool> live = entry.second.lock();
    if (!live || !*live) continue;
    if (entry.first->on_disconnected_) {
      entry.first->on_disconnected_(handle, reason);
    }
  }
}

void SharedLink::begin_connect() { connecting_ = true; }

void SharedLink::end_connect() {
  connecting_ = false;
  // The controller's scan state is not what applied_* says any more, whichever
  // way the attempt went; see the note on force_ in the header.
  force_ = true;
  reconcile(nullptr);
}

void SharedLink::reconcile(DoneHandler done) {
  if (done) pending_.push_back(std::move(done));
  if (!reconciling_) pump();
}

void SharedLink::settle(bool ok, const std::string& err) {
  std::vector<DoneHandler> waiting;
  waiting.swap(pending_);
  for (const DoneHandler& done : waiting) {
    if (done) done(ok, err);
  }
}

void SharedLink::pump() {
  if (!link_ || link_->closed() || state_ == State::kFailed) {
    settle(false, error_.empty() ? std::string(kGone) : error_);
    return;
  }
  // Nothing can be commanded until the controller is up. on_ready comes back
  // through here.
  if (state_ == State::kOpening) return;
  // A connection is being made, and the controller cannot scan while it is.
  // end_connect puts this right.
  if (connecting_) return;

  bool want = false;
  bool active = false;
  for (User* u : users_) {
    if (!u->want_scan_) continue;
    want = true;
    active = active || u->want_active_;
  }

  const bool on = link_->scanning();

  SharedLink* raw = this;
  std::shared_ptr<bool> alive = alive_;
  auto next = [raw, alive](bool ok, const std::string& err) {
    if (!*alive) return;
    raw->reconciling_ = false;
    if (!ok) {
      raw->settle(false, err);
      return;
    }
    raw->pump();
  };

  // Parameters that cannot be trusted: stop, so that the start below installs
  // known ones.
  if (force_) {
    force_ = false;
    if (on) {
      reconciling_ = true;
      link_->stop_scan([raw, alive, next](bool ok, const std::string& err) {
        if (!*alive) return;
        raw->applied_scanning_ = false;
        next(ok, err);
      });
      return;
    }
  }

  if (!want) {
    if (!on) {
      applied_scanning_ = false;
      settle(true, std::string());
      return;
    }
    reconciling_ = true;
    link_->stop_scan([raw, alive, next](bool ok, const std::string& err) {
      if (!*alive) return;
      raw->applied_scanning_ = false;
      next(ok, err);
    });
    return;
  }

  if (on && active == applied_active_) {
    applied_scanning_ = true;
    settle(true, std::string());
    return;
  }

  if (on) {
    // Wanted, running, wrong parameters. Stop; the next turn starts it again.
    reconciling_ = true;
    link_->stop_scan([raw, alive, next](bool ok, const std::string& err) {
      if (!*alive) return;
      raw->applied_scanning_ = false;
      next(ok, err);
    });
    return;
  }

  reconciling_ = true;
  applied_active_ = active;
  // Duplicate filtering is off for everybody. A Tentacle's whole value is that
  // it repeats its clock several times a second, and the camera scan counts
  // sightings rather than devices, so there is nothing here that wants the
  // controller to report a device once and then go quiet about it.
  link_->start_scan(
      active, /*filter_duplicates=*/false,
      [raw, alive](const AdvReport& r) {
        if (*alive) raw->on_advert(r);
      },
      [raw, alive, next](bool ok, const std::string& err) {
        if (!*alive) return;
        if (ok) raw->applied_scanning_ = true;
        next(ok, err);
      });
}

// ------------------------------------------------------------------- User

SharedLink::User::User(SharedLink* owner, Loop* loop, std::string name)
    : owner_(owner),
      loop_(loop),
      name_(std::move(name)),
      alive_(std::make_shared<bool>(true)) {}

SharedLink::User::~User() {
  *alive_ = false;
  on_ready_ = nullptr;
  on_closed_ = nullptr;
  on_advert_ = nullptr;
  on_connected_ = nullptr;
  on_disconnected_ = nullptr;
  want_scan_ = false;
  if (owner_) owner_->drop_user(this);
}

void SharedLink::User::when_ready(DoneHandler done) {
  if (!owner_) {
    if (done && loop_) {
      loop_->after(0.0, [done] { done(false, std::string(kGone)); });
    }
    return;
  }
  if (owner_->state_ == State::kOpening) {
    on_ready_ = std::move(done);
    return;
  }
  const bool ok = owner_->state_ == State::kReady;
  const std::string err = owner_->error_;
  std::shared_ptr<bool> alive = alive_;
  User* raw = this;
  owner_->defer([raw, alive, done, ok, err] {
    if (!*alive) return;
    (void)raw;
    if (done) done(ok, err);
  });
}

void SharedLink::User::set_closed_handler(Link::ClosedHandler on_closed) {
  on_closed_ = std::move(on_closed);
}

void SharedLink::User::set_connection_handlers(
    Link::ConnectedHandler on_connect,
    Link::DisconnectedHandler on_disconnect) {
  on_connected_ = std::move(on_connect);
  on_disconnected_ = std::move(on_disconnect);
}

void SharedLink::User::start_scan(bool active, Link::AdvHandler on_report,
                                  DoneHandler done) {
  if (!owner_) {
    if (done && loop_) {
      loop_->after(0.0, [done] { done(false, std::string(kGone)); });
    }
    return;
  }
  if (on_report) on_advert_ = std::move(on_report);
  want_scan_ = true;
  want_active_ = active;
  owner_->reconcile(std::move(done));
}

void SharedLink::User::stop_scan(DoneHandler done) {
  want_scan_ = false;
  want_active_ = false;
  if (!owner_) {
    if (done && loop_) {
      loop_->after(0.0, [done] { done(true, std::string()); });
    }
    return;
  }
  owner_->reconcile(std::move(done));
}

void SharedLink::User::connect(const Address& peer, double timeout,
                               Link::ConnectHandler done) {
  if (!owner_ || !owner_->link_) {
    if (done && loop_) {
      loop_->after(0.0, [done] { done(false, 0, std::string(kGone)); });
    }
    return;
  }
  SharedLink* owner = owner_;
  std::shared_ptr<bool> alive = owner_->alive_;
  owner->begin_connect();
  owner->link_->connect(peer, timeout,
                        [owner, alive, done](bool ok, uint16_t handle,
                                             const std::string& err) {
                          // Put the radio back before telling the caller: what
                          // it does next may take a while, and the reference
                          // clock should not be off for that whole time.
                          if (*alive) owner->end_connect();
                          if (done) done(ok, handle, err);
                        });
}

}  // namespace hci
}  // namespace octo
