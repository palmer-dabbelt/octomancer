// Talking to octomancerd. Shared by octomancerctl and the status-bar app so
// there is exactly one implementation of the handshake.
#ifndef OCTO_CLIENT_H
#define OCTO_CLIENT_H

#include <string>

#include "registry.h"

namespace octo {

// Send one command and read the whole reply. Blocking, with a deadline, so a
// wedged daemon cannot hang a UI's main thread indefinitely.
bool query(const std::string& socket_path, const std::string& command,
           std::string* reply, std::string* err, double timeout = 3.0);

// The common case: ask for a snapshot and parse it.
bool fetch(const std::string& socket_path, Snapshot* out, std::string* err,
           double timeout = 3.0);

}  // namespace octo

#endif  // OCTO_CLIENT_H
