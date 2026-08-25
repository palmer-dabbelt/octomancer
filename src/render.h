// Turning a snapshot into something a person reads. Shared by octomancerctl
// and by the daemon's --probe mode so both tell the same story.
#ifndef OCTO_RENDER_H
#define OCTO_RENDER_H

#include <string>

#include "registry.h"

namespace octo {

// `color` adds ANSI attributes; callers should pass isatty(1).
std::string render_human(const Snapshot& snap, bool color);

}  // namespace octo

#endif  // OCTO_RENDER_H
