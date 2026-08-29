#include "escape.h"

namespace octo {
namespace {

const char kHexDigits[] = "0123456789ABCDEF";

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string escape(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (unsigned char c : in) {
    const bool safe = c > 0x20 && c < 0x7f && c != '%' && c != '=';
    if (safe) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHexDigits[c >> 4]);
      out.push_back(kHexDigits[c & 0x0F]);
    }
  }
  // An empty value would vanish into "key=" and then into a token with no
  // value at all, so give it a body the parser can see.
  if (out.empty()) out = "%00";
  return out;
}

std::string unescape(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size()) {
      const int hi = hex_value(in[i + 1]), lo = hex_value(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        const char c = static_cast<char>(hi << 4 | lo);
        if (c != '\0') out.push_back(c);
        i += 2;
        continue;
      }
    }
    out.push_back(in[i]);
  }
  return out;
}

}  // namespace octo
