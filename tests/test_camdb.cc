// The per-camera database, and the send lead learned from it.
//
// Two things here can lose data and so are tested against a real filesystem
// rather than a mock: compaction, which rewrites the file, and replay, which
// is the only thing standing between a truncated line and a lost history.
#include "camdb.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "camsync.h"
#include "harness.h"

namespace {

std::string scratch(const char* name) {
  const char* base = getenv("TMPDIR");
  std::string dir = base && *base ? base : "/tmp";
  if (dir.back() != '/') dir += '/';
  dir += "octo-camdb-" + std::to_string(::getpid()) + "-" + name;
  ::mkdir(dir.c_str(), 0700);
  return dir;
}

void remove_tree(const std::string& dir) {
  ::unlink((dir + "/db.json").c_str());
  ::unlink((dir + "/db.json.tmp").c_str());
  ::rmdir(dir.c_str());
}

std::string read_all(const std::string& path) {
  FILE* f = ::fopen(path.c_str(), "rb");
  if (f == nullptr) return std::string();
  std::string out;
  char buf[4096];
  size_t got;
  while ((got = ::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, got);
  ::fclose(f);
  return out;
}

int count_lines(const std::string& text, const std::string& needle) {
  int n = 0;
  size_t at = 0;
  while ((at = text.find(needle, at)) != std::string::npos) {
    ++n;
    at += needle.size();
  }
  return n;
}

octo::WriteSample sample(double wall, double lead, double after) {
  octo::WriteSample s;
  s.wall = wall;
  s.error_before_s = -0.4;
  s.error_after_s = after;
  s.lead_used_s = lead;
  s.latency_s = 0.05;
  s.fps = 24;
  s.bias = 0;
  s.verified = true;
  s.timing_ok = true;
  return s;
}

// --------------------------------------------------------------- the maths

// The whole point of the exercise: a write that lands late means the lead was
// too short by exactly the amount left over.
void test_apply_delay_is_lead_plus_what_is_left() {
  // The real observation from this bench: 50ms of lead, and the camera still
  // ended up 99ms behind. So it actually takes 149ms to act.
  CHECK_NEAR(octo::observed_apply_delay(0.05, -0.0989), 0.1489, 1e-6);

  // A camera that landed early would mean the lead was too long.
  CHECK_NEAR(octo::observed_apply_delay(0.20, 0.05), 0.15, 1e-6);

  // And one that landed dead on has a delay equal to the lead, which is the
  // fixed point the whole loop is looking for.
  CHECK_NEAR(octo::observed_apply_delay(0.1489, 0.0), 0.1489, 1e-6);
}

void test_a_lead_is_not_guessed_from_too_little() {
  octo::SyncOptions opt;
  opt.min_lead_samples = 3;
  CHECK(!octo::estimate_lead({}, opt).has);
  CHECK(!octo::estimate_lead({0.15}, opt).has);
  CHECK(!octo::estimate_lead({0.15, 0.14}, opt).has);
  CHECK(octo::estimate_lead({0.15, 0.14, 0.16}, opt).has);
}

void test_the_lead_is_a_median_not_a_mean() {
  octo::SyncOptions opt;
  // One write landed during a mode change and is wildly out. A mean would be
  // dragged to 0.25s by it; the median must not care.
  const std::vector<double> delays = {0.15, 0.14, 0.16, 0.15, 1.00};
  const octo::LeadEstimate est = octo::estimate_lead(delays, opt);
  CHECK(est.has);
  CHECK_NEAR(est.lead_s, 0.15, 1e-9);
}

void test_only_the_recent_window_counts() {
  octo::SyncOptions opt;
  opt.lead_window = 3;
  // The camera used to take 400ms and now takes 150ms -- a firmware update,
  // say. Only the last three observations should be reflected.
  const std::vector<double> delays = {0.40, 0.40, 0.40, 0.40,
                                      0.15, 0.15, 0.15};
  const octo::LeadEstimate est = octo::estimate_lead(delays, opt);
  CHECK(est.has);
  CHECK_EQ(est.samples, 3);
  CHECK_NEAR(est.lead_s, 0.15, 1e-9);
}

void test_the_clamp_holds() {
  octo::SyncOptions opt;
  opt.max_lead = 0.5;
  // A camera that appears to take four seconds to act is a camera that is
  // wrong about something, and sleeping four seconds before every write would
  // make the daemon worse, not better.
  const octo::LeadEstimate high = octo::estimate_lead({4.0, 4.0, 4.0}, opt);
  CHECK(high.has);
  CHECK_NEAR(high.lead_s, 0.5, 1e-9);

  // Negative would mean the camera acted before being asked.
  const octo::LeadEstimate low = octo::estimate_lead({-1.0, -1.0, -1.0}, opt);
  CHECK(low.has);
  CHECK_NEAR(low.lead_s, 0.0, 1e-9);
}

void test_adaptation_can_be_switched_off() {
  octo::SyncOptions opt;
  opt.adapt_lead = false;
  CHECK(!octo::estimate_lead({0.15, 0.15, 0.15}, opt).has);

  octo::SyncState state;
  state.lead.has = true;
  state.lead.lead_s = 0.20;
  // With adaptation off the configured lead wins even if something learned one.
  CHECK_NEAR(octo::effective_lead(opt, state), opt.lead, 1e-9);
  opt.adapt_lead = true;
  CHECK_NEAR(octo::effective_lead(opt, state), 0.20, 1e-9);
}

// A power cycle invalidates what the clock reads, not how long a write takes
// to reach it. This is the distinction the whole design turns on.
void test_a_power_cycle_does_not_forget_the_lead() {
  octo::SyncState state;
  state.lead.has = true;
  state.lead.lead_s = 0.149;
  state.rtc_bias = -75;
  state.drift.has = true;
  state.drift.ppm = -24.8;

  octo::forget_drift(&state);

  CHECK(!state.drift.has);       // the clock's value is now unknown
  CHECK(state.lead.has);         // but the path to it has not changed
  CHECK_NEAR(state.lead.lead_s, 0.149, 1e-9);
  CHECK_EQ(state.rtc_bias, -75);
}

// --------------------------------------------------------- store and recall

void test_what_is_written_comes_back() {
  const std::string dir = scratch("roundtrip");
  const std::string path = dir + "/db.json";
  octo::CamDbOptions dbopt;
  std::string err;

  {
    octo::CamDb db;
    CHECK(db.open(path, dbopt, &err));
    CHECK(db.note_seen("cam-a", "A:1EAE18A7", 24, true, &err));
    CHECK(db.record_write("cam-a", sample(1000.0, 0.05, -0.10), &err));
    CHECK(db.record_write("cam-a", sample(1060.0, 0.05, -0.09), &err));
    CHECK(db.learn("cam-a", true, -75, true, 0.149, true, -24.8, 3600.0));
    CHECK(db.record_params("cam-a", &err));
  }

  octo::CamDb again;
  CHECK(again.open(path, dbopt, &err));
  const octo::CameraRecord* rec = again.find("cam-a");
  CHECK(rec != nullptr);
  if (rec != nullptr) {
    CHECK_STR(rec->name, "A:1EAE18A7");
    CHECK_EQ(rec->bias, -75);
    CHECK(rec->has_bias);
    CHECK_NEAR(rec->lead_s, 0.149, 1e-6);
    CHECK_NEAR(rec->drift_ppm, -24.8, 1e-3);
    CHECK_EQ(rec->samples.size(), size_t(2));
    CHECK_EQ(rec->writes, uint64_t(2));
    CHECK_EQ(rec->sessions, uint64_t(1));
  }
  remove_tree(dir);
}

// A bias of zero is a real, learned value. If it round-tripped as "unknown"
// the daemon would re-learn it every restart, and re-learning costs a write.
void test_a_learned_zero_is_not_the_same_as_unknown() {
  std::map<std::string, octo::CameraRecord> out;

  octo::CameraRecord known;
  known.id = "cam-a";
  known.has_bias = true;
  known.bias = 0;
  octo::replay_camera_db(octo::camera_line(known) + "\n", 1000, &out);
  CHECK(out["cam-a"].has_bias);

  octo::CameraRecord unknown;
  unknown.id = "cam-b";
  octo::replay_camera_db(octo::camera_line(unknown) + "\n", 1000, &out);
  CHECK(!out["cam-b"].has_bias);
}

void test_samples_are_bounded_per_camera() {
  const std::string dir = scratch("bounded");
  const std::string path = dir + "/db.json";
  octo::CamDbOptions dbopt;
  dbopt.max_samples = 10;
  std::string err;

  octo::CamDb db;
  CHECK(db.open(path, dbopt, &err));
  for (int i = 0; i < 50; ++i) {
    CHECK(db.record_write("cam-a", sample(1000.0 + i, 0.05, -0.10), &err));
  }
  const octo::CameraRecord* rec = db.find("cam-a");
  CHECK(rec != nullptr);
  if (rec != nullptr) {
    CHECK_EQ(rec->samples.size(), size_t(10));
    // The ones kept are the newest, not the first ten seen.
    CHECK_NEAR(rec->samples.front().wall, 1040.0, 1e-9);
    CHECK_NEAR(rec->samples.back().wall, 1049.0, 1e-9);
    // ...and the running total still counts every write ever made.
    CHECK_EQ(rec->writes, uint64_t(50));
  }
  remove_tree(dir);
}

// first_seen/last_seen bracket everything known about a body, so they only
// widen. Backfilling an old write must not make a camera look as though it was
// last seen before it was last seen.
void test_the_seen_window_only_widens() {
  const std::string dir = scratch("window");
  const std::string path = dir + "/db.json";
  octo::CamDbOptions dbopt;
  std::string err;

  octo::CamDb db;
  CHECK(db.open(path, dbopt, &err));
  CHECK(db.record_write("cam-a", sample(2000.0, 0.05, -0.10), &err));
  CHECK(db.record_write("cam-a", sample(1000.0, 0.05, -0.10), &err));  // older
  const octo::CameraRecord* rec = db.find("cam-a");
  CHECK(rec != nullptr);
  if (rec != nullptr) {
    CHECK_NEAR(rec->first_seen_wall, 1000.0, 1e-9);
    CHECK_NEAR(rec->last_seen_wall, 2000.0, 1e-9);
  }
  remove_tree(dir);
}

void test_one_camera_cannot_evict_another() {
  const std::string dir = scratch("twocams");
  const std::string path = dir + "/db.json";
  octo::CamDbOptions dbopt;
  dbopt.max_samples = 5;
  std::string err;

  octo::CamDb db;
  CHECK(db.open(path, dbopt, &err));
  CHECK(db.record_write("quiet", sample(1.0, 0.05, -0.10), &err));
  for (int i = 0; i < 100; ++i) {
    CHECK(db.record_write("busy", sample(1000.0 + i, 0.05, -0.10), &err));
  }
  const octo::CameraRecord* quiet = db.find("quiet");
  CHECK(quiet != nullptr);
  if (quiet != nullptr) CHECK_EQ(quiet->samples.size(), size_t(1));
  remove_tree(dir);
}

// ---------------------------------------------------------- the compaction

void test_compaction_keeps_the_values_worth_keeping() {
  const std::string dir = scratch("compact");
  const std::string path = dir + "/db.json";
  octo::CamDbOptions dbopt;
  dbopt.max_samples = 5;
  dbopt.compact_min_bytes = 0.0;  // compact as soon as the ratio says so
  dbopt.compact_factor = 2.0;
  std::string err;

  octo::CamDb db;
  CHECK(db.open(path, dbopt, &err));
  CHECK(db.note_seen("cam-a", "A:1EAE18A7", 24, true, &err));
  CHECK(db.learn("cam-a", true, -75, true, 0.149, false, 0.0, 0.0));
  CHECK(db.record_params("cam-a", &err));
  for (int i = 0; i < 200; ++i) {
    CHECK(db.record_write("cam-a", sample(1000.0 + i, 0.05, -0.10), &err));
  }
  CHECK(db.compactions() > 0);

  // The file must not have grown without bound: after compaction it holds the
  // retained samples and little else.
  const std::string text = read_all(path);
  CHECK(count_lines(text, "\"t\":\"write\"") <= 6);

  // ...and the learned parameters survived the rewrite, which is the whole
  // reason the rewrite has to dump them first.
  octo::CamDb again;
  CHECK(again.open(path, dbopt, &err));
  const octo::CameraRecord* rec = again.find("cam-a");
  CHECK(rec != nullptr);
  if (rec != nullptr) {
    CHECK_EQ(rec->bias, -75);
    CHECK_NEAR(rec->lead_s, 0.149, 1e-6);
    CHECK_EQ(rec->samples.size(), size_t(5));
    CHECK_NEAR(rec->samples.back().wall, 1199.0, 1e-9);
    CHECK_STR(rec->name, "A:1EAE18A7");
  }
  remove_tree(dir);
}

// The rule that keeps appends amortised: a database whose live set is already
// near a fixed threshold would otherwise rewrite itself on almost every write.
void test_compaction_does_not_thrash() {
  const std::string dir = scratch("thrash");
  const std::string path = dir + "/db.json";
  octo::CamDbOptions dbopt;
  dbopt.max_samples = 50;
  dbopt.compact_min_bytes = 0.0;
  dbopt.compact_factor = 2.0;
  std::string err;

  octo::CamDb db;
  CHECK(db.open(path, dbopt, &err));
  for (int i = 0; i < 400; ++i) {
    CHECK(db.record_write("cam-a", sample(1000.0 + i, 0.05, -0.10), &err));
  }
  // 400 writes against a 50-sample live set: with a doubling rule this is a
  // handful of rewrites, not one per append.
  CHECK(db.compactions() > 0);
  CHECK(db.compactions() < 20);
  remove_tree(dir);
}

void test_a_truncated_line_costs_one_record_not_the_history() {
  // Exactly what a lid closing mid-append leaves behind: a good file with a
  // half-written last line.
  octo::CameraRecord rec;
  rec.id = "cam-a";
  rec.name = "A:1EAE18A7";
  rec.has_bias = true;
  rec.bias = -75;

  std::string text = octo::camera_line(rec) + "\n";
  text += octo::write_line("cam-a", sample(1000.0, 0.05, -0.10)) + "\n";
  text += "{\"t\":\"write\",\"id\":\"cam-a\",\"wall\":1060.0,\"err";  // cut off

  std::map<std::string, octo::CameraRecord> out;
  octo::replay_camera_db(text, 1000, &out);
  CHECK_EQ(out.size(), size_t(1));
  CHECK_EQ(out["cam-a"].samples.size(), size_t(1));
  CHECK_EQ(out["cam-a"].bias, -75);
}

// Camera names come off the air and are user-set, so one containing a quote
// must not be able to forge a record.
void test_a_hostile_camera_name() {
  octo::CameraRecord rec;
  rec.id = "cam-a";
  rec.name = "evil\",\"bias\":999,\"x\":\"";

  std::map<std::string, octo::CameraRecord> out;
  octo::replay_camera_db(octo::camera_line(rec) + "\n", 1000, &out);
  CHECK_STR(out["cam-a"].name, "evil\",\"bias\":999,\"x\":\"");
  CHECK(!out["cam-a"].has_bias);
}

void test_only_fair_measurements_teach_timing() {
  octo::CameraRecord rec;
  octo::WriteSample good = sample(1000.0, 0.05, -0.10);
  octo::WriteSample bad = sample(1060.0, 0.05, -1.40);
  bad.timing_ok = false;  // the whole-second bias was wrong, not the timing
  rec.samples.push_back(good);
  rec.samples.push_back(bad);
  rec.samples.push_back(good);

  const std::vector<double> delays = rec.recent_apply_delays(10);
  CHECK_EQ(delays.size(), size_t(2));
  for (double d : delays) CHECK_NEAR(d, 0.15, 1e-6);
}

void test_a_missing_file_is_not_an_error() {
  const std::string dir = scratch("fresh");
  const std::string path = dir + "/db.json";
  octo::CamDbOptions dbopt;
  std::string err;
  octo::CamDb db;
  CHECK(db.open(path, dbopt, &err));
  CHECK(db.enabled());
  CHECK(db.cameras().empty());
  CHECK(db.find("nobody") == nullptr);
  remove_tree(dir);
}

void test_an_empty_path_disables_it_quietly() {
  octo::CamDbOptions dbopt;
  std::string err;
  octo::CamDb db;
  CHECK(db.open("", dbopt, &err));   // not an error
  CHECK(!db.enabled());
  // ...and every call is a no-op that still reports success.
  CHECK(db.record_write("cam-a", sample(1.0, 0.05, -0.1), &err));
  CHECK(db.note_seen("cam-a", "x", 24, true, &err));
}

}  // namespace

int main() {
  test_apply_delay_is_lead_plus_what_is_left();
  test_a_lead_is_not_guessed_from_too_little();
  test_the_lead_is_a_median_not_a_mean();
  test_only_the_recent_window_counts();
  test_the_clamp_holds();
  test_adaptation_can_be_switched_off();
  test_a_power_cycle_does_not_forget_the_lead();
  test_what_is_written_comes_back();
  test_a_learned_zero_is_not_the_same_as_unknown();
  test_samples_are_bounded_per_camera();
  test_the_seen_window_only_widens();
  test_one_camera_cannot_evict_another();
  test_compaction_keeps_the_values_worth_keeping();
  test_compaction_does_not_thrash();
  test_a_truncated_line_costs_one_record_not_the_history();
  test_a_hostile_camera_name();
  test_only_fair_measurements_teach_timing();
  test_a_missing_file_is_not_an_error();
  test_an_empty_path_disables_it_quietly();
  return octotest::report("test_camdb");
}
