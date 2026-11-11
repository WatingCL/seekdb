#include "json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>

namespace mvdb {

Json Json::object() {
  Json j;
  j.t = JType::Object;
  return j;
}
Json Json::array() {
  Json j;
  j.t = JType::Array;
  return j;
}
Json Json::str(std::string v) {
  Json j;
  j.t = JType::String;
  j.s = std::move(v);
  return j;
}
Json Json::integer(long long v) {
  Json j;
  j.t = JType::Int;
  j.i = v;
  return j;
}
Json Json::real(double v) {
  Json j;
  j.t = JType::Double;
  j.d = v;
  return j;
}
Json Json::boolean(bool v) {
  Json j;
  j.t = JType::Bool;
  j.b = v;
  return j;
}

const Json* Json::find(const std::string& key) const {
  for (const auto& kv : obj)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

void Json::set(const std::string& key, Json v) {
  for (auto& kv : obj)
    if (kv.first == key) {
      kv.second = std::move(v);
      return;
    }
  obj.emplace_back(key, std::move(v));
}

namespace {

void dumpString(const std::string& s, std::string& out) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

void appendUtf8(std::string& out, unsigned cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

struct Parser {
  const char* p;
  const char* end;
  std::string err;

  bool fail(const std::string& m) {
    err = m;
    return false;
  }
  void skipWs() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  }
  bool parseUnicodeEscape(std::string& out) {
    if (end - p < 4) return fail("truncated \\u escape");
    unsigned cp = 0;
    for (int k = 0; k < 4; ++k) {
      int h = hexVal(p[k]);
      if (h < 0) return fail("bad \\u escape");
      cp = (cp << 4) | static_cast<unsigned>(h);
    }
    p += 4;
    if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
      unsigned lo = 0;
      bool ok = true;
      for (int k = 0; k < 4; ++k) {
        int h = hexVal(p[2 + k]);
        if (h < 0) {
          ok = false;
          break;
        }
        lo = (lo << 4) | static_cast<unsigned>(h);
      }
      if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
        p += 6;
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
      }
    }
    appendUtf8(out, cp);
    return true;
  }
  bool parseString(std::string& out) {
    if (p >= end || *p != '"') return fail("expected string");
    ++p;
    while (p < end) {
      unsigned char c = static_cast<unsigned char>(*p);
      if (c == '"') {
        ++p;
        return true;
      }
      if (c == '\\') {
        ++p;
        if (p >= end) return fail("truncated escape");
        char e = *p++;
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u':
            if (!parseUnicodeEscape(out)) return false;
            break;
          default: return fail("bad escape");
        }
      } else if (c < 0x20) {
        return fail("raw control char in string");
      } else {
        out.push_back(static_cast<char>(c));
        ++p;
      }
    }
    return fail("unterminated string");
  }
  bool parseNumber(Json& out) {
    const char* start = p;
    if (p < end && (*p == '-' || *p == '+')) ++p;
    bool isFloat = false;
    while (p < end) {
      char c = *p;
      if ((c >= '0' && c <= '9')) {
        ++p;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
        if (c == '.' || c == 'e' || c == 'E') isFloat = true;
        ++p;
      } else {
        break;
      }
    }
    size_t len = static_cast<size_t>(p - start);
    if (len == 0) return fail("expected number");
    std::string tok(start, len);
    if (isFloat) {
      out.t = JType::Double;
      out.d = strtod(tok.c_str(), nullptr);
    } else {
      errno = 0;
      char* numEnd = nullptr;
      long long v = strtoll(tok.c_str(), &numEnd, 10);
      if (errno == ERANGE || numEnd != tok.c_str() + len) {
        out.t = JType::Double;
        out.d = strtod(tok.c_str(), nullptr);
      } else {
        out.t = JType::Int;
        out.i = v;
      }
    }
    return true;
  }
  bool parseValue(Json& out) {
    skipWs();
    if (p >= end) return fail("unexpected end");
    char c = *p;
    if (c == '{') {
      ++p;
      out.t = JType::Object;
      skipWs();
      if (p < end && *p == '}') {
        ++p;
        return true;
      }
      while (true) {
        skipWs();
        std::string key;
        if (!parseString(key)) return false;
        skipWs();
        if (p >= end || *p != ':') return fail("expected ':'");
        ++p;
        Json v;
        if (!parseValue(v)) return false;
        out.set(key, std::move(v));
        skipWs();
        if (p < end && *p == ',') {
          ++p;
          continue;
        }
        if (p < end && *p == '}') {
          ++p;
          return true;
        }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++p;
      out.t = JType::Array;
      skipWs();
      if (p < end && *p == ']') {
        ++p;
        return true;
      }
      while (true) {
        Json v;
        if (!parseValue(v)) return false;
        out.arr.push_back(std::move(v));
        skipWs();
        if (p < end && *p == ',') {
          ++p;
          continue;
        }
        if (p < end && *p == ']') {
          ++p;
          return true;
        }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      out.t = JType::String;
      return parseString(out.s);
    }
    if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
      p += 4;
      out.t = JType::Bool;
      out.b = true;
      return true;
    }
    if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
      p += 5;
      out.t = JType::Bool;
      out.b = false;
      return true;
    }
    if (end - p >= 4 && memcmp(p, "null", 4) == 0) {
      p += 4;
      out.t = JType::Null;
      return true;
    }
    return parseNumber(out);
  }
};

}  // namespace

bool Json::parse(const std::string& text, Json& out, std::string& err) {
  Parser ps{text.data(), text.data() + text.size(), {}};
  out = Json();
  if (!ps.parseValue(out)) {
    err = ps.err;
    return false;
  }
  ps.skipWs();
  if (ps.p != ps.end) {
    err = "trailing garbage";
    return false;
  }
  return true;
}

void Json::dumpTo(std::string& out) const {
  char buf[64];
  switch (t) {
    case JType::Null: out += "null"; break;
    case JType::Bool: out += b ? "true" : "false"; break;
    case JType::Int: {
      auto r = std::to_chars(buf, buf + sizeof(buf), i);
      out.append(buf, static_cast<size_t>(r.ptr - buf));
      break;
    }
    case JType::Double: {
      if (!std::isfinite(d)) {
        out += "null";
        break;
      }
      // Shortest representation that round-trips; portable across libstdc++ versions.
      for (int prec = 1; prec <= 17; ++prec) {
        int n = snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (n > 0 && strtod(buf, nullptr) == d) {
          out.append(buf, static_cast<size_t>(n));
          break;
        }
      }
      break;
    }
    case JType::String: dumpString(s, out); break;
    case JType::Array: {
      out.push_back('[');
      for (size_t k = 0; k < arr.size(); ++k) {
        if (k) out.push_back(',');
        arr[k].dumpTo(out);
      }
      out.push_back(']');
      break;
    }
    case JType::Object: {
      out.push_back('{');
      for (size_t k = 0; k < obj.size(); ++k) {
        if (k) out.push_back(',');
        dumpString(obj[k].first, out);
        out.push_back(':');
        obj[k].second.dumpTo(out);
      }
      out.push_back('}');
      break;
    }
  }
}

std::string Json::dump() const {
  std::string out;
  dumpTo(out);
  return out;
}

}  // namespace mvdb
