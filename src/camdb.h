// What we have learned about each camera body, kept across restarts.
//
// Two of the numbers this program discovers are properties of the *body*, not
// of its current clock reading, and both are expensive to acquire:
//
//   * the RTC bias, a whole number of seconds, which costs one rationed write
//     per adjustment and took hours to converge the first time;
//   * the apply delay, the sub-second lag between handing the camera an RTC
//     packet and the camera acting on it, which needs several writes to see
//     through a frame of quantisation.
//
// Both already survive a power cycle -- forget_drift() deliberately keeps them
// -- but neither survived the daemon exiting, so every restart threw away work
// that is measured an hour at a time. This is where they live now.
//
// # Format
//
// JSON Lines, replayed in order, with three record types:
//
//   {"t":"camera",...}   the learned state of one body
//   {"t":"write",...}    one observed write and what it implied
//   {"t":"compact",...}  a marker left where the file was rewritten
//
// Flat objects only, so src/logscan.h reads it without a general JSON parser
// and without growing one. That constraint is why a checkpoint is a *run of
// lines* rather than one line holding an array: a compaction writes a marker,
// then one `camera` line per body, then the retained `write` lines. Replay
// does not need to know a compaction happened -- later records simply win.
//
// # Why a log and not a document
//
// Rewriting the whole database on every observation would be the obvious
// thing, and it would put a full serialise-and-fsync on the path that runs
// while a camera is connected. Appending one line costs one write; the
// rewrite happens on a schedule that keeps the file within a constant factor
// of the data actually worth keeping. See should_compact().
#ifndef OCTO_CAMDB_H
#define OCTO_CAMDB_H

#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace octo {

// Bumped whenever a change in how a reading becomes an error makes new samples
// incomparable with old ones.
inline constexpr int kMeasureEpoch = 1;

// One write, and what it taught us. Filled in after the verification read, so
// error_after is a measurement rather than an expectation.
struct WriteSample {
  double wall = 0.0;
  double error_before_s = 0.0;
  double error_after_s = 0.0;
  double lead_used_s = 0.0;   // what aligned_wait was told to aim off by
  double latency_s = 0.0;     // the GATT write itself, measured
  int fps = 0;
  int bias = 0;
  bool verified = false;   // the write took: the clock moved where we asked
  // Whether the residual is a fair measurement of the apply delay. A write
  // that verified but missed by more than half a second missed because the
  // whole-second bias was wrong, and that says nothing about timing.
  bool timing_ok = false;

  // Which measurement basis produced error_after_s. A sample taken before the
  // reader was corrected for arrival staleness and frame centring is not
  // comparable with one taken after: the two differ by tens of milliseconds on
  // the same hardware, which is the size of the very quantity being learned.
  // Old records replay as epoch 0 and are kept as history, but they do not
  // teach the current lead.
  int measure_epoch = 0;

  // How late the camera actually acted, in seconds.
  //
  // The write is sent `lead` before the second boundary being aimed at, so a
  // camera that applied the value instantly would land dead on and leave
  // error_after at zero. It does not: error_after = lead - apply_delay, so the
  // delay is the lead plus whatever error is left over.
  double apply_delay_s() const { return lead_used_s - error_after_s; }
};

// Everything known about one body.
struct CameraRecord {
  std::string id;
  std::string name;
  double first_seen_wall = 0.0;
  double last_seen_wall = 0.0;
  uint64_t sessions = 0;   // power cycles observed
  uint64_t writes = 0;     // writes ever recorded, not just those retained
  int fps = 0;

  bool has_bias = false;
  int bias = 0;
  bool has_lead = false;
  double lead_s = 0.0;
  bool has_drift = false;
  double drift_ppm = 0.0;
  double drift_span_s = 0.0;

  // Bounded, oldest dropped. The cap is per body, so one busy camera cannot
  // push another body's history out of the file.
  std::deque<WriteSample> samples;

  // The most recent `n` apply delays, oldest first. Only from writes whose
  // residual was a fair measurement: one that did not take says nothing about
  // timing, because we do not know the camera acted on it at all.
  std::vector<double> recent_apply_delays(size_t n) const;
};

struct CamDbOptions {
  // Per body. At one write an hour the cap is about six weeks of history.
  size_t max_samples = 1000;
  // Never rewrite a file smaller than this, however much of it is dead: below
  // it the rewrite costs more than the space it reclaims.
  double compact_min_bytes = 64.0 * 1024.0;
  // Rewrite once the file passes this multiple of what a compaction would
  // leave behind. Bounds the wasted space at (factor - 1) and keeps the
  // amortised cost of an append constant, which a fixed byte threshold does
  // not: with a fixed one, a database whose live set already sits near the
  // threshold compacts again on almost every append.
  double compact_factor = 2.0;
};

// The default location, ~/.octomancer/per_camera.json. Empty when $HOME is
// unset, which disables the database rather than writing somewhere surprising.
std::string default_camera_db_path();

class CamDb {
 public:
  CamDb() = default;
  ~CamDb();

  CamDb(const CamDb&) = delete;
  CamDb& operator=(const CamDb&) = delete;

  // Replay `path` and hold it open for appending. An empty path disables the
  // database. A file that does not exist yet is not an error; nor is a corrupt
  // line, which is skipped -- a machine that lost power mid-append should cost
  // one observation, not the whole history.
  bool open(const std::string& path, const CamDbOptions& opt, std::string* err);
  bool enabled() const { return file_ != nullptr; }
  const std::string& path() const { return path_; }

  // Null when this body has never been seen.
  const CameraRecord* find(const std::string& id) const;
  const std::map<std::string, CameraRecord>& cameras() const { return cam_; }

  // Note that a body is on the air, counting a session if `new_session`.
  // Cheap by design: called once per power cycle, not once per poll.
  bool note_seen(const std::string& id, const std::string& name, int fps,
                 bool new_session, std::string* err);

  // Record one write. Updates the in-memory record and appends a line.
  bool record_write(const std::string& id, const WriteSample& s,
                    std::string* err);

  // Persist learned parameters for a body already known to the database.
  // Appends a `camera` line, so the value survives even if no compaction ever
  // happens.
  bool record_params(const std::string& id, std::string* err);

  // Update the learned parameters in memory. Returns true if anything actually
  // changed, so the caller can avoid appending a line that says nothing.
  bool learn(const std::string& id, bool has_bias, int bias, bool has_lead,
             double lead_s, bool has_drift, double drift_ppm,
             double drift_span_s);

  // Rewrite the file with only what is worth keeping. Called automatically;
  // exposed for tests and for a caller that wants to tidy up before exit.
  bool compact(std::string* err);

  uint64_t compactions() const { return compactions_; }
  double bytes() const { return static_cast<double>(bytes_); }

  void close();

 private:
  bool append(const std::string& line, std::string* err);
  bool should_compact() const;
  // Bytes a compaction would leave behind, from the records held in memory.
  double live_bytes_estimate() const;

  std::FILE* file_ = nullptr;
  std::string path_;
  CamDbOptions opt_;
  std::map<std::string, CameraRecord> cam_;
  long long bytes_ = 0;
  uint64_t compactions_ = 0;
};

// Replay a whole database from text. Exposed so a test can drive the parser
// without a filesystem, which is what makes the corrupt-line case easy to
// write and therefore likely to be written.
void replay_camera_db(const std::string& text, size_t max_samples,
                      std::map<std::string, CameraRecord>* out);

// One line of the file, for each record type.
std::string camera_line(const CameraRecord& rec);
std::string write_line(const std::string& id, const WriteSample& s);

}  // namespace octo

#endif  // OCTO_CAMDB_H
