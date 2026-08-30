// See firmware/src/cdcpeer.h.
#include "cdcpeer.h"

#include <zephyr/drivers/uart.h>

#include <cstring>
#include <string>
#include <vector>

namespace octo {
namespace {

// How often DTR is looked at. A person plugging in a cable does not notice a
// tenth of a second, and this is the only polling in the program -- everything
// else is woken by something that actually happened.
constexpr double kControlPoll = 0.1;

}  // namespace

CdcPeer::CdcPeer(Loop* loop, const struct device* uart)
    : loop_(loop), uart_(uart) {
  k_poll_signal_init(&rx_signal_);
  ring_buf_init(&rx_, sizeof rx_storage_, rx_storage_);
  ring_buf_init(&tx_, sizeof tx_storage_, tx_storage_);
}

CdcPeer::~CdcPeer() { stop(); }

bool CdcPeer::start(std::string* err) {
  if (started_) return true;
  if (uart_ == nullptr || !device_is_ready(uart_)) {
    if (err) *err = "the CDC ACM device is not ready";
    return false;
  }

  uart_irq_rx_disable(uart_);
  uart_irq_tx_disable(uart_);
  uart_irq_callback_user_data_set(uart_, &CdcPeer::isr_trampoline, this);

  Handle handle;
  handle.object = &rx_signal_;
  source_ = loop_->add_source(
      handle, kRead, [this](int) { drain(); },
      [](const std::string&) {});

  control_timer_ = loop_->every(kControlPoll, [this]() { poll_control(); });

  uart_irq_rx_enable(uart_);
  started_ = true;
  return true;
}

void CdcPeer::stop() {
  if (!started_) return;
  uart_irq_rx_disable(uart_);
  uart_irq_tx_disable(uart_);
  if (source_ != kNoSource) loop_->remove_source(source_);
  if (control_timer_ != kNoTimer) loop_->cancel(control_timer_);
  source_ = kNoSource;
  control_timer_ = kNoTimer;
  started_ = false;
}

void CdcPeer::isr_trampoline(const struct device* dev, void* user) {
  (void)dev;
  static_cast<CdcPeer*>(user)->isr();
}

// Interrupt context. Nothing here allocates, takes a lock, or touches the
// loop's data -- it moves bytes between the UART FIFO and a ring buffer and
// raises a signal, which is the whole of what src/loop.h permits.
void CdcPeer::isr() {
  // Proof of life for whoever is watching -- see probe_wire() in the header.
  // First thing, so that it counts even for a pass that finds nothing to do,
  // which is exactly the pass a probe provokes.
  ++wire_ticks_;

  while (uart_irq_update(uart_) && uart_irq_is_pending(uart_)) {
    if (uart_irq_rx_ready(uart_)) {
      uint8_t buf[64];
      const int n = uart_fifo_read(uart_, buf, sizeof buf);
      if (n > 0) {
        const uint32_t put = ring_buf_put(&rx_, buf, static_cast<uint32_t>(n));
        if (put < static_cast<uint32_t>(n)) {
          dropped_rx_ += static_cast<uint32_t>(n) - put;
        }
        // Idempotent: a signal already raised stays raised, so a burst of
        // interrupts is one wake rather than a queue of them.
        k_poll_signal_raise(&rx_signal_, 1);
      }
    }

    if (uart_irq_tx_ready(uart_)) {
      uint8_t* data = nullptr;
      const uint32_t claim = ring_buf_get_claim(&tx_, &data, 64);
      if (claim == 0) {
        ring_buf_get_finish(&tx_, 0);
        // Nothing left to send. Leaving the transmit interrupt enabled here
        // is a live-lock: it fires immediately, finds the buffer empty, and
        // fires again forever.
        uart_irq_tx_disable(uart_);
      } else {
        const int sent = uart_fifo_fill(uart_, data, claim);
        ring_buf_get_finish(&tx_, sent > 0 ? static_cast<uint32_t>(sent) : 0);
      }
    }
  }
}

void CdcPeer::send(const std::string& line) {
  // A line nobody is listening to is not worth buffering. Without this the
  // ring fills the moment the daemon starts announcing, and the first host to
  // attach is greeted with several minutes of stale sightings.
  if (!attached_) return;

  // One message per line -- src/boxmsg.h. The terminator is added here rather
  // than by the daemon because it is framing, and framing is this file's job.
  const size_t need = line.size() + 1;
  if (ring_buf_space_get(&tx_) < need) {
    // Dropping the newest rather than making room. A half-written line is a
    // parse error at the other end, and a reader that has to resynchronise is
    // a worse failure than a message that never arrived.
    dropped_tx_ += static_cast<uint32_t>(need);
    return;
  }
  ring_buf_put(&tx_, reinterpret_cast<const uint8_t*>(line.data()),
               static_cast<uint32_t>(line.size()));
  const uint8_t nl = '\n';
  ring_buf_put(&tx_, &nl, 1);
  uart_irq_tx_enable(uart_);
}

void CdcPeer::probe_wire() {
  if (!started_) return;
  // Enabling the transmit interrupt is the cheapest way to make the driver
  // queue our handler. It runs, finds the ring empty, and turns itself off
  // again -- which is the same path a finished transmission takes, so this
  // provokes nothing the ordinary code does not already do.
  uart_irq_tx_enable(uart_);
}

void CdcPeer::drain() {
  uint8_t buf[128];
  std::vector<std::string> lines;
  for (;;) {
    const uint32_t n = ring_buf_get(&rx_, buf, sizeof buf);
    if (n == 0) break;
    if (!reader_.feed(reinterpret_cast<const char*>(buf), n, &lines)) {
      ++long_lines_;
    }
  }
  if (!on_line_) return;
  for (const std::string& line : lines) on_line_(line);
}

// DTR is the only evidence a serial port offers that anyone is there.
void CdcPeer::poll_control() {
  uint32_t dtr = 0;
  if (uart_line_ctrl_get(uart_, UART_LINE_CTRL_DTR, &dtr) != 0) {
    // A port that cannot report DTR is one we cannot tell is attached, so
    // treat it as attached and let the ring buffer's counters say what
    // happened. Refusing to speak at all would be worse: it would make a
    // working cable look like a dead box.
    dtr = 1;
  }
  const bool now = dtr != 0;
  if (now == attached_) return;
  attached_ = now;
  if (now) {
    // A new host inherits none of the last one's half-sent line, and none of
    // its backlog. Both would arrive as a parse error against a greeting.
    reader_.reset();
    ring_buf_reset(&tx_);
    if (on_open_) on_open_();
  } else {
    if (on_close_) on_close_();
  }
}

}  // namespace octo
