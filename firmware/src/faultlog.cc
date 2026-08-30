// See firmware/src/faultlog.h.
#include "faultlog.h"

#include <zephyr/arch/cpu.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/toolchain.h>

#include <cstdio>
#include <cstring>

namespace octo {
namespace {

// Anything but zero and anything but a plausible value for uninitialised RAM.
constexpr uint32_t kMagic = 0x0C70417E;

struct Retained {
  uint32_t magic;
  uint32_t reason;
  uint32_t pc;
  uint32_t lr;
  uint32_t count;
};

// __noinit is the whole mechanism: RAM survives SYSRESETREQ, and this is the
// promise that nothing zeroes it on the way back up.
__noinit Retained g_retained;

// How long a run has to last before it counts as having got somewhere. Long
// enough to be past bt_enable() and the first adverts, short enough that a
// person watching does not wait for it.
constexpr int kSettledSeconds = 30;

// Zephyr's reason codes, written down so that a version of Zephyr which
// renumbers them fails the build rather than quietly relabelling a crash. The
// numbers travel through retained memory across a reboot, so a mislabelled one
// is a wrong answer in a place nobody would think to check.
static_assert(K_ERR_CPU_EXCEPTION == 0, "K_ERR_CPU_EXCEPTION moved");
static_assert(K_ERR_SPURIOUS_IRQ == 1, "K_ERR_SPURIOUS_IRQ moved");
static_assert(K_ERR_STACK_CHK_FAIL == 2, "K_ERR_STACK_CHK_FAIL moved");
static_assert(K_ERR_KERNEL_OOPS == 3, "K_ERR_KERNEL_OOPS moved");
static_assert(K_ERR_KERNEL_PANIC == 4, "K_ERR_KERNEL_PANIC moved");

const char* reason_name(uint32_t reason) {
  switch (reason) {
    case K_ERR_CPU_EXCEPTION: return "cpu-exception";
    case K_ERR_SPURIOUS_IRQ: return "spurious-irq";
    // The one worth naming precisely. It is what a recursion or a big local
    // buffer looks like from here, and the fix is a number in prj.conf rather
    // than anything in the code the backtrace points at.
    case K_ERR_STACK_CHK_FAIL: return "stack-overflow";
    case K_ERR_KERNEL_OOPS: return "kernel-oops";
    case K_ERR_KERNEL_PANIC: return "kernel-panic";
    default: return "unknown";
  }
}

void settled(struct k_timer*) { mark_run_settled(); }
K_TIMER_DEFINE(g_settle_timer, settled, nullptr);

}  // namespace

FaultRecord take_last_fault() {
  FaultRecord out;
  if (g_retained.magic == kMagic) {
    out.valid = true;
    out.reason = g_retained.reason;
    out.pc = g_retained.pc;
    out.lr = g_retained.lr;
    out.count = g_retained.count;
  } else {
    // Power-on, or a reset from somewhere that is not a fault -- the DFU verb
    // reboots too, and that is not a crash. Start the count from nothing.
    g_retained.count = 0;
  }
  // Cleared either way, so the next boot after a clean run says nothing. The
  // count deliberately survives: it is what makes a crash loop visible.
  g_retained.magic = 0;

  k_timer_start(&g_settle_timer, K_SECONDS(kSettledSeconds), K_NO_WAIT);
  return out;
}

void mark_run_settled() { g_retained.count = 0; }

std::string describe_fault(const FaultRecord& fault) {
  if (!fault.valid) return std::string();
  char buf[160];
  if (fault.count > 1) {
    std::snprintf(buf, sizeof buf,
                  "last run died: %s at pc=0x%08x lr=0x%08x "
                  "(%u in a row -- this box is not staying up)",
                  reason_name(fault.reason), fault.pc, fault.lr, fault.count);
  } else {
    std::snprintf(buf, sizeof buf, "last run died: %s at pc=0x%08x lr=0x%08x",
                  reason_name(fault.reason), fault.pc, fault.lr);
  }
  return buf;
}

}  // namespace octo

// Zephyr's hook. The default halts the CPU with interrupts locked, which on a
// USB device means the pull-up stays up, the host keeps listing a device, and
// every open() on it blocks forever with no clue why. Rebooting instead costs
// the run and saves the diagnosis: the dongle comes back and says what
// happened. See the header.
extern "C" void k_sys_fatal_error_handler(unsigned int reason,
                                          const struct arch_esf* esf) {
  octo::g_retained.magic = octo::kMagic;
  octo::g_retained.reason = reason;
  octo::g_retained.pc = esf != nullptr ? esf->basic.pc : 0;
  octo::g_retained.lr = esf != nullptr ? esf->basic.lr : 0;
  octo::g_retained.count++;

  // A box that faults during boot would otherwise reboot as fast as the CPU
  // can manage, which is a device that flickers on the USB bus and cannot be
  // caught long enough to be reprogrammed. Slow the loop down to something a
  // person can get a DFU command into.
  if (octo::g_retained.count > 3) k_busy_wait(2 * 1000 * 1000);

  sys_reboot(SYS_REBOOT_COLD);
  CODE_UNREACHABLE;
}
