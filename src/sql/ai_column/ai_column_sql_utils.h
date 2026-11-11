/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <string>
#include "lib/string/ob_string.h"
namespace oceanbase { namespace sql {
inline std::string ai_text(const common::ObString &s)
{ return s.empty() ? std::string() : std::string(s.ptr(), s.length()); }
inline std::string ai_identifier(const common::ObString &s)
{
  std::string out="`";
  for (char c : ai_text(s)) { out+=c; if (c=='`') out+=c; }
  return out+'`';
}
inline std::string ai_literal(const std::string &s)
{
  static const char hex[]="0123456789abcdef";
  std::string out="CONVERT(X'";
  for (unsigned char c:s) { out+=hex[c>>4]; out+=hex[c&15]; }
  return out+"' USING utf8mb4)";
}
} }
