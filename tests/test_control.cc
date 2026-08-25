// The control surface: what a client may ask, and what comes back.
//
// The point of keeping this out of octomancer-sync.cc is that all of it can be
// exercised without a camera, a radio or a second process. The queue in
// particular has a failure mode that only shows up in front of someone -- a
// request taken by the loop and then abandoned sits in `running` forever while
// a client polls it -- and that is checked here in microseconds.
#include <cmath>
#include <atomic>
#include <string>
#include <thread>

#include <unistd.h>

#include "client.h"
#include "control.h"
#include "server.h"
#include "proto.h"
#include "harness.h"

using octo::CameraStatus;
using octo::Command;
using octo::Control;
using octo::EventKind;
using octo::Request;
using octo::RequestKind;
using octo::RequestResult;
using octo::RequestState;
using octo::Status;

namespace {

// ------------------------------------------------------------------ parsing

void test_parse_command_basics() {
  Command c = octo::parse_command("status");
  CHECK(c.ok);
  CHECK_EQ(c.verb, std::string("status"));
  CHECK(c.cameras.empty());

  // A bare verb with trailing whitespace is the same verb. `printf status |
  // nc -U` sends exactly this.
  c = octo::parse_command("  status \r\n");
  CHECK(c.ok);
  CHECK_EQ(c.verb, std::string("status"));

  c = octo::parse_command("");
  CHECK(c.ok);
  CHECK(c.verb.empty());
}

void test_parse_command_cameras_accumulate() {
  // Repeating camera= names several, rather than the last one winning. This
  // is the whole mechanism behind `--camera A --camera B`.
  const Command c = octo::parse_command("sync camera=alpha camera=beta");
  CHECK(c.ok);
  CHECK_EQ(c.verb, std::string("sync"));
  CHECK_EQ(static_cast<int>(c.cameras.size()), 2);
  CHECK_EQ(c.cameras[0], std::string("alpha"));
  CHECK_EQ(c.cameras[1], std::string("beta"));
}

void test_parse_command_unescapes_camera_names() {
  // Camera names are user-set and arrive from the air. A name with a space or
  // an '=' in it must survive the round trip, and must not be able to smuggle
  // a second token into the command.
  const std::string hostile = "A cam=x";
  const std::string line = "sync camera=" + octo::escape(hostile);
  const Command c = octo::parse_command(line);
  CHECK(c.ok);
  CHECK_EQ(static_cast<int>(c.cameras.size()), 1);
  CHECK_EQ(c.cameras[0], hostile);
}

void test_parse_command_rejects_nonsense_numbers() {
  Command c = octo::parse_command("source value=banana");
  CHECK(!c.ok);
  CHECK(!c.error.empty());

  // A partly-numeric value is not a number either: "1x" must not become 1.
  c = octo::parse_command("result id=1x");
  CHECK(!c.ok);

  c = octo::parse_command("events since=12");
  CHECK(c.ok);
  CHECK(c.has_since);
  CHECK_EQ(static_cast<int>(c.since), 12);
}

// ---------------------------------------------------------------- rendering

Status sample_status() {
  Status s;
  s.daemon.version = "0.1.0";
  s.daemon.started_wall = 1787684305.5;
  s.daemon.now_wall = 1787684400.25;
  s.daemon.poll_s = 60.0;
  s.daemon.dry_run = false;
  s.daemon.socket_path = "/tmp/x.sock";

  s.bench.has = true;
  s.bench.source = "tentacle";
  s.bench.boxes = 3;
  s.bench.offset_s = -6.2112;
  s.bench.spread_s = 0.0115;
  s.bench.daemon_reachable = true;

  CameraStatus c;
  c.id = "09EE26AF-D630";
  c.name = "A:1EAE18A7";
  c.present = true;
  c.connected = false;
  c.has_error = true;
  c.error_s = -0.0777;
  c.timecode = "12:01:17:21";
  c.has_fps = true;
  c.fps = 24;
  c.recording = false;
  c.has_source = true;
  c.source = 0;
  c.action = "skip:rate-limited";
  c.has_last_write = true;
  c.last_write_wall = 1787684332.2;
  c.writes = 2;
  c.writes_enabled = true;
  c.has_lead = true;
  c.lead_s = 0.14217;
  c.has_drift = true;
  c.drift_ppm = 27.08;
  s.cameras.push_back(c);
  return s;
}

void test_status_round_trip() {
  const Status in = sample_status();
  Status out;
  std::string err;
  CHECK(octo::parse_status(octo::render_status(in), &out, &err));

  CHECK_EQ(out.daemon.version, in.daemon.version);
  CHECK_NEAR(out.daemon.poll_s, in.daemon.poll_s, 1e-6);
  CHECK_EQ(out.daemon.socket_path, in.daemon.socket_path);

  CHECK(out.bench.has);
  CHECK_EQ(out.bench.boxes, in.bench.boxes);
  CHECK_NEAR(out.bench.offset_s, in.bench.offset_s, 1e-4);
  CHECK(out.bench.daemon_reachable);

  CHECK_EQ(static_cast<int>(out.cameras.size()), 1);
  const CameraStatus& c = out.cameras[0];
  CHECK_EQ(c.id, in.cameras[0].id);
  CHECK_EQ(c.name, in.cameras[0].name);
  CHECK(c.present);
  CHECK(c.has_error);
  CHECK_NEAR(c.error_s, -0.0777, 1e-4);
  CHECK_EQ(c.timecode, std::string("12:01:17:21"));
  CHECK(c.has_fps);
  CHECK_EQ(c.fps, 24);
  CHECK(c.has_source);
  CHECK_EQ(static_cast<int>(c.source), 0);
  CHECK_EQ(c.action, std::string("skip:rate-limited"));
  CHECK_EQ(c.writes, 2);
  // The permission flag and the write *count* are different fields that once
  // shared a key on the wire, so the count silently read back as 1.
  CHECK(c.writes_enabled);
  CHECK(c.has_lead);
  CHECK_NEAR(c.lead_s, 0.14217, 1e-4);
}

void test_status_distinguishes_absent_fields() {
  // A camera that has said nothing about its timecode source must not come
  // back looking like one that said 0. That difference is the whole reason
  // the sync gate treats silence as permission.
  Status in;
  CameraStatus c;
  c.id = "bare";
  in.cameras.push_back(c);

  Status out;
  std::string err;
  CHECK(octo::parse_status(octo::render_status(in), &out, &err));
  CHECK_EQ(static_cast<int>(out.cameras.size()), 1);
  CHECK(!out.cameras[0].has_source);
  CHECK(!out.cameras[0].has_error);
  CHECK(!out.cameras[0].has_lead);
  CHECK(!out.cameras[0].has_last_write);
}

void test_hostile_camera_name_survives_rendering() {
  Status in;
  CameraStatus c;
  c.id = "id";
  c.name = "Bench 2 = the good one\nend";  // spaces, '=', and a fake trailer
  in.cameras.push_back(c);

  Status out;
  std::string err;
  CHECK(octo::parse_status(octo::render_status(in), &out, &err));
  CHECK_EQ(static_cast<int>(out.cameras.size()), 1);
  CHECK_EQ(out.cameras[0].name, c.name);
}

void test_reply_errors_are_reported_not_parsed() {
  Status out;
  std::string err;
  CHECK(!octo::parse_status(octo::render_error("no such camera"), &out, &err));
  CHECK_EQ(err, std::string("no such camera"));

  // A truncated reply -- a daemon killed mid-write -- must fail rather than
  // being read as a status with no cameras in it.
  CHECK(!octo::parse_status("octomancer 1\ndaemon version=1\n", &out, &err));
  CHECK(!err.empty());

  CHECK(!octo::parse_status("hello\n", &out, &err));
}

void test_result_round_trip() {
  RequestResult in;
  in.id = 7;
  in.state = RequestState::kDone;
  in.message = "corrected to within -12ms";
  in.updated_wall = 1787684400.0;

  RequestResult out;
  std::string err;
  CHECK(octo::parse_result(octo::render_result(in), &out, &err));
  CHECK_EQ(static_cast<int>(out.id), 7);
  CHECK(out.state == RequestState::kDone);
  CHECK_EQ(out.message, in.message);
  CHECK(octo::request_finished(out.state));
}

void test_unknown_state_does_not_masquerade_as_success() {
  // A newer daemon inventing a state name must not be read as done. Anything
  // unrecognised is unknown, which request_finished() ends the wait on rather
  // than reporting as a success that did not happen.
  RequestResult out;
  std::string err;
  const std::string wire =
      "octomancer 1\nresult id=3 state=deferred finished=0\nend\n";
  CHECK(octo::parse_result(wire, &out, &err));
  CHECK(out.state == RequestState::kUnknown);
  CHECK(!(out.state == RequestState::kDone));
}

void test_events_round_trip() {
  std::vector<octo::Event> in;
  octo::Event a;
  a.seq = 1;
  a.kind = EventKind::kFirstSync;
  a.camera_id = "id-a";
  a.camera_name = "A:1EAE18A7";
  a.message = "corrected to within +3ms";
  a.wall = 1787684400.0;
  in.push_back(a);

  octo::Event b;
  b.seq = 2;
  b.kind = EventKind::kCameraLost;
  b.camera_id = "id-a";
  in.push_back(b);

  std::vector<octo::Event> out;
  int64_t next = 0;
  std::string err;
  CHECK(octo::parse_events(octo::render_events(in, 3), &out, &next, &err));
  CHECK_EQ(static_cast<int>(next), 3);
  CHECK_EQ(static_cast<int>(out.size()), 2);
  CHECK(out[0].kind == EventKind::kFirstSync);
  CHECK_EQ(out[0].message, a.message);
  CHECK(out[1].kind == EventKind::kCameraLost);
}

void test_unknown_event_kind_is_skipped_not_guessed() {
  // Same reasoning as the unknown state: a UI that guesses would notify
  // someone about something it cannot name.
  const std::string wire =
      "octomancer 1\nevents next=9\n"
      "event seq=8 kind=lens-fell-off id=x name=y wall=1.0\nend\n";
  std::vector<octo::Event> out;
  int64_t next = 0;
  std::string err;
  CHECK(octo::parse_events(wire, &out, &next, &err));
  CHECK_EQ(static_cast<int>(next), 9);
  CHECK(out.empty());
}

// ------------------------------------------------------------------ Control

// The reply to a queueing command carries the id to ask after.
int64_t queued_id(const std::string& reply) {
  RequestResult r;
  std::string err;
  if (!octo::parse_result(reply, &r, &err)) return -1;
  return r.id;
}

void test_queue_take_finish() {
  Control control;
  const int64_t id = queued_id(control.handle("sync"));
  CHECK(id > 0);
  CHECK_EQ(control.queued_count(), 1);

  Request req;
  CHECK(control.take_request(&req));
  CHECK_EQ(static_cast<int>(req.id), static_cast<int>(id));
  CHECK(req.kind == RequestKind::kSync);
  CHECK(req.cameras.empty());
  CHECK_EQ(control.queued_count(), 0);

  // Taken once, and only once: a second drain must not run the same sync
  // twice.
  Request again;
  CHECK(!control.take_request(&again));

  RequestResult r;
  std::string err;
  CHECK(octo::parse_result(control.handle("result id=" + std::to_string(id)),
                           &r, &err));
  CHECK(r.state == RequestState::kRunning);

  control.finish(id, true, "corrected");
  CHECK(octo::parse_result(control.handle("result id=" + std::to_string(id)),
                           &r, &err));
  CHECK(r.state == RequestState::kDone);
  CHECK_EQ(r.message, std::string("corrected"));
}

void test_requeue_running_after_an_abandoned_pass() {
  // The failure this exists to prevent: the loop takes a request, is
  // interrupted before finishing it, and a client polls `running` forever.
  Control control;
  const int64_t id = queued_id(control.handle("sync"));
  Request req;
  CHECK(control.take_request(&req));
  control.requeue_running();
  CHECK_EQ(control.queued_count(), 1);

  Request second;
  CHECK(control.take_request(&second));
  CHECK_EQ(static_cast<int>(second.id), static_cast<int>(id));
}

void test_unknown_request_id_is_answered() {
  // Not an error: a client asking after a request that has aged out of the
  // ring deserves an answer it can stop waiting on.
  Control control;
  RequestResult r;
  std::string err;
  CHECK(octo::parse_result(control.handle("result id=999"), &r, &err));
  CHECK(r.state == RequestState::kUnknown);
  CHECK(octo::request_finished(r.state));
}

void test_source_values_are_validated_at_the_door() {
  Control control;
  // The camera clamps anything above 1 to 1, so accepting 2 would report a
  // success that did not happen.
  std::string err;
  Status ignored;
  CHECK(!octo::parse_status(control.handle("source value=2"), &ignored, &err));
  CHECK(!octo::parse_status(control.handle("source"), &ignored, &err));
  CHECK_EQ(control.queued_count(), 0);

  RequestResult r;
  CHECK(octo::parse_result(control.handle("source value=1"), &r, &err));
  CHECK_EQ(control.queued_count(), 1);
  Request req;
  CHECK(control.take_request(&req));
  CHECK(req.kind == RequestKind::kSetSource);
  CHECK_EQ(static_cast<int>(req.source), 1);
}

void test_request_carries_its_cameras() {
  Control control;
  control.handle("sync camera=alpha camera=beta");
  Request req;
  CHECK(control.take_request(&req));
  CHECK_EQ(static_cast<int>(req.cameras.size()), 2);
  CHECK_EQ(req.cameras[1], std::string("beta"));
}

void test_events_are_delivered_once() {
  Control control;
  control.emit(EventKind::kFirstSync, "id", "name", "one");
  control.emit(EventKind::kSyncFailed, "id", "name", "two");

  std::vector<octo::Event> got;
  int64_t next = 0;
  std::string err;
  CHECK(octo::parse_events(control.handle("events since=0"), &got, &next, &err));
  CHECK_EQ(static_cast<int>(got.size()), 2);

  // Asking again from the cursor returns nothing, which is what stops a UI
  // notifying about the same failure every time it refreshes.
  CHECK(octo::parse_events(control.handle("events since=" +
                                          std::to_string(next)),
                           &got, &next, &err));
  CHECK(got.empty());

  control.emit(EventKind::kCameraLost, "id", "name", "three");
  CHECK(octo::parse_events(control.handle("events since=" +
                                          std::to_string(next - 1)),
                           &got, &next, &err));
  CHECK_EQ(static_cast<int>(got.size()), 1);
  CHECK(got[0].kind == EventKind::kCameraLost);
}

void test_cameras_persist_but_go_absent() {
  // A camera that has stopped advertising must stay in the list. This camera
  // duty-cycles its adverts with gaps of tens of seconds, and a list that
  // dropped it would flicker.
  Control control;
  CameraStatus c;
  c.id = "id";
  c.name = "A";
  c.present = true;
  c.connected = true;
  control.publish_camera(c);

  control.set_present("id", false);

  Status s;
  std::string err;
  CHECK(octo::parse_status(control.handle("status"), &s, &err));
  CHECK_EQ(static_cast<int>(s.cameras.size()), 1);
  CHECK(!s.cameras[0].present);
  // Absent implies disconnected; a stale `connected` would have a UI offering
  // to sync something that is not there.
  CHECK(!s.cameras[0].connected);

  // Publishing the same id updates rather than duplicating.
  c.present = true;
  control.publish_camera(c);
  CHECK(octo::parse_status(control.handle("status"), &s, &err));
  CHECK_EQ(static_cast<int>(s.cameras.size()), 1);
  CHECK(s.cameras[0].present);
}

// A camera's name is only learned by scanning, and most cycles skip the scan
// and connect straight to a known identifier. Publishing the later, nameless
// picture must not blank a name that was already learned -- it showed up as
// "(unnamed)" from the second cycle onwards.
void test_a_learned_name_is_not_blanked() {
  Control control;
  CameraStatus named;
  named.id = "id";
  named.name = "A:1EAE18A7";
  named.present = true;
  control.publish_camera(named);

  CameraStatus nameless;
  nameless.id = "id";
  nameless.present = true;
  nameless.has_error = true;
  nameless.error_s = -0.078;
  control.publish_camera(nameless);

  Status s;
  std::string err;
  CHECK(octo::parse_status(control.handle("status"), &s, &err));
  CHECK_EQ(static_cast<int>(s.cameras.size()), 1);
  CHECK_EQ(s.cameras[0].name, std::string("A:1EAE18A7"));
  // ...while everything else still updates.
  CHECK(s.cameras[0].has_error);
  CHECK_NEAR(s.cameras[0].error_s, -0.078, 1e-4);

  // A genuinely new name still replaces the old one.
  CameraStatus renamed;
  renamed.id = "id";
  renamed.name = "B:CAM2";
  control.publish_camera(renamed);
  CHECK(octo::parse_status(control.handle("status"), &s, &err));
  CHECK_EQ(s.cameras[0].name, std::string("B:CAM2"));
}

void test_unknown_command_is_an_error_not_a_status() {
  Control control;
  Status s;
  std::string err;
  CHECK(!octo::parse_status(control.handle("frobnicate"), &s, &err));
  CHECK(!err.empty());
}

void test_ping() {
  Control control;
  const std::string reply = control.handle("ping");
  CHECK(reply.find("pong") != std::string::npos);
}

// -------------------------------------------------------- over a real socket
//
// Everything above tests Control in isolation. This runs the whole path a
// client actually takes -- Server on one thread, octo::query from another,
// over a Unix socket -- because the seam between them is where a line protocol
// usually breaks: a reply that never terminates, a request read in two pieces,
// a handler that blocks the accept loop.

void test_round_trip_over_a_socket() {
  Control control;
  {
    octo::DaemonStatus d;
    d.version = "test";
    d.poll_s = 60.0;
    control.set_daemon(d);
  }
  CameraStatus cam;
  cam.id = "id-1";
  cam.name = "A:1EAE18A7";
  cam.present = true;
  cam.has_error = true;
  cam.error_s = -0.078;
  control.publish_camera(cam);

  const std::string path =
      "/tmp/octo-test-" + std::to_string(getpid()) + ".sock";
  ::unlink(path.c_str());

  octo::Server server(
      [&control](const std::string& line) { return control.handle(line); },
      path);
  std::string err;
  CHECK(server.start(&err));

  std::atomic<bool> running{true};
  std::thread serving([&server, &running] {
    while (running.load()) server.serve(50);
  });

  // A status query comes back parseable, with what was published in it.
  std::string reply;
  Status s;
  CHECK(octo::query(path, "status", &reply, &err, 5.0));
  CHECK(octo::parse_status(reply, &s, &err));
  CHECK_EQ(static_cast<int>(s.cameras.size()), 1);
  CHECK_EQ(s.cameras[0].name, std::string("A:1EAE18A7"));
  CHECK_NEAR(s.cameras[0].error_s, -0.078, 1e-4);

  // Queue a request over the wire, and see it arrive on the daemon side.
  RequestResult r;
  CHECK(octo::query(path, "sync camera=id-1", &reply, &err, 5.0));
  CHECK(octo::parse_result(reply, &r, &err));
  CHECK(r.state == RequestState::kQueued);

  Request req;
  CHECK(control.take_request(&req));
  CHECK_EQ(static_cast<int>(req.cameras.size()), 1);
  CHECK_EQ(req.cameras[0], std::string("id-1"));

  // ...and the answer comes back through the same door.
  control.finish(req.id, true, "corrected to within +4ms");
  CHECK(octo::query(path, "result id=" + std::to_string(r.id), &reply, &err,
                    5.0));
  CHECK(octo::parse_result(reply, &r, &err));
  CHECK(r.state == RequestState::kDone);
  CHECK_EQ(r.message, std::string("corrected to within +4ms"));

  // An unknown command over the wire is an error a client can read, not a
  // dropped connection.
  CHECK(octo::query(path, "frobnicate", &reply, &err, 5.0));
  Status ignored;
  CHECK(!octo::parse_status(reply, &ignored, &err));

  // Several clients in a row, because the server drops each one after its
  // reply and getting that wrong leaves the second hanging.
  for (int i = 0; i < 5; ++i) {
    CHECK(octo::query(path, "ping", &reply, &err, 5.0));
    CHECK(reply.find("pong") != std::string::npos);
  }

  running = false;
  serving.join();
  server.shutdown();
}

void test_second_daemon_is_refused_the_socket() {
  // Two sync daemons on one socket would each answer half the requests. The
  // second one has to fail to start rather than silently take the path over.
  const std::string path =
      "/tmp/octo-test-dup-" + std::to_string(getpid()) + ".sock";
  ::unlink(path.c_str());

  Control control;
  octo::Server first(
      [&control](const std::string& line) { return control.handle(line); },
      path);
  std::string err;
  CHECK(first.start(&err));

  std::atomic<bool> running{true};
  std::thread serving([&first, &running] {
    while (running.load()) first.serve(50);
  });

  octo::Server second(
      [&control](const std::string& line) { return control.handle(line); },
      path);
  std::string err2;
  CHECK(!second.start(&err2));
  CHECK(!err2.empty());

  running = false;
  serving.join();
  first.shutdown();
}

}  // namespace

int main() {
  test_parse_command_basics();
  test_parse_command_cameras_accumulate();
  test_parse_command_unescapes_camera_names();
  test_parse_command_rejects_nonsense_numbers();
  test_status_round_trip();
  test_status_distinguishes_absent_fields();
  test_hostile_camera_name_survives_rendering();
  test_reply_errors_are_reported_not_parsed();
  test_result_round_trip();
  test_unknown_state_does_not_masquerade_as_success();
  test_events_round_trip();
  test_unknown_event_kind_is_skipped_not_guessed();
  test_queue_take_finish();
  test_requeue_running_after_an_abandoned_pass();
  test_unknown_request_id_is_answered();
  test_source_values_are_validated_at_the_door();
  test_request_carries_its_cameras();
  test_events_are_delivered_once();
  test_cameras_persist_but_go_absent();
  test_a_learned_name_is_not_blanked();
  test_unknown_command_is_an_error_not_a_status();
  test_ping();
  test_round_trip_over_a_socket();
  test_second_daemon_is_refused_the_socket();
  return octotest::report("test_control");
}
