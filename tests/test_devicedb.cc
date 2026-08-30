// The roster that survives a restart.
//
// octomancerd used to forget every device the moment it exited, so a restart
// made `octomancer status` quietly disagree with the room: five boxes before,
// four after, with nothing saying the fifth had ever existed. Somebody looking
// for a box that is missing needs a row saying it has not been heard since
// Tuesday, not an absence they must already know about to notice.
//
// The properties here are mostly about *not* letting a remembered device
// pretend to be a present one. A restored row is identity plus a last-known
// reading; it is not evidence, and the moment it is allowed to behave like
// evidence it starts voting on what time it is.
#include <unistd.h>

#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "devicedb.h"
#include "harness.h"
#include "registry.h"
#include "tentacle.h"

using octo::DeviceDb;
using octo::Registry;
using octo::RememberedDevice;
using octo::Snapshot;

namespace {

std::string temp_path(const char* tag) {
  return "/tmp/octo-devdb-" + std::to_string(getpid()) + "-" + tag + ".json";
}

// A wall instant on a whole second. Which one does not matter; that the
// arithmetic below is exact does.
const double kWall = 1788122231.0;
const double kMono = 5000.0;

RememberedDevice made(const std::string& id, const std::string& name,
                      double last_seen) {
  RememberedDevice d;
  d.id = id;
  d.name = name;
  d.first_seen_wall = last_seen - 3600.0;
  d.last_seen_wall = last_seen;
  d.has_time = true;
  d.offset = -3.578164;
  d.median_offset = -3.578083;
  d.resolution = "frame+us";
  d.fps = 24;
  d.rssi = -49;
  return d;
}

// One advertisement from a box that is `offset` seconds off the true time.
void hear(Registry* reg, const std::string& id, const std::string& name,
          double offset, double mono, double wall) {
  const time_t whole = static_cast<time_t>(std::floor(wall));
  struct tm local;
  localtime_r(&whole, &local);
  const double sod = local.tm_hour * 3600.0 + local.tm_min * 60.0 +
                     local.tm_sec + (wall - std::floor(wall)) + offset;
  const std::vector<uint8_t> data = octo::encode_timecode(sod, 24);
  reg->observe(id, name, -50, data.data(), data.size(), mono, wall);
}

const octo::DeviceSnapshot* find(const Snapshot& snap, const std::string& id) {
  for (const octo::DeviceSnapshot& d : snap.device) {
    if (d.id == id) return &d;
  }
  return nullptr;
}

// ------------------------------------------------------------------ the file

void test_a_roster_survives_a_round_trip() {
  const std::string path = temp_path("round");
  ::unlink(path.c_str());

  std::vector<RememberedDevice> out;
  out.push_back(made("id-a", "BMPCC", kWall));
  out.push_back(made("id-b", "Krysta", kWall - 60.0));
  out.back().has_time = false;  // seen, never decoded

  DeviceDb writer;
  std::string err;
  CHECK(writer.save(path, out, &err));
  CHECK_STR(err, "");

  DeviceDb reader;
  CHECK(reader.load(path, &err));
  CHECK_STR(err, "");
  CHECK_EQ((int)reader.devices().size(), 2);
  if (reader.devices().size() != 2) return;

  const RememberedDevice& a = reader.devices()[0];
  CHECK_STR(a.id, "id-a");
  CHECK_STR(a.name, "BMPCC");
  CHECK_NEAR(a.first_seen_wall, kWall - 3600.0, 0.002);
  CHECK_NEAR(a.last_seen_wall, kWall, 0.002);
  CHECK(a.has_time);
  CHECK_NEAR(a.offset, -3.578164, 1e-6);
  CHECK_NEAR(a.median_offset, -3.578083, 1e-6);
  CHECK_STR(a.resolution, "frame+us");
  CHECK_EQ(a.fps, 24);
  CHECK_EQ(a.rssi, -49);

  // "seen but never decoded" has to survive as itself, or a box that only ever
  // sent a payload we cannot read comes back claiming a time of zero.
  CHECK(!reader.devices()[1].has_time);
  ::unlink(path.c_str());
}

void test_a_missing_file_is_an_empty_roster_not_an_error() {
  DeviceDb db;
  std::string err;
  CHECK(db.load(temp_path("absent"), &err));
  CHECK_STR(err, "");
  CHECK(db.devices().empty());
}

// A file that exists and will not parse must stop the daemon rather than be
// skipped. Carrying on would serve a roster short of whatever the file held
// and then overwrite the file with the short version on the next save --
// turning a parse error into data loss, silently, in the direction nobody
// checks.
void test_a_damaged_file_is_refused() {
  const std::string path = temp_path("damaged");
  {
    std::ofstream out(path, std::ios::trunc);
    out << "{\"t\":\"device\",\"id\":\"good\"}\n";
    out << "this is not json\n";
  }
  DeviceDb db;
  std::string err;
  CHECK(!db.load(path, &err));
  CHECK(!err.empty());
  // ...and it hands back nothing rather than the half it managed, so a caller
  // that ignores the return value cannot save the truncated version.
  CHECK(db.devices().empty());
  ::unlink(path.c_str());
}

void test_a_record_with_no_id_is_refused() {
  const std::string path = temp_path("noid");
  {
    std::ofstream out(path, std::ios::trunc);
    out << "{\"t\":\"device\",\"name\":\"nameless\"}\n";
  }
  DeviceDb db;
  std::string err;
  CHECK(!db.load(path, &err));
  CHECK(!err.empty());
  ::unlink(path.c_str());
}

// --------------------------------------------------------------- the registry

void test_a_remembered_device_is_listed_but_not_live() {
  Registry reg(octo::Policy(), kMono);
  reg.remember(made("id-a", "BMPCC", kWall - 7200.0), kWall);

  const Snapshot snap = reg.snapshot(kMono, kWall);
  const octo::DeviceSnapshot* d = find(snap, "id-a");
  CHECK(d != nullptr);
  if (d == nullptr) return;
  CHECK_STR(d->name, "BMPCC");
  CHECK(!d->live);
  CHECK(!d->heard_this_run);
  // Aged against the wall, because there is no monotonic stamp from this
  // process to subtract. Two hours, not "since the daemon started".
  CHECK_NEAR(d->age, 7200.0, 1.0);
  CHECK_EQ(snap.devices, 1);
  CHECK_EQ(snap.live, 0);
}

// The property that keeps a remembered row honest. A box switched off last
// week must not vote on what time it is now.
void test_a_remembered_device_does_not_vote_on_the_bench() {
  Registry reg(octo::Policy(), kMono);
  RememberedDevice old = made("id-old", "Gone", kWall - 86400.0);
  old.offset = 30.0;          // wildly wrong, and a week stale
  old.median_offset = 30.0;
  reg.remember(old, kWall);

  hear(&reg, "id-live", "Here", -3.5, kMono + 1.0, kWall + 1.0);
  hear(&reg, "id-live", "Here", -3.5, kMono + 2.0, kWall + 2.0);

  const Snapshot snap = reg.snapshot(kMono + 2.0, kWall + 2.0);
  CHECK_EQ(snap.devices, 2);
  CHECK_EQ(snap.live, 1);
  CHECK(snap.has_bench);
  // The bench is the live box alone. If the remembered one were counted the
  // median would be dragged to about +13 s and the spread to 33 s.
  CHECK_NEAR(snap.bench_offset, -3.5, 0.05);
  CHECK_NEAR(snap.bench_spread, 0.0, 0.05);
}

// Hearing a device beats remembering one, whichever order they arrive in. The
// daemon loads then observes; a test will do the opposite, and both have to
// mean the same thing.
void test_hearing_beats_remembering_in_either_order() {
  {
    Registry reg(octo::Policy(), kMono);
    reg.remember(made("id-a", "Old Name", kWall - 7200.0), kWall);
    hear(&reg, "id-a", "New Name", -3.5, kMono + 1.0, kWall + 1.0);
    // Bound to a local, not passed as a temporary: snapshot() returns by
    // value and find() hands back a pointer into it, so the obvious one-liner
    // reads freed memory and passes or fails at random.
    const Snapshot snap = reg.snapshot(kMono + 1.0, kWall + 1.0);
    const octo::DeviceSnapshot* d = find(snap, "id-a");
    CHECK(d != nullptr);
    if (d != nullptr) {
      CHECK(d->live);
      CHECK(d->heard_this_run);
      CHECK_STR(d->name, "New Name");
      CHECK_NEAR(d->age, 0.0, 0.01);
    }
  }
  {
    Registry reg(octo::Policy(), kMono);
    hear(&reg, "id-a", "New Name", -3.5, kMono + 1.0, kWall + 1.0);
    reg.remember(made("id-a", "Old Name", kWall - 7200.0), kWall + 1.0);
    const Snapshot snap = reg.snapshot(kMono + 1.0, kWall + 1.0);
    const octo::DeviceSnapshot* d = find(snap, "id-a");
    CHECK(d != nullptr);
    if (d != nullptr) {
      CHECK(d->live);
      CHECK_STR(d->name, "New Name");
    }
  }
}

// The erosion test. A device offline across several restarts must come back
// unchanged every time -- otherwise the file loses a field per restart and the
// roster decays into a list of bare ids.
void test_a_remembered_device_round_trips_unchanged() {
  RememberedDevice seed = made("id-a", "BMPCC", kWall - 7200.0);

  RememberedDevice carried = seed;
  for (int restart = 0; restart < 5; ++restart) {
    Registry reg(octo::Policy(), kMono);
    reg.remember(carried, kWall);
    const std::vector<RememberedDevice> out = reg.remembered(kWall);
    CHECK_EQ((int)out.size(), 1);
    if (out.empty()) return;
    carried = out.front();
  }

  CHECK_STR(carried.id, seed.id);
  CHECK_STR(carried.name, seed.name);
  CHECK_NEAR(carried.first_seen_wall, seed.first_seen_wall, 1e-6);
  // Emphatically not "now": a save must not claim the whole roster was on the
  // air at the moment of shutdown.
  CHECK_NEAR(carried.last_seen_wall, seed.last_seen_wall, 1e-6);
  CHECK(carried.has_time);
  CHECK_NEAR(carried.offset, seed.offset, 1e-6);
  CHECK_NEAR(carried.median_offset, seed.median_offset, 1e-6);
  CHECK_STR(carried.resolution, seed.resolution);
  CHECK_EQ(carried.fps, seed.fps);
}

// A device that *is* heard exports what it has just measured, not what it was
// loaded with.
void test_a_heard_device_exports_what_it_measured() {
  Registry reg(octo::Policy(), kMono);
  for (int i = 1; i <= 5; ++i) {
    hear(&reg, "id-a", "Here", -3.5, kMono + i, kWall + i);
  }
  const std::vector<RememberedDevice> out = reg.remembered(kWall + 5.0);
  CHECK_EQ((int)out.size(), 1);
  if (out.empty()) return;
  CHECK(out.front().has_time);
  CHECK_NEAR(out.front().offset, -3.5, 0.05);
  CHECK_NEAR(out.front().median_offset, -3.5, 0.05);
  CHECK_NEAR(out.front().last_seen_wall, kWall + 5.0, 0.01);
  CHECK_STR(out.front().resolution, "frame+us");
}

void test_forgetting_a_remembered_device_removes_it() {
  Registry reg(octo::Policy(), kMono);
  reg.remember(made("id-a", "BMPCC", kWall - 60.0), kWall);
  CHECK(reg.forget("id-a"));
  CHECK(reg.snapshot(kMono, kWall).device.empty());
  CHECK(reg.remembered(kWall).empty());
}

// The two halves of the program want opposite things, and this is the switch.
// A sync daemon holds a working set because it may be a box with nothing but
// NVS; octomancerd remembers everything because remembering is what it is for.
void test_forget_after_zero_means_never() {
  octo::Policy keep;
  keep.forget_after = 0.0;
  Registry reg(keep, kMono);
  hear(&reg, "id-old", "Old", -3.5, kMono + 1.0, kWall + 1.0);

  // A day and a half later, another box turns up. Under the default policy the
  // sweep on that advertisement would drop the first one.
  const double later = kMono + 129600.0;
  hear(&reg, "id-new", "New", -3.5, later, kWall + 129600.0);
  CHECK_EQ(reg.snapshot(later, kWall + 129600.0).devices, 2);

  octo::Policy sweep;  // the default, 86400
  Registry box(sweep, kMono);
  hear(&box, "id-old", "Old", -3.5, kMono + 1.0, kWall + 1.0);
  hear(&box, "id-new", "New", -3.5, later, kWall + 129600.0);
  CHECK_EQ(box.snapshot(later, kWall + 129600.0).devices, 1);
}

// A roster copied from another machine, or a wall clock stepped backwards, can
// put a last sighting in the future. A negative age renders as a device heard
// in several hours' time, which is worse than useless in a program about
// clocks.
void test_a_sighting_in_the_future_reads_as_zero() {
  Registry reg(octo::Policy(), kMono);
  reg.remember(made("id-a", "BMPCC", kWall + 3600.0), kWall);
  const Snapshot snap = reg.snapshot(kMono, kWall);
  const octo::DeviceSnapshot* d = find(snap, "id-a");
  CHECK(d != nullptr);
  if (d != nullptr) CHECK_NEAR(d->age, 0.0, 1e-9);
}

}  // namespace

int main() {
  test_a_roster_survives_a_round_trip();
  test_a_missing_file_is_an_empty_roster_not_an_error();
  test_a_damaged_file_is_refused();
  test_a_record_with_no_id_is_refused();
  test_a_remembered_device_is_listed_but_not_live();
  test_a_remembered_device_does_not_vote_on_the_bench();
  test_hearing_beats_remembering_in_either_order();
  test_a_remembered_device_round_trips_unchanged();
  test_a_heard_device_exports_what_it_measured();
  test_forgetting_a_remembered_device_removes_it();
  test_forget_after_zero_means_never();
  test_a_sighting_in_the_future_reads_as_zero();
  return octotest::report("test_devicedb");
}
