// See src/watchdog.h.
#include "watchdog.h"

#include <utility>

namespace octo {
namespace {

const std::string& empty_name() {
  static const std::string empty;
  return empty;
}

}  // namespace

bool WatchdogPolicy::watch(std::string name, Check check) {
  if (checks_.size() >= kMaxChecks || !check) return false;
  Entry entry;
  entry.name = std::move(name);
  entry.check = std::move(check);
  checks_.push_back(std::move(entry));
  return true;
}

const std::string& WatchdogPolicy::name(size_t index) const {
  if (index >= checks_.size()) return empty_name();
  return checks_[index].name;
}

uint32_t WatchdogPolicy::poll(double now) {
  uint32_t mask = 0;
  for (size_t i = 0; i < checks_.size(); ++i) {
    Entry& entry = checks_[i];
    const bool ok = entry.check();
    if (ok) {
      entry.failing = false;
      mask |= (1u << i);
    } else if (!entry.failing) {
      // First failure. Stamped rather than counted, so that a check which
      // fails, recovers and fails again does not accumulate towards a reset --
      // a watchdog is about the machine being stuck now, not about it having
      // had a bad afternoon.
      entry.failing = true;
      entry.since = now;
    }
  }
  return mask;
}

double WatchdogPolicy::failing_for(size_t index, double now) const {
  if (index >= checks_.size()) return 0.0;
  const Entry& entry = checks_[index];
  if (!entry.failing) return 0.0;
  const double span = now - entry.since;
  return span > 0.0 ? span : 0.0;
}

std::string WatchdogPolicy::worst(double now) const {
  std::string name;
  double longest = 0.0;
  for (size_t i = 0; i < checks_.size(); ++i) {
    const double span = failing_for(i, now);
    if (!checks_[i].failing) continue;
    if (name.empty() || span > longest) {
      longest = span;
      name = checks_[i].name;
    }
  }
  return name;
}

ProbeLiveness::ProbeLiveness(double period, double patience)
    : period_(period), patience_(patience) {}

bool ProbeLiveness::poll(double now, uint32_t ticks, bool* poke) {
  if (poke != nullptr) *poke = false;

  if (!started_) {
    // The first call establishes the baseline rather than judging against one.
    // Without this, a probe created at boot is already overdue.
    started_ = true;
    last_poke_ = now;
    ticks_at_poke_ = ticks;
    waiting_ = false;
    return true;
  }

  if (waiting_) {
    if (ticks != ticks_at_poke_) {
      // Answered. Nothing more is asked of it until the next period.
      waiting_ = false;
      last_poke_ = now;
      return true;
    }
    // Still nothing. Healthy until the patience runs out -- a workqueue that
    // is merely busy is not a workqueue that is stuck.
    return now - last_poke_ <= patience_;
  }

  if (now - last_poke_ >= period_) {
    waiting_ = true;
    last_poke_ = now;
    ticks_at_poke_ = ticks;
    if (poke != nullptr) *poke = true;
  }
  return true;
}

}  // namespace octo
