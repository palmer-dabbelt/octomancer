// The wire format between octomancerd and anything that wants to look at it.
//
// Deliberately a line protocol of escaped key=value pairs rather than JSON.
// Every consumer here is a C++ program that would otherwise need a JSON
// parser, and a hand-rolled parser is exactly the kind of dependency-avoidance
// that turns into a supply of subtle bugs. Splitting on spaces and then on the
// first '=' has no edge cases once values are escaped.
//
// Unknown keys must be ignored by readers, so the daemon can grow fields
// without breaking a UI built against an older version.
//
//   octomancer 1
//   snapshot wall=... uptime=... radio=poweredOn devices=5 live=5 ...
//   device id=... name=Krysta offset=-6.2314 ...
//   device id=... name=FS7 ...
//   end
//
// JSON is still offered, for the benefit of everything that is not this
// program -- jq, a scratch Python script, a future web view.
#ifndef OCTO_PROTO_H
#define OCTO_PROTO_H

#include <string>

#include "registry.h"

namespace octo {

inline constexpr int kProtocolVersion = 1;

// Percent-escape anything that would break the tokenizer: space, '=', '%',
// and every byte outside printable ASCII. Box names are user-set and arrive
// from the air, so they are assumed hostile.
std::string escape(const std::string& in);
std::string unescape(const std::string& in);

std::string render_text(const Snapshot& snap);
std::string render_json(const Snapshot& snap);

// Parse what render_text produced. Returns false and sets *err on anything it
// does not recognise as a snapshot.
bool parse_text(const std::string& text, Snapshot* out, std::string* err);

}  // namespace octo

#endif  // OCTO_PROTO_H
