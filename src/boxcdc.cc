// See src/boxcdc.h.
#include "boxcdc.h"

#include <utility>
#include <vector>

namespace octo {

std::unique_ptr<BoxLink> BoxLink::attach(Loop* loop,
                                         std::unique_ptr<hci::Port> port) {
  if (loop == nullptr || port == nullptr) return nullptr;

  std::unique_ptr<BoxLink> link(new BoxLink());
  link->loop_ = loop;
  link->port_ = std::move(port);
  link->alive_ = std::make_shared<bool>(true);

  BoxLink* raw = link.get();
  std::shared_ptr<bool> alive = link->alive_;
  link->source_ = loop->add_source(
      link->port_->handle(), kRead,
      [raw, alive](int) {
        if (*alive) raw->on_readable();
      },
      [raw, alive](const std::string& why) {
        if (*alive) raw->on_port_error(why);
      });
  return link;
}

BoxLink::~BoxLink() {
  if (alive_) *alive_ = false;
  if (source_ != kNoSource && loop_ != nullptr) loop_->remove_source(source_);
  source_ = kNoSource;
}

void BoxLink::on_line(LineHandler handler) { on_line_ = std::move(handler); }
void BoxLink::on_message(MessageHandler handler) {
  on_message_ = std::move(handler);
}
void BoxLink::on_closed(ClosedHandler handler) {
  on_closed_ = std::move(handler);
}

bool BoxLink::is_open() const {
  return !closed_ && port_ != nullptr && port_->is_open();
}

std::string BoxLink::name() const {
  return port_ != nullptr ? port_->name() : std::string();
}

void BoxLink::send(const std::string& line) {
  if (!is_open()) return;
  // One message per line -- src/boxmsg.h. The terminator is added here rather
  // than by the caller for the same reason firmware/src/cdcpeer.cc adds it:
  // framing belongs to whoever owns the wire.
  std::string out = line;
  out.push_back('\n');
  if (!port_->write(reinterpret_cast<const uint8_t*>(out.data()), out.size())) {
    // A partial line is worse than none: the box would parse the fragment as a
    // command and answer an error, and the rest as another. There is no
    // resynchronising from that beyond closing.
    close("write failed");
  }
}

void BoxLink::send(const Message& msg) { send(encode(msg)); }

void BoxLink::close(const std::string& why) {
  if (closed_) return;
  closed_ = true;
  if (source_ != kNoSource && loop_ != nullptr) {
    loop_->remove_source(source_);
    source_ = kNoSource;
  }
  if (port_ != nullptr) port_->close();
  if (on_closed_) on_closed_(why);
}

void BoxLink::on_port_error(const std::string& why) { close(why); }

void BoxLink::on_readable() {
  // Guarded against a handler that closes the link mid-batch, which is the
  // ordinary response to hearing that the box has crashed: the loop is still
  // inside this call, and both the reader and the port are members of it.
  std::shared_ptr<bool> alive = alive_;

  uint8_t buf[512];
  std::vector<std::string> lines;
  bool broke = false;
  // Bounded, so that one noisy port cannot hold the loop against every other
  // source. Whatever is left stays in the kernel and the loop comes straight
  // back for it -- src/loop.h is level-triggered.
  constexpr int kMaxReadsPerWake = 64;
  for (int i = 0; i < kMaxReadsPerWake; ++i) {
    const int n = port_->read(buf, sizeof buf, 0.0);
    if (n < 0) {
      // What unplugging the dongle looks like. Noted rather than acted on, so
      // that whatever already arrived is delivered first: the last thing a box
      // says before it goes is very often the reason it went.
      broke = true;
      break;
    }
    if (n == 0) break;
    if (!reader_.feed(reinterpret_cast<const char*>(buf),
                      static_cast<size_t>(n), &lines)) {
      ++long_lines_;
    }
    // Deliberately no "a short read means it is drained" shortcut. It is true,
    // and it is also how a broken port goes unnoticed: the read that would
    // have returned -1 is the one the shortcut skips, so an unplugged dongle
    // stays "open" until something else happens to notice. Reading once more
    // costs a poll with a zero timeout and removes the whole class of bug.
  }

  for (const std::string& line : lines) {
    if (!*alive || closed_) return;
    if (on_line_) on_line_(line);
    if (!*alive || closed_) return;
    if (on_message_) {
      Message msg;
      std::string err;
      if (decode(line, &msg, &err)) {
        on_message_(msg);
      } else {
        ++bad_lines_;
      }
    }
  }

  if (!*alive || closed_) return;
  if (broke) {
    close("the dongle went away");
  } else if (port_ != nullptr && !port_->is_open()) {
    close("port closed");
  }
}

}  // namespace octo
