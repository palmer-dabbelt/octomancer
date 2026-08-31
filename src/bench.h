// Which timecode boxes are the bench, and what time the bench says it is.
//
// This is the arithmetic that decides what gets written to a camera, so it is
// kept away from the radio and away from the daemon loop: it takes a snapshot
// and a configuration, both of which a test can build, and returns a number.
//
// It lived in src/octomancer-sync.cc until a bug went unnoticed in it for
// want of anywhere to write the test down. See doc/dongle-notes.md.
//
// **Why the median is recomputed here** rather than taken from
// Snapshot::bench_offset, which octomancerd already worked out. octomancerd
// computed that across every live box it could hear, and octomancerd has
// never read cameras.conf -- it is passive, listening costs nothing, so it
// listens to all of them and has no idea which ones a person has dismissed.
// Doing the arithmetic here against the enabled set is what makes the Devices
// page and the clock written to the camera agree about what the bench is. If
// they disagree, the page is lying: the figure it shows is not the figure
// anything acted on. src/devices.cc computes the same number the same way,
// for the same reason.
//
// **Why a box that is off the air does not vote.** A box out at the far end
// of the room is still part of the Tentacle mesh and is still in step with
// the others; it is only its *broadcasts* that are missing. It is tempting to
// keep counting it on those grounds. Don't: what would be counted is a
// remembered offset, and a remembered offset is not evidence about the
// present. The bench is what is being heard now, and a box dropping out of it
// is a fall in confidence rather than a fault.
#ifndef OCTO_BENCH_H
#define OCTO_BENCH_H

#include <string>
#include <vector>

#include "camconf.h"
#include "registry.h"

namespace octo {

struct Bench {
  // Whether anything voted. False and boxes == 0 is the honest answer when
  // the room has gone quiet, and it is a different statement from an offset
  // of zero -- which is what a bench of one box sitting on this Mac's clock
  // would report.
  bool ok = false;
  double offset = 0.0;
  double spread = 0.0;
  int boxes = 0;
  // Boxes we heard and then ignored, because somebody switched them off. Kept
  // as a number rather than dropped silently: "no boxes to sync to" and "the
  // only box in the room is disabled" are different situations and a person
  // reading the log should not have to guess which one they are in.
  int skipped = 0;
  std::string source;
  std::string boxes_json;
};

// Fold a snapshot's boxes into a bench, leaving out the ones somebody has
// switched off and the ones nobody is currently hearing.
//
// `source` is carried through untouched: it says how the snapshot was come by
// -- "octomancerd", or "scan" when this program had to listen for itself --
// and exists so a log line can say which.
Bench bench_from(const Snapshot& snap, const CamConf& conf,
                 const std::string& source);

// The boxes that voted, as a JSON object keyed by name, for the cycle record.
// Only the voters: the log line has to be the arithmetic that actually
// happened, so a box that was skipped is absent here and counted in
// Bench::skipped instead.
std::string boxes_to_json(const std::vector<DeviceSnapshot>& devices,
                          const CamConf& conf);

}  // namespace octo

#endif  // OCTO_BENCH_H
