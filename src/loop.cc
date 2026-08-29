#include "loop.h"

#include <algorithm>
#include <utility>

namespace octo {

Loop::Loop() = default;
Loop::~Loop() = default;

SourceId Loop::add_source(const Handle& handle, int interest,
                          ReadyHandler on_ready, ErrorHandler on_error) {
  Source s;
  s.id = next_source_++;
  s.handle = handle;
  s.interest = interest;
  s.on_ready = std::move(on_ready);
  s.on_error = std::move(on_error);
  sources_.push_back(std::move(s));
  return sources_.back().id;
}

Loop::Source* Loop::find(SourceId id) {
  for (auto& s : sources_) {
    if (s.id == id && !s.dead) return &s;
  }
  return nullptr;
}

void Loop::set_interest(SourceId id, int interest) {
  if (Source* s = find(id)) s->interest = interest;
}

void Loop::remove_source(SourceId id) {
  if (Source* s = find(id)) {
    s->dead = true;
    need_sweep_ = true;
  }
  if (depth_ == 0) sweep();
}

TimerId Loop::at(double deadline, TimerHandler fn) {
  Timer t;
  t.id = next_timer_++;
  t.deadline = deadline;
  t.seq = next_seq_++;
  t.fn = std::move(fn);
  timers_.push_back(std::move(t));
  return timers_.back().id;
}

TimerId Loop::after(double seconds, TimerHandler fn) {
  return at(now() + seconds, std::move(fn));
}

TimerId Loop::every(double period, TimerHandler fn) {
  TimerId id = at(now() + period, std::move(fn));
  for (auto& t : timers_) {
    if (t.id == id) t.period = period;
  }
  return id;
}

void Loop::cancel(TimerId id) {
  for (auto& t : timers_) {
    if (t.id == id) {
      t.dead = true;
      need_sweep_ = true;
    }
  }
  if (depth_ == 0) sweep();
}

double Loop::next_deadline() const {
  double best = -1.0;
  for (const auto& t : timers_) {
    if (t.dead) continue;
    if (best < 0.0 || t.deadline < best) best = t.deadline;
  }
  return best;
}

void Loop::sweep() {
  if (!need_sweep_) return;
  sources_.erase(
      std::remove_if(sources_.begin(), sources_.end(),
                     [](const Source& s) { return s.dead; }),
      sources_.end());
  timers_.erase(
      std::remove_if(timers_.begin(), timers_.end(),
                     [](const Timer& t) { return t.dead; }),
      timers_.end());
  need_sweep_ = false;
}

void Loop::stop() { running_ = false; }

void Loop::wake() { wake_backend(); }

void Loop::run() {
  while (tick(-1.0)) {
  }
}

bool Loop::tick(double max_wait) {
  if (!running_) return false;

  // How long there is to wait: whatever the caller allowed, cut short by the
  // earliest timer. A negative wait means "no limit", and the two negatives
  // have to be kept apart -- "no timers" and "no limit" are not the same
  // number even though both are spelled as one.
  double budget = max_wait;
  const double deadline = next_deadline();
  if (deadline >= 0.0) {
    double remaining = deadline - now();
    if (remaining < 0.0) remaining = 0.0;
    if (budget < 0.0 || remaining < budget) budget = remaining;
  }

  std::vector<std::pair<SourceId, int>> ready;
  std::vector<SourceId> failed;
  wait(budget, &ready, &failed);

  ++depth_;

  // I/O before timers, deliberately. A timer that fires at the same instant as
  // an arriving packet is almost always a timeout for that packet, and running
  // the timeout first would report a failure for something that had in fact
  // arrived.
  for (const auto& r : ready) {
    Source* s = find(r.first);
    if (!s || !s->on_ready) continue;
    // Copy the handler out before calling it. A handler is allowed to add a
    // source -- Server's accept handler does exactly that on every new
    // connection -- and adding one can reallocate the vector underneath a
    // std::function that is still executing.
    ReadyHandler fn = s->on_ready;
    fn(r.second);
  }

  for (SourceId id : failed) {
    Source* s = find(id);
    if (!s || !s->on_error) continue;
    ErrorHandler fn = s->on_error;
    fn("the source failed");
  }

  // Snapshot the due set before running any of it. A handler that posts
  // after(0.0, ...) is posting a continuation, and a continuation belongs on
  // the next tick -- otherwise a handler that reposts itself never returns.
  const double t_now = now();
  struct Due {
    double deadline;
    uint64_t seq;
    TimerId id;
    bool operator<(const Due& o) const {
      if (deadline != o.deadline) return deadline < o.deadline;
      return seq < o.seq;
    }
  };
  std::vector<Due> due;
  for (const auto& t : timers_) {
    if (t.dead || t.deadline > t_now) continue;
    due.push_back(Due{t.deadline, t.seq, t.id});
  }
  // Earliest first; ties broken by creation order, so that two timers armed
  // for the same instant fire in the order they were asked for.
  std::sort(due.begin(), due.end());

  for (const auto& key : due) {
    // Re-find every time: an earlier handler in this same batch is allowed to
    // have cancelled this one, and a cancelled timer must not fire.
    Timer* t = nullptr;
    for (auto& cand : timers_) {
      if (cand.id == key.id && !cand.dead) {
        t = &cand;
        break;
      }
    }
    if (!t) continue;

    const TimerHandler fn = t->fn;
    const double period = t->period;
    const double fired_for = t->deadline;
    if (period <= 0.0) {
      t->dead = true;
      need_sweep_ = true;
    }

    fn();

    // Re-arm after the handler, not before, so that a handler which ran long
    // is measured by how long it actually ran. Re-find it: the handler is
    // allowed to have cancelled itself, and re-arming a cancelled timer would
    // resurrect it.
    if (period <= 0.0) continue;
    for (auto& cand : timers_) {
      if (cand.id != key.id || cand.dead) continue;
      // Realign to the future rather than to the past. Periods missed while a
      // handler was running are gone and are not owed: a beacon catching up on
      // four minutes of skipped broadcasts is a burst of radio nobody asked
      // for, and a sync cycle doing it is four connections to a camera.
      double next = fired_for + period;
      const double after = now();
      if (next <= after) {
        const long long skip =
            static_cast<long long>((after - fired_for) / period) + 1;
        next = fired_for + static_cast<double>(skip) * period;
      }
      cand.deadline = next;
      break;
    }
  }

  --depth_;
  sweep();
  return running_;
}

}  // namespace octo
