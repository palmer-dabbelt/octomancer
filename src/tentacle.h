// Decoder for the Tentacle Sync BLE advertising payload.
//
// Tentacles put their timecode in the *advertising* packet as service data
// under the 16-bit UUID FDAC, so reading them is entirely passive: no
// connection, no pairing, and no limit on how many boxes can be listened to at
// once. The layout below was reverse-engineered against a bench of five boxes;
// doc/tentacle-notes.md carries the evidence.
//
//   0x22, 9 bytes -- timecode, good to a few milliseconds
//     byte 0    type 0x22
//     byte 1    flags        low 6 bits 0x3d; bit 0x40 changes over minutes
//     byte 2    frame rate   low 6 bits = fps (0x18 = 24)
//     byte 3    hours        plain binary, NOT BCD
//     byte 4    minutes
//     byte 5    seconds
//     byte 6    frames
//     byte 7-8  microseconds since the frame, big-endian. Added to the frame
//               time, never substituted for part of it -- and offset by about
//               3.6 ms, so observed values run 3600..45300 rather than
//               0..41666 and 5% of them exceed a 24 fps frame outright. See
//               doc/tentacle-notes.md section 7. The consequence for anyone
//               writing one of these rather than reading it: (frame, micros)
//               is not a canonical spelling of an instant, and two encodings
//               of the same microsecond can both be correct.
//
//   0x32, 8 bytes -- microsecond clock, seen on a Track E
//     byte 0    type 0x32
//     byte 1-2  as above
//     byte 3-7  microseconds since midnight, 40-bit big-endian
//
//   0x42, 9 bytes -- a static payload carrying no clock.
//
// Note the contrast with Blackmagic, which sends timecode as packed BCD: here
// 0x15 means 21, not 15. Decoding one as the other yields a plausible wrong
// number rather than an obvious error, which is why it is worth stating twice.
#ifndef OCTO_TENTACLE_H
#define OCTO_TENTACLE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace octo {

// The service-data UUID the boxes broadcast under, in the long form
// CoreBluetooth reports.
extern const char kFdacUuid[];

enum PacketType : uint8_t {
  kTypeTimecode = 0x22,
  kTypeMicros = 0x32,
  kTypeStatic = 0x42,
};

// How precisely a box states its time. A Track E sends a microsecond counter;
// the others send frames plus a sub-frame microsecond field.
enum class Resolution { kNone, kFrame, kFrameMicros, kMicrosecond };

const char* resolution_name(Resolution r);

struct Decoded {
  // True only when `sod` is a time we are prepared to stand behind. Anything
  // we cannot read as a clock reports false and says why in `note`, because a
  // plausible wrong number is worse than an admission of ignorance.
  bool ok = false;

  bool has_type = false;
  uint8_t type = 0;

  double sod = 0.0;  // seconds since local midnight
  std::string display;
  std::string note;  // why this payload carried no usable time

  int fps = 0;
  int frames = -1;
  uint8_t flags = 0;
  bool has_micros = false;
  uint32_t micros = 0;
  Resolution resolution = Resolution::kNone;
};

Decoded decode(const uint8_t* data, size_t len);
Decoded decode(const std::vector<uint8_t>& data);

// The inverse, for making a box that is not there.
//
// Only a test fixture and the fake radio have any use for these -- nothing in
// octomancer transmits, and nothing should. They live beside the decoder
// rather than in the fake because that is the only way they can be held to
// it: encode-then-decode is checkable, and a second copy of the layout kept
// somewhere else would drift from this one silently, which is the failure
// mode where a synthetic bench proves the decoder against itself.
//
// The contract is exactly one property: what comes back out of decode() is
// the `sod` that went in, to the precision the format allows. Anything else a
// caller wants -- the ~3.6 ms floor real boxes show, a box running fast -- is
// expressed by adding it to `sod` before calling, not by a second parameter
// that would give the same payload two meanings.
//
// `sod` is seconds since local midnight; values outside a day are wrapped,
// because a fake box drifting past midnight is a case worth being able to
// arrange rather than an error. `sub_frame` false omits bytes 7-8 entirely,
// which is how to make a box that reports frame resolution rather than
// frame+us -- the decoder has a separate branch for it.
std::vector<uint8_t> encode_timecode(double sod, int fps,
                                     bool sub_frame = true,
                                     uint8_t flags = 0x3d);

// A 0x32 payload: the microsecond counter a Track E sends.
std::vector<uint8_t> encode_micros(double sod, uint8_t flags = 0x3d);

// A 0x42 payload, which carries no clock at all. Worth being able to make:
// it is the one a decoder must refuse, and a bench with one on it proves the
// refusal happens somewhere other than in a unit test.
std::vector<uint8_t> encode_static();

std::string to_hex(const uint8_t* data, size_t len);
std::string to_hex(const std::vector<uint8_t>& data);

}  // namespace octo

#endif  // OCTO_TENTACLE_H
