/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Standalone differential test and optional hot-path microbenchmark.
// Link with the same runtime libraries as seekdb; no running server is needed.
#include "lib/charset/ob_charset.h"
#include "lib/charset/ob_charset_string_helper.h"
#include "lib/charset/ob_ctype.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/utils/unicode_utils.h"
#include <cstdint>
#include <type_traits>
// Exercise UTF-16 templates even in builds that do not register UTF-16 collations.
#define private public
#include "storage/fts/ik/ob_ik_char_util.h"
#undef private
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

using namespace oceanbase::common;
using namespace oceanbase::storage;
using CharType = ObFTCharUtil::CharType;

namespace
{
// Preserve the original sequence of decoding predicates as an independent oracle.
template <ObCharsetType CS>
int reference_classify(const char *input, uint8_t len, CharType &type)
{
  int ret = OB_SUCCESS;
  bool checker = false;
  type = CharType::USELESS;
  if (OB_FAIL(ObFTCharUtil::is_alpha<CS>(input, len, checker))) {
  } else if (checker) {
    type = CharType::ENGLISH_LETTER;
  } else if (OB_FAIL(ObFTCharUtil::is_arabic<CS>(input, len, checker))) {
  } else if (checker) {
    type = CharType::ARABIC_LETTER;
  } else if (OB_FAIL(ObFTCharUtil::is_chinese<CS>(input, len, checker))) {
  } else if (checker) {
    type = CharType::CHINESE;
  } else if (OB_FAIL(ObFTCharUtil::is_other_cjk<CS>(input, len, checker))) {
  } else if (checker) {
    type = CharType::OTHER_CJK;
  } else if (OB_FAIL(ObFTCharUtil::is_surrogate_high<CS>(input, len, checker))) {
  } else if (checker) {
    type = CharType::SURROGATE_HIGH;
  } else if (OB_FAIL(ObFTCharUtil::is_surrogate_low<CS>(input, len, checker))) {
  } else if (checker) {
    type = CharType::SURROGATE_LOW;
  }
  return ret;
}

template <ObCharsetType CS>
int compare(ObCollationType coll, const char *bytes, uint8_t len)
{
  CharType expected = CharType::USELESS;
  CharType actual = CharType::USELESS;
  const int expected_ret = reference_classify<CS>(bytes, len, expected);
  const int actual_ret = ObFTCharUtil::do_classify<CS>(bytes, len, actual);
  if (expected_ret != actual_ret || expected != actual) {
    std::fprintf(stderr, "collation=%d length=%u expected=(%d,%d) actual=(%d,%d)\n",
                 int(coll), unsigned(len), expected_ret, int(expected), actual_ret, int(actual));
    return 1;
  }
  return 0;
}

template <ObCharsetType CS>
int check_charset(ObCollationType coll)
{
  unsigned char bytes[8];
  int64_t count = 0;
  const ob_wc_t last_code = CS == CHARSET_UTF8MB4 ? 0x10ffff : 0xffff;
  for (ob_wc_t code = 0; code <= last_code; ++code) {
    int len = 2;
    if constexpr (CS == CHARSET_UTF8MB4) {
      len = ob_charset_encode_unicode<CS>(code, bytes, bytes + sizeof(bytes));
    } else if constexpr (CS == CHARSET_UTF16) {
      bytes[0] = code >> 8;
      bytes[1] = code & 0xff;
    } else {
      bytes[0] = code & 0xff;
      bytes[1] = code >> 8;
    }
    if (len > 0) {
      if (compare<CS>(coll, reinterpret_cast<const char *>(bytes), len)) {
        std::fprintf(stderr, "codepoint=U+%06lx\n", static_cast<unsigned long>(code));
        return 1;
      }
      ++count;
    }
  }
  // Truncation, zero length, invalid encodings and NULL retain their error/type behavior.
  if (compare<CS>(coll, nullptr, 0)) return 1;
  const char invalid[][4] = {{0}, {char(0xff),char(0xff),char(0xff),char(0xff)},
                            {char(0xed),char(0xa0),char(0x80),0},
                            {char(0xd8),0,0,0}, {0,char(0xd8),0,0}};
  for (const auto &input : invalid) {
    for (uint8_t len = 0; len <= 4; ++len) {
      if (compare<CS>(coll, input, len)) return 1;
    }
  }
  std::printf("collation=%d compared=%lld encoded inputs + invalid input cases\n",
              int(coll), static_cast<long long>(count));
  return 0;
}

int check_chinese_numerals()
{
  const char32_t *numerals = U"一二两三四五六七八九十零壹贰叁肆伍陆柒捌玖拾百千万亿拾佰仟萬億兆卅廿";
  for (ob_wc_t code = 0; code <= 0x10ffff; ++code) {
    bool expected = false;
    for (const char32_t *it = numerals; *it != 0; ++it) {
      if (code == *it) expected = true;
    }
    if (expected != ObUnicodeBlockUtils::is_unicode_cn_number(code)) {
      std::fprintf(stderr, "numeral mismatch at U+%06lx\n", static_cast<unsigned long>(code));
      return 1;
    }
  }
  std::printf("Chinese numeral membership: all 1114112 codepoints agree\n");
  return 0;
}

void benchmark()
{
  const char *text = "OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化包括解析器实例复用、停用词全局单例、内存池化数据结构等技术，在大量文档索引和查询场景下可以显著降低 CPU 开销。";
  const size_t size = std::strlen(text);
  const int rounds = 200000;
  volatile int64_t checksum = 0;
  double samples[2][6];
  // Balanced AB/BA ordering reduces warm-cache and frequency-scaling bias.
  for (int sample = 0; sample < 6; ++sample) {
    for (int order = 0; order < 2; ++order) {
      const int variant = (sample + order) % 2;
      const auto begin = std::chrono::steady_clock::now();
      for (int round = 0; round < rounds; ++round) {
        for (size_t i = 0; i < size;) {
          const unsigned char *start = reinterpret_cast<const unsigned char *>(text + i);
          const int len = ob_charset_char_len<CHARSET_UTF8MB4>(start, start + size - i);
          CharType type;
          if (variant == 0) {
            if (ObCharset::charset_type_by_coll(CS_TYPE_UTF8MB4_GENERAL_CI) == CHARSET_UTF8MB4) {
              reference_classify<CHARSET_UTF8MB4>(text + i, len, type);
            } else {
              type = CharType::USELESS;
            }
          } else {
            ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_GENERAL_CI, text + i, len, type);
          }
          checksum = checksum + int(type);
          i += len;
        }
      }
      samples[variant][sample] = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - begin).count();
      std::printf("%s sample=%d rounds=%d milliseconds=%.3f\n",
                  variant == 0 ? "reference" : "single_decode", sample, rounds,
                  samples[variant][sample]);
    }
  }
  for (auto &variant_samples : samples) std::sort(variant_samples, variant_samples + 6);
  const double old_median = (samples[0][2] + samples[0][3]) / 2;
  const double new_median = (samples[1][2] + samples[1][3]) / 2;
  std::printf("median_reference_ms=%.3f median_single_decode_ms=%.3f reduction=%.2f%% checksum=%lld\n",
              old_median, new_median, (old_median - new_median) * 100 / old_median,
              static_cast<long long>(checksum));
}

} // namespace

int main(int argc, char **argv)
{
  if (ObCharset::init_charset() != OB_SUCCESS) return 1;
  if (check_chinese_numerals()
      || check_charset<CHARSET_UTF8MB4>(CS_TYPE_UTF8MB4_GENERAL_CI)
      || check_charset<CHARSET_UTF16>(CS_TYPE_UTF16_GENERAL_CI)
      || check_charset<CHARSET_UTF16LE>(CS_TYPE_UTF16LE_GENERAL_CI)) {
    return 1;
  }
  if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) {
    benchmark();
  }
  return 0;
}
