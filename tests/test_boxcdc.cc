// The box protocol over a byte pipe, driven with no dongle in the building.
//
// The port is two byte vectors and the loop is a variable, so the cases that
// are awkward to arrange on a bench are the cheap ones here: a dongle unplugged
// mid-line, a box that answers in fragments, a handler that closes the link
// while the loop is still walking the batch it came from.
#include "../src/boxcdc.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "../src/loopfake.h"
#include "harness.h"

using namespace octo;

namespace {

// The same shape as tests/test_hcilink.cc's, which is deliberate: this is the
// other protocol over the same wire, and if the fake had to differ then the
// abstraction in src/hciport.h would not be earning its place.
class FakePort : public hci::Port {
 public:
  Handle handle() const override {
    return Handle{-1, const_cast<FakePort*>(this)};
  }

  int read(uint8_t* buf, size_t len, double) override {
    // Buffered bytes first, then the break. That is what read(2) does -- bytes
    // already in the kernel are still readable after the far end has gone --
    // and a fake that reported the break first would let BoxLink pass a test
    // that a real unplugged dongle would fail.
    if (rx_.empty()) return broken_ ? -1 : 0;
    size_t n = len < rx_.size() ? len : rx_.size();
    std::memcpy(buf, rx_.data(), n);
    rx_.erase(rx_.begin(), rx_.begin() + n);
    return static_cast<int>(n);
  }

  bool write(const uint8_t* data, size_t len) override {
    if (write_fails_) return false;
    tx_.insert(tx_.end(), data, data + len);
    return true;
  }

  void close() override { closed_ = true; }
  bool is_open() const override { return !closed_; }
  std::string name() const override { return "/dev/fake"; }

  void feed(const std::string& bytes) {
    rx_.insert(rx_.end(), bytes.begin(), bytes.end());
  }
  void unplug() { broken_ = true; }
  void break_writes() { write_fails_ = true; }
  bool closed() const { return closed_; }

  std::string written() const { return std::string(tx_.begin(), tx_.end()); }

 private:
  std::vector<uint8_t> rx_;
  std::vector<uint8_t> tx_;
  bool broken_ = false;
  bool write_fails_ = false;
  bool closed_ = false;
};

struct Fixture {
  FakeLoop loop{1000.0};
  FakePort* port = nullptr;
  std::unique_ptr<BoxLink> link;
  std::vector<std::string> lines;
  std::vector<Message> messages;
  std::string closed_why;
  int closes = 0;

  Fixture() {
    std::unique_ptr<FakePort> owned(new FakePort());
    port = owned.get();
    link = BoxLink::attach(&loop, std::move(owned));
    link->on_line([this](const std::string& l) { lines.push_back(l); });
    link->on_message([this](const Message& m) { messages.push_back(m); });
    link->on_closed([this](const std::string& why) {
      closed_why = why;
      ++closes;
    });
  }

  // What the loop does when the port says it has something. FakeLoop does not
  // watch anything -- readiness is whatever a test says it is -- so this is
  // the test standing in for the kernel.
  void pump() {
    loop.deliver(loop.last_source(), kRead, 0.0);
    loop.advance(0.0005);
  }

  // ...and this is the port breaking rather than speaking, which arrives on a
  // different path: the loop's error callback, not its read one.
  void pump_error(const std::string& why) {
    (void)why;
    loop.fail(loop.last_source(), 0.0);
    loop.advance(0.0005);
  }
};

// ------------------------------------------------------------------ the tests

void test_a_greeting_arrives_as_a_message() {
  Fixture f;
  f.port->feed("hello proto=1 role=sync\n");
  f.pump();

  CHECK_EQ(static_cast<int>(f.lines.size()), 1);
  CHECK_STR(f.lines[0], "hello proto=1 role=sync");
  CHECK_EQ(static_cast<int>(f.messages.size()), 1);
  CHECK_STR(f.messages[0].verb, "hello");
  CHECK_STR(f.messages[0].get("role"), "sync");
}

// A serial port hands over whatever happened to be in the buffer, which is not
// a message boundary and has no reason to be. A reader that assumed one read
// was one line would work on a bench and fail on a busy box.
void test_a_message_split_across_reads_is_reassembled() {
  Fixture f;
  f.port->feed("status phase=idle ");
  f.pump();
  CHECK_EQ(static_cast<int>(f.messages.size()), 0);  // nothing complete yet

  f.port->feed("boxes=5\nsay text=hi\n");
  f.pump();
  CHECK_EQ(static_cast<int>(f.messages.size()), 2);
  CHECK_STR(f.messages[0].verb, "status");
  CHECK_STR(f.messages[0].get("boxes"), "5");
  CHECK_STR(f.messages[1].verb, "say");
}

void test_sending_adds_the_terminator() {
  Fixture f;
  Message msg;
  msg.verb = "time";
  msg.set_double("wall", 1700000000.5, 3);
  msg.set_int("zone", -25200);
  f.link->send(msg);

  const std::string out = f.port->written();
  CHECK_EQ(static_cast<int>(out.size() > 0), 1);
  CHECK_EQ(out[out.size() - 1], '\n');
  // Exactly one line, so a box reading this cannot see two commands or half of
  // one.
  CHECK_EQ(static_cast<int>(std::count(out.begin(), out.end(), '\n')), 1);

  Message back;
  std::string err;
  CHECK(decode(out.substr(0, out.size() - 1), &back, &err));
  CHECK_STR(back.verb, "time");
  CHECK_STR(back.get("zone"), "-25200");
}

// Unplugging is the common failure and the one with a trap in it: the last
// thing a box says before it goes is often the reason it went, so whatever
// arrived has to be delivered before the link is torn down.
void test_a_dongle_that_goes_away_delivers_what_it_already_said() {
  Fixture f;
  // Built rather than written out, because a real `say` carries a sentence and
  // the quoting that needs is src/boxmsg.h's business, not this test's.
  Message last;
  last.verb = "say";
  last.set("text", "last words");
  f.port->feed(encode(last) + "\n");
  f.pump();
  CHECK_EQ(static_cast<int>(f.messages.size()), 1);

  f.port->unplug();
  f.pump();
  CHECK_EQ(f.closes, 1);
  CHECK(!f.link->is_open());
  // Still the one message: it was delivered, not lost to the teardown.
  CHECK_EQ(static_cast<int>(f.messages.size()), 1);
  CHECK_STR(f.messages[0].get("text"), "last words");
}

void test_a_line_and_a_break_in_the_same_read_both_land() {
  Fixture f;
  f.port->feed("say text=goodbye\n");
  f.port->unplug();  // the read after the data returns -1
  f.pump();

  CHECK_EQ(static_cast<int>(f.messages.size()), 1);
  CHECK_STR(f.messages[0].get("text"), "goodbye");
  CHECK_EQ(f.closes, 1);
}

// The reason BoxLink carries a liveness flag. Hearing that the box has crashed
// is the ordinary reason to close the link, and the handler that decides that
// runs from inside the loop over the batch it arrived in.
void test_a_handler_that_closes_mid_batch_is_not_a_use_after_free() {
  Fixture f;
  f.link->on_message([&f](const Message& m) {
    f.messages.push_back(m);
    if (m.verb == "say") f.link->close("heard enough");
  });
  f.port->feed("hello proto=1\nsay text=stop\nstatus phase=idle\n");
  f.pump();

  // The third message is not delivered, because the link was closed by the
  // second. That is the point: no further handler runs after close.
  CHECK_EQ(static_cast<int>(f.messages.size()), 2);
  CHECK_STR(f.messages[1].verb, "say");
  CHECK_EQ(f.closes, 1);
  CHECK_STR(f.closed_why, "heard enough");
}

void test_closing_twice_reports_once() {
  Fixture f;
  f.link->close("first");
  f.link->close("second");
  CHECK_EQ(f.closes, 1);
  CHECK_STR(f.closed_why, "first");
  CHECK(f.port->closed());
}

// A write that half-succeeds would leave a fragment the box parses as a
// command, and there is no resynchronising from that short of closing.
void test_a_failed_write_closes_the_link() {
  Fixture f;
  f.port->break_writes();
  Message msg;
  msg.verb = "status";
  f.link->send(msg);

  CHECK_EQ(f.closes, 1);
  CHECK_STR(f.closed_why, "write failed");
  CHECK(!f.link->is_open());
}

void test_sending_on_a_closed_link_is_quiet() {
  Fixture f;
  f.link->close("done");
  Message msg;
  msg.verb = "status";
  f.link->send(msg);
  CHECK_STR(f.port->written(), "");
}

// A well-formed line that is not a well-formed message is a version skew or a
// corrupted byte, and it is counted rather than thrown -- but the line handler
// still sees it, because a person debugging wants the raw text.
void test_an_undecodable_line_is_counted_and_still_shown() {
  Fixture f;
  f.port->feed("   \n");
  f.pump();

  CHECK_EQ(static_cast<int>(f.lines.size()), 1);
  CHECK_EQ(static_cast<int>(f.messages.size()), 0);
  CHECK_EQ(static_cast<long long>(f.link->bad_lines()), 1LL);
}

void test_an_overlong_line_is_counted_and_dropped() {
  Fixture f;
  f.port->feed(std::string(LineReader::kMaxLine + 100, 'x'));
  f.port->feed("\nsay text=after\n");
  f.pump();

  CHECK_EQ(static_cast<long long>(f.link->long_lines()), 1LL);
  // ...and the link recovers: the next line is fine.
  CHECK_EQ(static_cast<int>(f.messages.size()), 1);
  CHECK_STR(f.messages[0].get("text"), "after");
}

}  // namespace

int main() {
  test_a_greeting_arrives_as_a_message();
  test_a_message_split_across_reads_is_reassembled();
  test_sending_adds_the_terminator();
  test_a_dongle_that_goes_away_delivers_what_it_already_said();
  test_a_line_and_a_break_in_the_same_read_both_land();
  test_a_handler_that_closes_mid_batch_is_not_a_use_after_free();
  test_closing_twice_reports_once();
  test_a_failed_write_closes_the_link();
  test_sending_on_a_closed_link_is_quiet();
  test_an_undecodable_line_is_counted_and_still_shown();
  test_an_overlong_line_is_counted_and_dropped();
  return octotest::report("test_boxcdc");
}
