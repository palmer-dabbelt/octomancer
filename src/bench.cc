// See src/bench.h.
#include "bench.h"

#include <algorithm>
#include <cstdio>

#include "jsonlog.h"

namespace octo {
namespace {

// Whether this box is entitled to a vote. Three separate reasons not to be,
// and they are kept apart because only one of them is a person's decision.
bool heard_now(const DeviceSnapshot& d) { return d.live && d.has_time; }

}  // namespace

std::string boxes_to_json(const std::vector<DeviceSnapshot>& devices,
                          const CamConf& conf) {
  std::string out = "{";
  bool first = true;
  for (const DeviceSnapshot& d : devices) {
    if (!heard_now(d)) continue;
    if (!conf.box_enabled(d.id)) continue;
    if (!first) out += ',';
    first = false;
    out += '"' + json_escape(d.name.empty() ? d.id : d.name) + "\":";
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"offset_s\":%.4f,\"adverts\":%d,\"rssi\":%d,"
                  "\"resolution\":\"%s\"}",
                  d.median_offset, d.samples, d.rssi,
                  json_escape(d.resolution).c_str());
    out += buf;
  }
  out += '}';
  return out;
}

Bench bench_from(const Snapshot& snap, const CamConf& conf,
                 const std::string& source) {
  Bench bench;
  // The source is set whether or not anything voted. A caller reporting "no
  // boxes" wants to be able to say where it looked.
  bench.source = source;
  std::vector<double> votes;
  for (const DeviceSnapshot& d : snap.device) {
    if (!heard_now(d)) continue;
    if (!conf.box_enabled(d.id)) {
      ++bench.skipped;
      continue;
    }
    votes.push_back(d.median_offset);
  }
  bench.boxes_json = boxes_to_json(snap.device, conf);
  if (votes.empty()) return bench;

  bench.ok = true;
  bench.offset = median_offset(votes);
  const auto lo = std::min_element(votes.begin(), votes.end());
  const auto hi = std::max_element(votes.begin(), votes.end());
  bench.spread = *hi - *lo;
  bench.boxes = static_cast<int>(votes.size());
  return bench;
}

}  // namespace octo
