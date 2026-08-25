// octomancer-report -- turn a sync log into the numbers worth tuning against.
//
// The daemon logs every cycle, including the ones where it did nothing, so
// these questions can be answered from evidence rather than guessed at:
//
//   * How fast does the camera actually drift away from Tentacle time?
//   * Given a tolerance, how often does it therefore need correcting?
//   * Do the Tentacle boxes stay agreed with each other?
//   * Are writes landing, and did the learned RTC bias settle?
//
// The drift figure is the one that matters, and it is also the one easiest to
// fake: the camera reports whole frames, so every reading is quantised to
// 1/fps. At 24 fps that is 42 ms, which across a 30-second stretch fits a
// confident-looking +/-1400 ppm made entirely of rounding. So drift is
// measured only across free-running stretches between writes, only across
// stretches long enough to mean anything, and is refused rather than
// estimated when the answer would be below the quantisation floor.
#include <getopt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "logscan.h"
#include "timeutil.h"

namespace {

struct Options {
  std::string path = "octomancer-sync.jsonl";
  double min_segment = 1800.0;
  bool segments = false;
};

struct Point {
  double t = 0.0;
  double error = 0.0;
};

struct Fit {
  bool ok = false;
  double slope = 0.0;      // seconds of error per second elapsed
  double intercept = 0.0;
  double residual = 0.0;   // worst single deviation from the line
};

Fit linfit(const std::vector<Point>& pts) {
  Fit fit;
  if (pts.size() < 3) return fit;
  double mx = 0.0, my = 0.0;
  for (const Point& p : pts) {
    mx += p.t;
    my += p.error;
  }
  mx /= pts.size();
  my /= pts.size();
  double num = 0.0, den = 0.0;
  for (const Point& p : pts) {
    num += (p.t - mx) * (p.error - my);
    den += (p.t - mx) * (p.t - mx);
  }
  if (den == 0.0) return fit;
  fit.slope = num / den;
  fit.intercept = my - fit.slope * mx;
  for (const Point& p : pts) {
    fit.residual =
        std::max(fit.residual, std::fabs(p.error - (fit.slope * p.t + fit.intercept)));
  }
  fit.ok = true;
  return fit;
}

double median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

void usage(FILE* out) {
  std::fprintf(out,
      "usage: octomancer-report [options] [LOGFILE]\n"
      "\n"
      "Summarise a log written by octomancer-sync (default"
      " octomancer-sync.jsonl).\n"
      "\n"
      "  --segments        show each free-running segment between writes\n"
      "  --min-segment SEC shortest stretch that can measure drift (default\n"
      "                    1800; anything shorter is frame quantisation)\n"
      "  --version, --help\n");
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  enum { kSegments = 1000, kMinSegment, kVersion, kHelp };
  static const struct option longs[] = {
      {"segments", no_argument, nullptr, kSegments},
      {"min-segment", required_argument, nullptr, kMinSegment},
      {"version", no_argument, nullptr, kVersion},
      {"help", no_argument, nullptr, kHelp},
      {nullptr, 0, nullptr, 0},
  };
  for (;;) {
    const int c = getopt_long(argc, argv, "", longs, nullptr);
    if (c == -1) break;
    switch (c) {
      case kSegments: opt.segments = true; break;
      case kMinSegment: opt.min_segment = std::atof(optarg); break;
      case kVersion: std::printf("octomancer-report %s\n", OCTO_VERSION); return 0;
      case kHelp: usage(stdout); return 0;
      default: usage(stderr); return 2;
    }
  }
  if (optind < argc) opt.path = argv[optind];

  std::ifstream in(opt.path.c_str());
  if (!in) {
    std::printf("no log at %s -- run octomancer-sync first\n", opt.path.c_str());
    return 1;
  }

  std::vector<octo::LogRecord> cycles;
  int total = 0, malformed = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
    ++total;
    octo::LogRecord rec;
    if (!octo::parse_record(line, &rec)) {
      ++malformed;
      continue;
    }
    if (rec.text("event") == "cycle") cycles.push_back(rec);
  }

  if (malformed) {
    std::printf("(skipped %d unreadable line(s))\n", malformed);
  }
  if (cycles.empty()) {
    std::printf("%d record(s) but no cycles yet\n", total);
    return 1;
  }

  double t0 = 0.0, t1 = 0.0;
  octo::record_time(cycles.front(), &t0);
  octo::record_time(cycles.back(), &t1);
  std::printf("%zu cycles over %.1f minutes  (%s)\n", cycles.size(),
              (t1 - t0) / 60.0, opt.path.c_str());

  // --- what happened -------------------------------------------------
  std::map<std::string, int> actions;
  for (const octo::LogRecord& r : cycles) actions[r.text("action", "?")] += 1;
  std::vector<std::pair<int, std::string>> ranked;
  for (const auto& e : actions) ranked.push_back({e.second, e.first});
  std::sort(ranked.rbegin(), ranked.rend());
  std::printf("\n--- what happened ---\n");
  for (const auto& e : ranked) {
    std::printf("  %-26s %4d  (%.0f%%)\n", e.second.c_str(), e.first,
                100.0 * e.first / cycles.size());
  }

  // --- the Tentacle bench --------------------------------------------
  std::vector<double> spreads;
  std::map<std::string, std::vector<double>> per_box;
  int disagreed = 0;
  std::map<int, int> box_counts;
  for (const octo::LogRecord& r : cycles) {
    if (!r.has("tentacle_spread_s")) continue;
    spreads.push_back(r.number("tentacle_spread_s"));
    box_counts[static_cast<int>(r.number("tentacles"))] += 1;
    if (r.flag("bench_disagreement")) ++disagreed;
    std::map<std::string, std::string> boxes;
    if (r.has("boxes") && octo::parse_object(r.raw("boxes"), &boxes)) {
      for (const auto& entry : boxes) {
        octo::LogRecord box;
        std::map<std::string, std::string> members;
        if (!octo::parse_object(entry.second, &members)) continue;
        for (const auto& m : members) box.set(m.first, m.second);
        per_box[entry.first].push_back(box.number("offset_s"));
      }
    }
  }
  if (!spreads.empty()) {
    std::sort(spreads.begin(), spreads.end());
    std::printf("\n--- Tentacle bench ---\n  boxes heard:   ");
    bool first = true;
    for (const auto& e : box_counts) {
      std::printf("%s%d boxes x%d", first ? "" : ", ", e.first, e.second);
      first = false;
    }
    std::printf("\n  spread between boxes: median %.4fs  worst %.4fs\n",
                spreads[spreads.size() / 2], spreads.back());
    if (disagreed) {
      std::printf("  WARNING: %d cycle(s) where the boxes did not agree\n",
                  disagreed);
    }
    if (!per_box.empty()) {
      std::printf("\n  per box, offset from this Mac:\n");
      for (auto& entry : per_box) {
        std::vector<double>& offs = entry.second;
        std::sort(offs.begin(), offs.end());
        std::printf("    %-18s n=%-4zu median %+.4fs  range %+.4f..%+.4f\n",
                    entry.first.c_str(), offs.size(), offs[offs.size() / 2],
                    offs.front(), offs.back());
      }
    }
  }

  // --- camera error ---------------------------------------------------
  std::vector<double> errors;
  for (const octo::LogRecord& r : cycles) {
    double t;
    if (r.has("error_s") && octo::record_time(r, &t)) {
      errors.push_back(r.number("error_s"));
    }
  }
  if (errors.size() < 2) {
    std::printf("\nnot enough camera observations yet to say anything about"
                " drift\n");
    return 0;
  }
  std::sort(errors.begin(), errors.end());
  std::printf("\n--- camera error vs its reference ---\n");
  std::printf("  observations: %zu   median %+.3fs   range %+.3f..%+.3f\n",
              errors.size(), errors[errors.size() / 2], errors.front(),
              errors.back());

  // --- drift, from free-running stretches only -------------------------
  //
  // A write resets the clock, so fitting across one would measure the
  // correction rather than the drift.
  std::vector<std::vector<Point>> segments;
  std::vector<Point> current;
  for (const octo::LogRecord& r : cycles) {
    double t;
    if (!r.has("error_s") || !octo::record_time(r, &t)) continue;
    current.push_back({t, r.number("error_s")});
    if (r.text("action").compare(0, 5, "write") == 0) {
      if (current.size() >= 3) segments.push_back(current);
      current.clear();
    }
  }
  if (current.size() >= 3) segments.push_back(current);

  std::vector<std::vector<Point>> usable;
  double longest_dropped = 0.0;
  for (const std::vector<Point>& seg : segments) {
    const double span = seg.back().t - seg.front().t;
    if (span >= opt.min_segment) {
      usable.push_back(seg);
    } else {
      longest_dropped = std::max(longest_dropped, span);
    }
  }
  const size_t dropped = segments.size() - usable.size();

  if (usable.empty()) {
    std::printf("\n--- drift ---\n");
    if (dropped) {
      std::printf("  %zu free-running stretch(es), longest %.1f min -- all"
                  " below the\n  %.0f min needed to tell drift from frame"
                  " quantisation.\n",
                  dropped, longest_dropped / 60.0, opt.min_segment / 60.0);
    } else {
      std::printf("  No free-running stretch of 3+ cycles between writes yet.\n");
    }
    std::printf("\n  Leave it running for an hour or so. Drift on these clocks"
                " is parts per\n  million; measuring it needs a long lever arm,"
                " not more samples.\n");
    return 0;
  }

  std::printf("\n--- drift (free-running stretches only) ---\n");
  if (dropped) {
    std::printf("  (ignoring %zu stretch(es) shorter than %.0f min -- too short"
                " to\n   separate drift from frame quantisation)\n",
                dropped, opt.min_segment / 60.0);
  }
  std::vector<double> rates;
  double longest = 0.0;
  for (size_t i = 0; i < usable.size(); ++i) {
    const Fit fit = linfit(usable[i]);
    if (!fit.ok) continue;
    rates.push_back(fit.slope);
    const double span = usable[i].back().t - usable[i].front().t;
    longest = std::max(longest, span);
    if (opt.segments) {
      std::printf("  segment %zu: %zu cycles over %.1f min -> %+.3f s/hour"
                  "  (%+.1f ppm, resid %.3fs)\n",
                  i + 1, usable[i].size(), span / 60.0, fit.slope * 3600.0,
                  fit.slope * 1e6, fit.residual);
    }
  }
  if (rates.empty()) {
    std::printf("  nothing fittable yet\n");
    return 0;
  }

  const double med = median(rates);
  // The noise floor, from frame quantisation over the longest lever arm. A
  // measurement below its own floor is not a small number, it is no number.
  const double floor_ppm = longest > 0.0 ? (1.0 / 24.0) / longest * 1e6 : 0.0;
  std::printf("  %zu segment(s), median drift %+.4f s/hour  (%+.1f ppm)\n",
              rates.size(), med * 3600.0, med * 1e6);
  std::printf("  measurement floor is about %.1f ppm over a %.1f min stretch\n",
              floor_ppm, longest / 60.0);
  if (std::fabs(med * 1e6) < floor_ppm) {
    std::printf("  -> the measured drift is below that floor: treat it as 'no"
                " drift\n     detected yet' rather than as a value.\n");
  } else if (std::fabs(med) > 1e-12) {
    const double hours = 1.0 / std::fabs(med * 3600.0);
    std::printf("\n  At that rate the camera moves %.3fs per hour, so a 1.0s"
                " tolerance is\n  reached about every %.1f hours. Polling much"
                " faster than that only costs\n  connections -- though a short"
                " poll still catches a camera that was\n  power-cycled or"
                " re-jammed out from under us.\n",
                std::fabs(med * 3600.0), hours);
  }

  // --- writes ----------------------------------------------------------
  std::vector<const octo::LogRecord*> writes;
  for (const octo::LogRecord& r : cycles) {
    if (r.text("action").compare(0, 5, "write") == 0) writes.push_back(&r);
  }
  if (!writes.empty()) {
    std::printf("\n--- writes ---\n");
    int verified = 0;
    std::vector<double> latency, after;
    std::vector<long> biases;
    for (const octo::LogRecord* r : writes) {
      if (r->flag("verified")) ++verified;
      if (r->has("write_latency_s")) latency.push_back(r->number("write_latency_s"));
      if (r->has("error_after_s")) after.push_back(r->number("error_after_s"));
      if (r->has("rtc_bias")) {
        const long b = static_cast<long>(r->number("rtc_bias"));
        if (biases.empty() || biases.back() != b) biases.push_back(b);
      }
    }
    std::printf("  %zu write(s); %d verified\n", writes.size(), verified);
    if (!latency.empty()) {
      std::sort(latency.begin(), latency.end());
      std::printf("  BLE write latency: median %.0f ms  worst %.0f ms\n",
                  latency[latency.size() / 2] * 1000.0, latency.back() * 1000.0);
    }
    if (!after.empty()) {
      std::vector<double> sorted = after;
      std::sort(sorted.begin(), sorted.end());
      double worst = 0.0;
      for (double v : after) {
        if (std::fabs(v) > std::fabs(worst)) worst = v;
      }
      std::printf("  error just after a write: median %+.3fs  worst %+.3fs\n",
                  sorted[sorted.size() / 2], worst);
    }
    if (!biases.empty()) {
      std::printf("  learned RTC bias: ");
      for (size_t i = 0; i < biases.size(); ++i) {
        std::printf("%s%+ld", i ? " -> " : "", biases[i]);
      }
      std::printf("\n");
      if (biases.size() > 1 && biases[biases.size() - 1] == biases[biases.size() - 2]) {
        std::printf("    (settled)\n");
      }
    }
  }
  return 0;
}
