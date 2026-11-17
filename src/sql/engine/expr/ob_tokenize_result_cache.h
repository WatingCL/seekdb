/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "lib/charset/ob_charset.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{
// One exact result for an immutable builtin analyzer. No caller-owned pointers
// survive put(); get() returns bytes which the expression copies to its arena.
class TokenizeResultCache final
{
public:
  static constexpr int64_t MAX_NAME_BYTES = 64;
  static constexpr int64_t MAX_PROPERTIES_BYTES = 1024;
  static constexpr int64_t MAX_INPUT_BYTES = 4096;
  static constexpr int64_t MAX_RESULT_BYTES = 64 * 1024;

  bool get(const common::ObString &name, const common::ObString &properties,
           const common::ObString &input, common::ObCollationType collation,
           int64_t mode, common::ObString &result)
  {
    const bool hit = valid_ && collation_ == collation && mode_ == mode
        && equal(name_, name_len_, name) && equal(properties_, properties_len_, properties)
        && equal(input_, input_len_, input);
    if (hit) {
      result.assign_ptr(result_, result_len_);
    }
    return hit;
  }

  void put(const common::ObString &name, const common::ObString &properties,
           const common::ObString &input, common::ObCollationType collation,
           int64_t mode, const common::ObString &result)
  {
    valid_ = false;
    if (name.length() <= MAX_NAME_BYTES && properties.length() <= MAX_PROPERTIES_BYTES
        && input.length() <= MAX_INPUT_BYTES && result.length() <= MAX_RESULT_BYTES) {
      copy(name_, name_len_, name);
      copy(properties_, properties_len_, properties);
      copy(input_, input_len_, input);
      copy(result_, result_len_, result);
      collation_ = collation;
      mode_ = mode;
      valid_ = true;
    }
  }

private:
  static bool equal(const char *bytes, int32_t length, const common::ObString &value)
  {
    return length == value.length() && (length == 0 || MEMCMP(bytes, value.ptr(), length) == 0);
  }
  static void copy(char *bytes, int32_t &length, const common::ObString &value)
  {
    length = value.length();
    if (length > 0) {
      MEMCPY(bytes, value.ptr(), length);
    }
  }
  char name_[MAX_NAME_BYTES];
  char properties_[MAX_PROPERTIES_BYTES];
  char input_[MAX_INPUT_BYTES];
  char result_[MAX_RESULT_BYTES];
  int32_t name_len_ = 0;
  int32_t properties_len_ = 0;
  int32_t input_len_ = 0;
  int32_t result_len_ = 0;
  common::ObCollationType collation_ = common::CS_TYPE_INVALID;
  int64_t mode_ = 0;
  bool valid_ = false;
};
} // namespace sql
} // namespace oceanbase
