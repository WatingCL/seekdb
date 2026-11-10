#include "value.hpp"

#include <cmath>
#include <cstdlib>
#include <float.h>

namespace mvdb {

size_t unicodeLen(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

int cmpStr(const std::string& a, const std::string& b) {
  size_t n = a.size() < b.size() ? a.size() : b.size();
  for (size_t k = 0; k < n; ++k) {
    unsigned char ca = static_cast<unsigned char>(a[k]);
    unsigned char cb = static_cast<unsigned char>(b[k]);
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

bool parseVectorLiteral(const std::string& text, std::vector<float>& out, std::string& err) {
  size_t p = 0;
  const size_t n = text.size();
  auto skipWs = [&] {
    while (p < n && (text[p] == ' ' || text[p] == '\t' || text[p] == '\n' || text[p] == '\r')) ++p;
  };
  skipWs();
  if (p >= n || text[p] != '[') {
    err = "vector literal must start with '['";
    return false;
  }
  ++p;
  skipWs();
  if (p < n && text[p] == ']') {
    ++p;
    skipWs();
    if (p != n) {
      err = "trailing garbage in vector literal";
      return false;
    }
    return true;
  }
  while (true) {
    skipWs();
    size_t start = p;
    if (p < n && (text[p] == '-' || text[p] == '+')) ++p;
    bool digits = false;
    while (p < n && text[p] >= '0' && text[p] <= '9') {
      ++p;
      digits = true;
    }
    if (p < n && text[p] == '.') {
      ++p;
      while (p < n && text[p] >= '0' && text[p] <= '9') {
        ++p;
        digits = true;
      }
    }
    if (!digits) {
      err = "bad number in vector literal";
      return false;
    }
    if (p < n && (text[p] == 'e' || text[p] == 'E')) {
      ++p;
      if (p < n && (text[p] == '-' || text[p] == '+')) ++p;
      if (p >= n || text[p] < '0' || text[p] > '9') {
        err = "bad exponent in vector literal";
        return false;
      }
      while (p < n && text[p] >= '0' && text[p] <= '9') ++p;
    }
    std::string tok = text.substr(start, p - start);
    double d = strtod(tok.c_str(), nullptr);
    if (!std::isfinite(d)) {
      err = "vector component is not finite";
      return false;
    }
    if (std::fabs(d) > FLT_MAX) {
      err = "vector component overflows float32";
      return false;
    }
    out.push_back(static_cast<float>(d));
    skipWs();
    if (p < n && text[p] == ',') {
      ++p;
      continue;
    }
    if (p < n && text[p] == ']') {
      ++p;
      skipWs();
      if (p != n) {
        err = "trailing garbage in vector literal";
        return false;
      }
      return true;
    }
    err = "expected ',' or ']' in vector literal";
    return false;
  }
}

}  // namespace mvdb
