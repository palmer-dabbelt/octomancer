// A bench of timecode boxes and a camera that are not there.
//
// This exists because almost nothing in octomancer can be exercised end to end
// without hardware. The decision-making is testable -- that is the seam
// CLAUDE.md describes and it has held -- but everything above it has only ever
// been run against five real boxes in one room. A daemon that mis-handles a
// box going quiet, a camera that reports an implausible rate, a bench that
// disagrees with itself: all reachable in an afternoon with real hardware and
// otherwise not reachable at all.
//
// The split here is the same one as everywhere else. This header is arithmetic
// -- given an instant, what would each box be transmitting? -- and has no
// timer, no thread and no radio in it, so it is tested directly. The glue that
// calls it on a timer and hands the results to a Scanner callback is in
// src/scanner_fake.cc and is as thin as it can be made.
//
// What this is emphatically not: a simulation of Bluetooth. There is no
// advertising interval jitter, no packet loss and no connection state machine
// beyond what the callers need. It answers "what does the program above do
// when it is told these things", which is the question that has been
// unanswerable, and not "does the radio work".
#ifndef OCTO_FAKEBENCH_H
#define OCTO_FAKEBENCH_H

#include <cstdint>
#include <string>
#include <vector>

#include "scanner.h"

namespace octo {

// One synthetic timecode box.
struct FakeBox {
  std::string id;
  std::string name;

  // How far this box's clock is from true time, in seconds. The whole point of
  // the program is measuring this, so it is the first thing a fake one must be
  // able to be wrong about.
  double offset_s = 0.0;

  // And how fast that offset is changing. Real boxes drift by tens of parts
  // per million -- the bench in doc/tentacle-notes.md runs about -23 ppm -- and
  // a drift estimator cannot be exercised by boxes that hold still.
  double drift_ppm = 0.0;

  int fps = 24;
  int rssi = -60;

  // Which of the three payload types this box speaks. A Track E sends the
  // microsecond counter; the others send frames, with or without the sub-frame
  // field. A bench of one kind would leave two decoder branches unvisited.
  enum class Kind { kFrameMicros, kFrame, kMicros, kStatic };
  Kind kind = Kind::kFrameMicros;

  // How often it transmits, and when it stops. `silent_after_s` is measured
  // from the start of the run and is how "a box went off the air" is arranged;
  // negative means never.
  double interval_s = 0.5;
  double silent_after_s = -1.0;
  // ...and when it comes back, for the case that actually matters: a box that
  // returns has to be re-heard rather than remembered wrongly. Negative means
  // never.
  double returns_after_s = -1.0;
};

// The camera, which is a different kind of thing: it is connected to and
// written to rather than merely overheard, so most of it lives behind
// CameraLink. What belongs here is only what a scanner can see.
struct FakeCamera {
  std::string id;
  std::string name;
  int rssi = -55;
  double interval_s = 1.0;
  double silent_after_s = -1.0;
  double returns_after_s = -1.0;

  // The camera's own clock error at the start of the run, in seconds, and its
  // drift. Not visible to a scanner -- a camera puts no clock in its
  // advertisement -- but carried here because this struct is also what the
  // fake CameraLink is built from, and splitting one device across two
  // descriptions is how they get out of step.
  double error_s = 0.0;
  double drift_ppm = 0.0;
  int fps = 24;
  bool recording = false;
  // 4.7. A camera whose timecode does not follow its clock cannot be synced,
  // and saying so is a case worth being able to arrange.
  bool timecode_follows_clock = true;
};

struct FakeBench {
  std::vector<FakeBox> boxes;
  FakeCamera camera;
  bool has_camera = false;

  // Parse a bench from a one-line description. Returns false with a reason
  // rather than a partial bench: a typo in a spec should not quietly produce a
  // different experiment from the one somebody meant to run.
  //
  //   box,<name>,<offset_s>[,<fps>][,<kind>][,<drift_ppm>]
  //   cam,<id>,<name>,<error_s>[,<fps>]
  //
  // separated by ';'. `kind` is one of frame+us, frame, us, static. A leading
  // '@' means the rest is a path to read the spec from, so a long bench does
  // not have to live in an environment variable.
  static bool parse(const std::string& spec, FakeBench* out, std::string* err);

  // The bench that `--radio fake` gives with nothing else said: five boxes
  // that disagree slightly, one of them a microsecond box, one that goes quiet
  // and comes back, and a camera that is a quarter-second out. Modelled on the
  // real bench in doc/tentacle-notes.md, because a default that resembles
  // nothing would make the fake radio useless for the thing it is for.
  static FakeBench standard();
};

// The arithmetic, and the whole reason this file is separate: what would be on
// the air at `mono` seconds into the run, given that the run started at wall
// clock `wall0`?
//
// `since` and `mono` are *elapsed* seconds since the run began, because that
// is what a bench spec talks about: "goes quiet after sixty seconds" means
// sixty seconds after somebody started looking.
//
// `since` is the last instant this was asked, so a caller polling on a timer
// gets each advert exactly once however irregularly it polls -- a scanner that
// dropped adverts when its loop was late would make the program above it look
// unreliable for reasons that are not its own. Pass a negative `since` for the
// first call.
//
// `mono0` and `wall0` are where those two clocks stood when the run began, and
// exist only to stamp the output: an Advert's `mono` is compared against
// mono_now() by everything downstream, and mono_now() counts from boot rather
// than from the start of this program. Emitting elapsed time there makes every
// device look as old as the machine's uptime, which reads as a bench that has
// not been heard from in days.
std::vector<Advert> adverts_between(const FakeBench& bench, double since,
                                    double mono, double mono0, double wall0);

// The same question for the camera, whose sightings are a different callback.
std::vector<Sighting> sightings_between(const FakeBench& bench, double since,
                                        double mono, double mono0,
                                        double wall0);

// Where a box's clock actually is at `mono` seconds into the run: true time
// plus its offset, plus whatever its drift has accumulated. Exposed because
// the tests want to state the expected answer without restating the model.
double box_clock(const FakeBox& box, double mono, double wall0);

}  // namespace octo

#endif  // OCTO_FAKEBENCH_H
