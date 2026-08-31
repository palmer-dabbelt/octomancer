// See src/naming.h.
#include "naming.h"

namespace octo {

const char* name_source_name(NameSource source) {
  switch (source) {
    case NameSource::kUser: return "user";
    case NameSource::kProbed: return "probed";
    case NameSource::kHeard: return "heard";
    case NameSource::kNone: break;
  }
  return "none";
}

bool want_active_scan(bool active_now, int unnamed_live, double since_change) {
  const bool want = unnamed_live > 0;
  if (want == active_now) return active_now;
  // Hysteresis in time rather than in count, because the thing being damped is
  // the cost of restarting the scan, and that cost is per restart however many
  // devices provoked it.
  if (since_change < kScanSettleSeconds) return active_now;
  return want;
}

bool is_placeholder_name(const std::string& name) {
  // src/registry.cc substitutes the first when a device has never said what it
  // is called, and it travels over the wire looking exactly like a name --
  // four such devices arrive wearing the same one. The second is what a
  // renderer puts in a column.
  return name.empty() || name == "(unnamed)" || name == "(no name)";
}

std::string NameBook::display(const std::string& id, NameSource* from) const {
  const auto it = names_.find(id);
  if (it != names_.end()) {
    if (!it->second.user.empty()) {
      if (from != nullptr) *from = NameSource::kUser;
      return it->second.user;
    }
    if (!it->second.probed.empty()) {
      if (from != nullptr) *from = NameSource::kProbed;
      return it->second.probed;
    }
    if (!it->second.heard.empty()) {
      if (from != nullptr) *from = NameSource::kHeard;
      return it->second.heard;
    }
  }
  if (from != nullptr) *from = NameSource::kNone;
  return id;
}

void NameBook::rename(const std::string& id, const std::string& name) {
  if (id.empty()) return;
  if (name.empty()) {
    const auto it = names_.find(id);
    if (it == names_.end()) return;
    it->second.user.clear();
    if (it->second.empty()) names_.erase(it);
    return;
  }
  names_[id].user = name;
}

void NameBook::probed(const std::string& id, const std::string& name) {
  if (id.empty()) return;
  DeviceName& entry = names_[id];
  entry.probed_done = true;
  entry.probed = is_placeholder_name(name) ? std::string() : name;
}

void NameBook::heard(const std::string& id, const std::string& name) {
  if (id.empty() || is_placeholder_name(name)) return;
  names_[id].heard = name;
}

bool NameBook::refresh(const std::string& id) {
  const auto it = names_.find(id);
  if (it == names_.end()) return false;
  const std::string keep = it->second.user;
  names_.erase(it);
  if (!keep.empty()) names_[id].user = keep;
  return true;
}

bool NameBook::needs_probe(const std::string& id) const {
  const auto it = names_.find(id);
  if (it == names_.end()) return true;
  // A person's name does not stop a probe being useful -- it is still worth
  // knowing what the device calls itself -- but it does stop it being urgent,
  // and a probe costs a connection. So a named device is left alone.
  if (!it->second.user.empty()) return false;
  return !it->second.probed_done;
}

std::vector<std::string> NameBook::unprobed() const {
  std::vector<std::string> out;
  for (const auto& entry : names_) {
    if (needs_probe(entry.first)) out.push_back(entry.first);
  }
  return out;
}

void NameBook::put(const std::string& id, const DeviceName& name) {
  if (id.empty() || name.empty()) return;
  names_[id] = name;
}

}  // namespace octo
