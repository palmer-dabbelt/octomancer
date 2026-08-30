// See firmware/src/hwwatchdog.h.
#include "hwwatchdog.h"

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>

#include <vector>

#include "faultlog.h"

namespace octo {
namespace {

const struct device* wdt_device() {
  return DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
}

// How often the checks are run, as a fraction of the timeout. A quarter leaves
// three missed feeds of slack before a reset, which is the difference between
// catching a machine that has stopped and punishing one that was briefly busy.
constexpr double kFeedFraction = 0.25;

struct Channels {
  std::vector<int> ids;
  WatchdogPolicy* policy = nullptr;
};

Channels g_channels;

}  // namespace

bool start_watchdog(Loop* loop, WatchdogPolicy* policy, double timeout_s,
                    std::string* err) {
  if (loop == nullptr || policy == nullptr || policy->size() == 0) {
    if (err) *err = "nothing to watch";
    return false;
  }

  const struct device* wdt = wdt_device();
  if (wdt == nullptr || !device_is_ready(wdt)) {
    if (err) *err = "no watchdog on this board";
    return false;
  }

  const uint32_t timeout_ms = static_cast<uint32_t>(timeout_s * 1000.0);
  g_channels.ids.clear();
  g_channels.policy = policy;

  for (size_t i = 0; i < policy->size(); ++i) {
    struct wdt_timeout_cfg cfg = {};
    cfg.window.min = 0;
    cfg.window.max = timeout_ms;
    // No callback. The nRF part gives two cycles of a 32 kHz clock between the
    // timeout and the reset -- about sixty microseconds -- and there is nothing
    // useful and safe to do in that. What the next boot needs is recorded
    // before the fact instead, by the feeder below, which knows which check was
    // failing while there was still time to write it down.
    cfg.callback = nullptr;
    cfg.flags = WDT_FLAG_RESET_SOC;
    const int id = wdt_install_timeout(wdt, &cfg);
    if (id < 0) {
      if (err) *err = "the watchdog would not take another channel";
      return false;
    }
    g_channels.ids.push_back(id);
  }

  // PAUSE_HALTED_BY_DBG so that stopping in a debugger is not a reset. There is
  // no debugger on this board today, and leaving it out would make attaching
  // one later mysteriously impossible.
  if (wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG) != 0) {
    if (err) *err = "the watchdog would not start";
    return false;
  }

  loop->every(timeout_s * kFeedFraction, [wdt, policy, loop]() {
    const double now = loop->now();
    const uint32_t mask = policy->poll(now);
    for (size_t i = 0; i < g_channels.ids.size(); ++i) {
      if ((mask & (1u << i)) != 0) wdt_feed(wdt, g_channels.ids[i]);
    }
    // Written down every time round rather than only when something is wrong,
    // because the moment it is needed is the moment there is no longer anywhere
    // to write it from. A box that resets can then say which check stopped
    // holding rather than only that something did.
    note_watchdog_state(policy->worst(now));
  });

  return true;
}

}  // namespace octo
