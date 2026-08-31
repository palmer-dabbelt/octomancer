// See src/dongle.h.
#include "dongle.h"

namespace octo {

const char* link_way_name(LinkWay way) {
  switch (way) {
    case LinkWay::kUsb: return "usb";
    case LinkWay::kBluetooth: return "bluetooth";
    case LinkWay::kNone: break;
  }
  return "none";
}

bool parse_ble_use(const std::string& text, BleUse* out) {
  if (text == "off" || text == "no" || text == "never") {
    if (out != nullptr) *out = BleUse::kOff;
    return true;
  }
  if (text == "auto") {
    if (out != nullptr) *out = BleUse::kAuto;
    return true;
  }
  if (text == "both" || text == "debug") {
    if (out != nullptr) *out = BleUse::kBoth;
    return true;
  }
  return false;
}

const char* ble_use_name(BleUse use) {
  switch (use) {
    case BleUse::kOff: return "off";
    case BleUse::kAuto: return "auto";
    case BleUse::kBoth: return "both";
  }
  return "auto";
}

bool want_bluetooth(bool usb_ready, bool debug_both) {
  return debug_both || !usb_ready;
}

LinkWay carrier(bool usb_ready, bool ble_ready) {
  if (usb_ready) return LinkWay::kUsb;
  if (ble_ready) return LinkWay::kBluetooth;
  return LinkWay::kNone;
}

DongleView::DongleView(std::string radio) : radio_(std::move(radio)) {}

DeviceSnapshot device_from_message(const Message& msg,
                                   const std::string& radio) {
  DeviceSnapshot d;
  d.radio = radio;
  d.id = msg.get("id");
  d.name = msg.get("name");
  // "(unnamed)" is not a name. src/registry.cc substitutes it for an empty
  // one when it builds a snapshot, so a box that has never told us what it is
  // called arrives over the wire wearing that string -- and four such boxes
  // arrive wearing the same one, which renders as four identical rows.
  //
  // Translated back here, at the boundary, which is where a protocol's
  // stand-ins belong. The better fix is at the sending end, and it needs the
  // box reflashed; this one works against the firmware already on it.
  //
  // A dongle scans passively (src/tentacle.h explains why: the timecode is in
  // the advertisement, so nothing has to be asked for), and a Tentacle puts
  // its name in the scan response rather than the advertisement. So the
  // dongle genuinely does not know what these boxes are called, and the row
  // falls back to the hardware address.
  if (d.name == "(unnamed)") d.name.clear();
  int64_t n = 0;
  if (msg.get_int("rssi", &n)) d.rssi = static_cast<int>(n);
  bool live = false;
  if (msg.get_bool("live", &live)) d.live = live;
  msg.get_double("age", &d.age);
  // Everything in a row from a dongle was measured by the dongle during this
  // run of the dongle. It has no roster on disk to restore from, so there is
  // no such thing here as a remembered device -- which is what
  // heard_this_run distinguishes.
  d.heard_this_run = true;

  // A box with no time is a real answer and a different one from an offset of
  // zero: the dongle heard it advertise and had nothing readable in it. The
  // median is the field that decides, because the median is the field
  // everything downstream actually uses.
  double median = 0.0;
  if (msg.get_double("median", &median)) {
    d.has_time = true;
    d.median_offset = median;
    // The most recent reading when it was sent, falling back to the median.
    // Kept apart from it deliberately: one is what the box just said, the
    // other is what it has been saying.
    d.offset = median;
    msg.get_double("offset", &d.offset);
    if (msg.get_int("samples", &n)) d.samples = static_cast<int>(n);
  }
  double ppm = 0.0;
  if (msg.get_double("ppm", &ppm)) {
    d.has_drift = true;
    d.drift_ppm = ppm;
  }
  return d;
}

void DongleView::opened(double now_mono) {
  attached_ = true;
  greeted_ = false;
  // Not a fresh poll interval: a dongle that has just greeted us should be
  // asked immediately rather than after the first interval has elapsed.
  ever_asked_ = false;
  asked_mono_ = now_mono;
  in_batch_ = false;
  pending_.clear();
}

void DongleView::closed(double now_mono) {
  (void)now_mono;
  attached_ = false;
  greeted_ = false;
  in_batch_ = false;
  pending_.clear();
  current_.clear();
  current_mono_ = 0.0;
  clock_real_ = false;
}

void DongleView::observe(const Message& msg, double now_mono) {
  if (msg.verb == "hello") {
    greeted_ = true;
    // A box may name itself. If it does not, the host's label stands: the
    // rows have to be tagged with something a person can tell apart from
    // "the radio in this machine", and a port path is not that.
    const std::string named = msg.get("name");
    if (!named.empty()) radio_ = named;
    return;
  }

  if (msg.verb == "say") {
    said_ = msg.get("text");
    return;
  }

  if (msg.verb == "status") {
    // Absent means an older box that does not carry the field. Treating that
    // as "free" is the safe way round: it only ever withholds a claim.
    clock_real_ = msg.get("clock") == "real";
    return;
  }

  if (msg.verb == "dev") {
    if (!in_batch_) {
      in_batch_ = true;
      pending_.clear();
    }
    DeviceSnapshot d = device_from_message(msg, radio_);
    // Whether the box is *alarmed* about this device is only worth importing
    // from a box that knows what time it is.
    //
    // An alert means "this device is more than a minute from the truth", and a
    // dongle nobody has told the time measures everything against a clock that
    // started at zero when it was plugged in -- so every box it hears is four
    // hours out by its own reckoning, and every row arrives alarmed. Four red
    // rows describing a bench that is in perfect agreement is worse than no
    // colour at all: it is the alarm that teaches somebody to ignore alarms.
    if (clock_real_) {
      bool alerting = false;
      if (msg.get_bool("alerting", &alerting)) d.alerting = alerting;
    }
    pending_.push_back(std::move(d));
    return;
  }

  if (msg.verb == "end" && msg.get("what") == "devices") {
    // A batch is a complete statement about the room, so it replaces the last
    // one wholesale. A box the dongle has stopped hearing is absent from the
    // new list and therefore gone from ours, which is the point: keeping it
    // would leave a row nothing is measuring.
    //
    // An `end` with no `dev` before it is a real and ordinary answer -- an
    // empty room -- and has to be taken as one, or a dongle that stops
    // hearing everything would go on showing the last thing it heard forever.
    current_.swap(pending_);
    pending_.clear();
    in_batch_ = false;
    current_mono_ = now_mono;
    ++answers_;
    return;
  }

  // Anything else -- bench, ok, err, a verb from a version we have not met --
  // is not this object's business.
}

bool DongleView::wants_poll(double now_mono, Message* out) {
  if (!attached_ || !greeted_) return false;
  if (ever_asked_ && now_mono - asked_mono_ < kPollEvery) return false;
  if (out != nullptr) {
    out->verb = "devices";
    out->fields.clear();
  }
  return true;
}

void DongleView::polled(double now_mono) {
  ever_asked_ = true;
  asked_mono_ = now_mono;
}

std::vector<DeviceSnapshot> DongleView::devices(double now_mono) const {
  if (!attached_) return {};
  if (current_.empty() && current_mono_ == 0.0) return {};
  if (now_mono - current_mono_ > kStaleAfter) return {};
  return current_;
}

}  // namespace octo
