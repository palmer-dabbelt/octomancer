// Percent-escaping, so that a value can never break the tokenizer around it.
//
// This is the bottom layer of both line protocols in the tree: proto.h's
// snapshot format on the daemon socket, and boxmsg.h's message format on the
// box's three transports. It lives on its own because those two are otherwise
// unrelated, and because the box needs this without needing Snapshot -- which
// on a device matters, since registry.h is one of the headers that cannot be
// compiled for a target with no std::mutex.
//
// Box names are user-set and arrive from the air, so they are assumed hostile:
// space, '=', '%' and every byte outside printable ASCII are escaped, which
// leaves splitting on spaces and then on the first '=' with no edge cases.
#ifndef OCTO_ESCAPE_H
#define OCTO_ESCAPE_H

#include <string>

namespace octo {

std::string escape(const std::string& in);
std::string unescape(const std::string& in);

}  // namespace octo

#endif  // OCTO_ESCAPE_H
