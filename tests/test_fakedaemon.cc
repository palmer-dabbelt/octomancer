// The client halves, against a server, for the first time.
//
// Everything in this file has been testable in principle for as long as it has
// existed, and none of it has been tested, because doing so meant starting two
// daemons on a machine with a radio. So the parsers have been checked against
// strings a human typed, and the renderers against expectations a human wrote,
// and the two have never been introduced.
//
// That gap is not theoretical. src/proto.cc refuses a protocol version it does
// not know; src/control.cc was doing only half of that check until it was
// noticed by reading it. A round trip through a socket is what catches the
// class of bug where both sides are individually reasonable and disagree.
//
// The daemons here are the real server implementations with fake data poured
// in -- see tests/fakedaemon.h for why a hand-written mock of a wire format
// would be worse than none.
#include <cmath>
#include <string>
#include <vector>

#include "../src/client.h"
#include "../src/control.h"
#include "../src/devices.h"
#include "../src/boxmsg.h"
#include "../src/proto.h"
#include "fakedaemon.h"
#include "harness.h"

using octotest::FakeBenchDaemon;
using octotest::FakeControlDaemon;

namespace {

// ------------------------------------------------------------- octomancerd
//
// The bench, fetched the way every front-end fetches it.
void test_a_client_fetches_the_bench_over_a_socket() {
  FakeBenchDaemon daemon;
  daemon.listen(30.0);
  std::string err;
  CHECK(daemon.start(&err));
  CHECK_STR(err, "");

  octo::Snapshot snap;
  CHECK(octo::fetch(daemon.path(), &snap, &err, 2.0));
  CHECK_STR(err, "");

  // Five boxes, all of them heard from, which is what the standard bench is.
  CHECK_EQ((int)snap.device.size(), 5);
  CHECK_EQ(snap.live, 5);
  CHECK_STR(snap.radio, "poweredOn");

  // And the numbers survived the round trip. The bench is about 3.59 s behind
  // this Mac and agrees with itself to a few tens of milliseconds; a parser
  // that dropped a sign or a decimal point would still produce five devices.
  CHECK(snap.has_bench);
  CHECK_NEAR(snap.bench_offset, -3.59, 0.05);
  CHECK(snap.bench_spread > 0.005);
  CHECK(snap.bench_spread < 0.100);

  for (const octo::DeviceSnapshot& d : snap.device) {
    CHECK(!d.name.empty());
    CHECK(d.live);
    CHECK(d.rssi < 0);
    // Every box on the standard bench is within a tenth of a second of the
    // others, so any one of them is close to the bench offset.
    CHECK_NEAR(d.median_offset, -3.59, 0.05);
  }
  daemon.stop();
}

// A device id is not a name, and `forget` is the one thing this daemon can be
// asked to *do*. Both directions of that are checked here because the CLI's
// remove path has never met a server.
void test_forgetting_a_device_removes_it() {
  FakeBenchDaemon daemon;
  daemon.listen(10.0);
  std::string err;
  CHECK(daemon.start(&err));

  octo::Snapshot before;
  CHECK(octo::fetch(daemon.path(), &before, &err, 2.0));
  CHECK_EQ((int)before.device.size(), 5);
  if (before.device.empty()) return;
  const std::string id = before.device.front().id;

  std::string reply;
  CHECK(octo::query(daemon.path(), "forget " + id, &reply, &err, 2.0));

  octo::Snapshot after;
  CHECK(octo::fetch(daemon.path(), &after, &err, 2.0));
  CHECK_EQ((int)after.device.size(), 4);
  for (const octo::DeviceSnapshot& d : after.device) CHECK(d.id != id);
  daemon.stop();
}

// --------------------------------------------------------- octomancer-sync
//
// The camera surface, fetched and parsed the way the UI and the CLI do it.
void test_a_client_reads_camera_status_over_a_socket() {
  FakeControlDaemon daemon;
  std::string err;
  CHECK(daemon.start(&err));
  CHECK_STR(err, "");

  std::string reply;
  CHECK(octo::query(daemon.path(), "status", &reply, &err, 2.0));
  octo::Status status;
  CHECK(octo::parse_status(reply, &status, &err));
  CHECK_STR(err, "");
  CHECK_EQ((int)status.cameras.size(), 1);
  if (status.cameras.empty()) return;

  const octo::CameraStatus& c = status.cameras.front();
  CHECK_STR(c.id, "A:1EAE18A7");
  CHECK(c.present);
  CHECK(c.has_error);
  CHECK_NEAR(c.error_s, -0.25, 1e-6);
  CHECK(c.has_fps);
  CHECK_EQ(c.fps, 24);
  CHECK(c.writes_enabled);
  daemon.stop();
}

// The request handle, which is the part of this protocol layer 2 will have to
// broker and the part that has never been exercised end to end: queue a sync,
// get an id back, poll it, watch it finish.
void test_a_queued_request_can_be_followed_to_the_end() {
  FakeControlDaemon daemon;
  std::string err;
  CHECK(daemon.start(&err));

  std::string reply;
  CHECK(octo::query(daemon.path(), "sync camera=A:1EAE18A7", &reply, &err, 2.0));
  octo::RequestResult queued;
  CHECK(octo::parse_result(reply, &queued, &err));
  CHECK_STR(err, "");
  CHECK(queued.id > 0);
  CHECK(!octo::request_finished(queued.state));

  // The daemon side takes it, as its loop would.
  octo::Request taken;
  CHECK(daemon.control().take_request(&taken));
  CHECK_EQ(taken.id, queued.id);
  CHECK(taken.kind == octo::RequestKind::kSync);
  CHECK_EQ((int)taken.cameras.size(), 1);
  if (!taken.cameras.empty()) CHECK_STR(taken.cameras.front(), "A:1EAE18A7");

  // A client polling now sees it running, not finished. This is the state the
  // CLI and the app both sit in for half a second at a time, and neither has
  // ever seen it come from a socket.
  CHECK(octo::query(daemon.path(),
                    "result id=" + std::to_string(queued.id), &reply, &err, 2.0));
  octo::RequestResult running;
  CHECK(octo::parse_result(reply, &running, &err));
  CHECK(!octo::request_finished(running.state));

  daemon.control().finish(queued.id, true, "wrote RTC");

  CHECK(octo::query(daemon.path(),
                    "result id=" + std::to_string(queued.id), &reply, &err, 2.0));
  octo::RequestResult done;
  CHECK(octo::parse_result(reply, &done, &err));
  CHECK(octo::request_finished(done.state));
  CHECK(done.state == octo::RequestState::kDone);
  CHECK_STR(done.message, "wrote RTC");
  daemon.stop();
}

// Events, and the property that makes them worth having: a client that was not
// listening catches up rather than missing what happened. The sync daemon's
// own announcements cannot do this, which is entry 7 in doc/KNOWN_ISSUES.md,
// and this is the behaviour that entry says has to survive.
void test_events_replay_for_a_client_that_was_not_listening() {
  FakeControlDaemon daemon;
  std::string err;
  CHECK(daemon.start(&err));

  daemon.control().emit(octo::EventKind::kFirstSync, "A:1EAE18A7", "Studio",
                        "first");
  daemon.control().emit(octo::EventKind::kSyncFailed, "A:1EAE18A7", "Studio",
                        "second");

  std::string reply;
  CHECK(octo::query(daemon.path(), "events since=0", &reply, &err, 2.0));
  std::vector<octo::Event> events;
  int64_t next = 0;
  CHECK(octo::parse_events(reply, &events, &next, &err));
  CHECK_STR(err, "");
  CHECK_EQ((int)events.size(), 2);
  CHECK(next > 0);
  if (events.size() == 2) {
    CHECK_STR(events[0].message, "first");
    CHECK_STR(events[1].message, "second");
  }

  // Asking again from where it got to yields nothing, and then yields exactly
  // what happened next. A client that re-polled and got the whole log again
  // would double every notification the app raises.
  CHECK(octo::query(daemon.path(), "events since=" + std::to_string(next),
                    &reply, &err, 2.0));
  std::vector<octo::Event> none;
  CHECK(octo::parse_events(reply, &none, &next, &err));
  CHECK_EQ((int)none.size(), 0);

  daemon.control().emit(octo::EventKind::kCameraLost, "A:1EAE18A7", "Studio",
                        "third");
  CHECK(octo::query(daemon.path(), "events since=" + std::to_string(next),
                    &reply, &err, 2.0));
  std::vector<octo::Event> more;
  CHECK(octo::parse_events(reply, &more, &next, &err));
  CHECK_EQ((int)more.size(), 1);
  if (more.size() == 1) CHECK_STR(more[0].message, "third");
  daemon.stop();
}

// ------------------------------------------------------------- both at once
//
// What every front-end actually does: hold both sockets, ask both daemons, and
// merge. build_device_view has tests, but only ever against structures built
// in memory -- never against two that came off two sockets, which is the
// arrangement the program is in every time somebody runs it.
void test_the_merged_view_comes_off_two_sockets() {
  FakeBenchDaemon bench;
  bench.listen(30.0);
  FakeControlDaemon control;
  std::string err;
  CHECK(bench.start(&err));
  CHECK(control.start(&err));

  octo::Snapshot snap;
  CHECK(octo::fetch(bench.path(), &snap, &err, 2.0));
  std::string reply;
  CHECK(octo::query(control.path(), "status", &reply, &err, 2.0));
  octo::Status status;
  CHECK(octo::parse_status(reply, &status, &err));

  octo::DeviceSources from;
  from.bench = &snap;
  from.cameras = &status;
  const octo::DeviceView view = octo::build_device_view(from);

  // Five boxes and a camera, in one list, which is what a person sees.
  CHECK_EQ((int)view.rows.size(), 6);
  CHECK(view.has_canonical);
  CHECK_EQ(view.contributing, 5);
  CHECK_STR(view.radio, "poweredOn");

  int boxes = 0, cameras = 0;
  for (const octo::DeviceRow& r : view.rows) {
    if (r.kind == octo::DeviceKind::kTentacle) ++boxes;
    if (r.kind == octo::DeviceKind::kCamera) ++cameras;
  }
  CHECK_EQ(boxes, 5);
  CHECK_EQ(cameras, 1);

  // And it renders. A view that could be built but not printed would still be
  // a broken program.
  const std::string text = octo::render_devices(view, false, false);
  CHECK(text.find("no devices") == std::string::npos);
  CHECK(text.find("Krysta") != std::string::npos);

  bench.stop();
  control.stop();
}

// A daemon that is not answering is an ordinary Tuesday, not an error, and
// every front-end says so. Checked against a socket that genuinely is not
// there rather than against a null pointer, because those are different code
// paths and only one of them has ever run.
void test_a_daemon_that_is_not_there_is_not_an_error() {
  const std::string nowhere = octotest::socket_path("absent");
  ::unlink(nowhere.c_str());

  octo::Snapshot snap;
  std::string err;
  CHECK(!octo::fetch(nowhere, &snap, &err, 0.5));
  CHECK(!err.empty());

  // ...and the merge copes with having only one side.
  FakeControlDaemon control;
  CHECK(control.start(&err));
  std::string reply;
  CHECK(octo::query(control.path(), "status", &reply, &err, 2.0));
  octo::Status status;
  CHECK(octo::parse_status(reply, &status, &err));

  octo::DeviceSources from;
  from.bench = nullptr;  // octomancerd did not answer
  from.cameras = &status;
  const octo::DeviceView view = octo::build_device_view(from);
  CHECK_EQ((int)view.rows.size(), 1);
  // No bench means no canonical time and nothing to measure a camera against,
  // which the view has to say rather than imply with a zero.
  CHECK(!view.has_canonical);
  CHECK_STR(view.radio, "");
  control.stop();
}

// ------------------------------------------------------- the box protocol
//
// The seam nothing outside tests has ever connected to (doc/KNOWN_ISSUES.md
// entry 4). Checked here as a *client* would see it -- open a socket, read
// what arrives -- rather than by calling the daemon's methods, because the
// question this answers is "can something connect to this and get anywhere",
// and that has never had an answer.
void test_the_box_protocol_greets_and_answers_a_stranger() {
  octotest::FakeBoxDaemon daemon;
  std::string err;
  CHECK(daemon.start(&err));
  CHECK_STR(err, "");

  octotest::LineClient client;
  CHECK(client.connect(daemon.path(), &err));
  CHECK_STR(err, "");

  // The greeting arrives unasked, which is what makes the protocol usable by
  // something that connected without knowing what it had connected to.
  std::string line;
  CHECK(client.next(&line, 3.0));
  octo::Message hello;
  CHECK(octo::decode(line, &hello, &err));
  CHECK_STR(hello.verb, "hello");
  CHECK_STR(hello.get("role"), "sync");

  // And it answers a question. `devices` is the one a control daemon needs
  // first, being the roster it would merge.
  client.send("devices");
  int devices = 0;
  bool ended = false;
  for (int i = 0; i < 40 && !ended; ++i) {
    if (!client.next(&line, 3.0)) break;
    octo::Message msg;
    if (!octo::decode(line, &msg, &err)) continue;
    if (msg.verb == "dev") ++devices;
    if (msg.verb == "end" && msg.get("what") == "devices") ended = true;
  }
  CHECK(ended);
  CHECK_EQ(devices, 5);
  daemon.stop();
}

// The half that makes this protocol different from the other two: messages
// arrive that nobody asked for. A control daemon is built around that, and it
// has never been observed from outside the daemon that sends them.
void test_the_box_protocol_announces_without_being_asked() {
  octotest::FakeBoxDaemon daemon;
  std::string err;
  CHECK(daemon.start(&err));

  octotest::LineClient client;
  CHECK(client.connect(daemon.path(), &err));

  // Nothing is sent. Everything below arrives because the daemon decided to.
  int benches = 0;
  std::string line;
  for (int i = 0; i < 60 && benches < 2; ++i) {
    if (!client.next(&line, 3.0)) break;
    octo::Message msg;
    if (!octo::decode(line, &msg, &err)) continue;
    if (msg.verb != "bench") continue;
    ++benches;
    // ...and it is the bench, not an empty placeholder: five boxes about
    // 3.59 s out, which is the fake bench having reached the daemon through
    // the registry and back out over the socket.
    double offset = 0.0;
    if (msg.get_double("offset", &offset)) {
      CHECK_NEAR(offset, -3.59, 0.05);
    }
  }
  // Two, so that this is a repeating announcement rather than one greeting
  // that happened to look like one.
  CHECK_EQ(benches, 2);
  daemon.stop();
}

}  // namespace

int main() {
  test_a_client_fetches_the_bench_over_a_socket();
  test_forgetting_a_device_removes_it();
  test_a_client_reads_camera_status_over_a_socket();
  test_a_queued_request_can_be_followed_to_the_end();
  test_events_replay_for_a_client_that_was_not_listening();
  test_the_merged_view_comes_off_two_sockets();
  test_a_daemon_that_is_not_there_is_not_an_error();
  test_the_box_protocol_greets_and_answers_a_stranger();
  test_the_box_protocol_announces_without_being_asked();
  return octotest::report("test_fakedaemon");
}
