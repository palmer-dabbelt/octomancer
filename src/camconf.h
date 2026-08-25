// Per-camera configuration: what a person has decided about each body.
//
// Deliberately not the same file as camdb.h. That one is the daemon's
// notebook -- learned biases, measured apply delays, write history -- and the
// daemon owns it and rewrites it constantly. This one is the opposite: a
// person owns it, the tools write it, and **the daemon only ever reads it**.
// Nothing here is ever inferred, learned, or updated behind someone's back, so
// a setting that says "do not touch this camera" cannot quietly become
// something else overnight.
//
// It is meant to be opened in a text editor, which is why it is a commented
// line format rather than JSON, and why rewriting it preserves every line the
// writer did not have a reason to change -- comments, ordering, and settings
// this version has never heard of. A configuration file that eats your
// comments is a configuration file you stop editing.
//
//   # octomancer camera configuration
//   default writes=off
//   camera 09EE26AF-...  writes=on  name=A:1EAE18A7
//
// The one setting so far is `writes`, and it defaults to **off**. Octomancer
// connects to cameras and changes their clocks; doing that to a body someone
// has not explicitly named is not a default worth having. The cost is that a
// fresh install syncs nothing until a camera is enabled, which the daemon says
// out loud rather than leaving to be discovered.
#ifndef OCTO_CAMCONF_H
#define OCTO_CAMCONF_H

#include <string>
#include <vector>

namespace octo {

struct CameraConfig {
  std::string id;
  std::string name;   // a label for the file's benefit; matching is on id
  bool writes_enabled = false;
};

// ~/.octomancer/cameras.conf
std::string default_camera_config_path();

class CamConf {
 public:
  CamConf() = default;

  // Read the file. A file that does not exist is not an error -- it is a
  // fresh install, and every camera is disabled, which is the same answer the
  // file would have given. A file that cannot be *parsed* is an error worth
  // reporting, because the alternative is silently ignoring what someone
  // wrote.
  bool load(const std::string& path, std::string* err);

  // Re-read whatever was loaded last. This is what the daemon does when it is
  // told to reload.
  bool reload(std::string* err);

  const std::string& path() const { return path_; }
  bool loaded() const { return loaded_; }
  // Whether the file itself exists, as opposed to having been read.
  bool file_exists() const { return exists_; }

  const std::vector<CameraConfig>& cameras() const { return cameras_; }
  const CameraConfig* find(const std::string& id) const;

  // The question the daemon actually asks. An unknown camera gets the default,
  // which is off unless the file says otherwise.
  bool writes_enabled(const std::string& id) const;
  bool default_writes_enabled() const { return default_writes_; }

  // Whether any camera at all may be written to. Used to say something useful
  // at startup rather than leaving someone watching a daemon that has quietly
  // decided to do nothing.
  bool any_writes_enabled() const;

  // --- writing, which only the tools do -------------------------------
  //
  // Rewrites the file in place, keeping every other line exactly as it was.
  // Creates it, with a header explaining the format, if it is not there yet.
  bool set_writes(const std::string& id, const std::string& name, bool enabled,
                  std::string* err);

 private:
  bool parse(const std::string& text, std::string* err);

  std::string path_;
  bool loaded_ = false;
  bool exists_ = false;
  bool default_writes_ = false;
  std::vector<CameraConfig> cameras_;
};

}  // namespace octo

#endif  // OCTO_CAMCONF_H
