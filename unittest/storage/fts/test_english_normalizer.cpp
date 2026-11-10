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

#include "share/text_analysis/ob_token_stream.h"
#include "lib/charset/ob_charset.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace oceanbase::common;
using namespace oceanbase::share;

int reference_tokens(const ObCharsetInfo *cs, const ObDatum &doc,
                     std::vector<std::string> &result)
{
  ObTextWhitespaceTokenizer tokenizer;
  int ret = tokenizer.open(doc, cs);
  ObArenaAllocator arena;
  ObDatum token;
  int64_t freq = 0;
  while (ret == OB_SUCCESS && (ret = tokenizer.get_next(token, freq)) == OB_SUCCESS) {
    const char *text = token.ptr_;
    const uint32_t length = token.len_;
    uint32_t first = 0;
    while (first < length && !ob_isalnum(cs, text[first])) ++first;
    if (first == length) continue;
    uint32_t last = length;
    while (last > first && !ob_isalnum(cs, text[last - 1])) --last;
    ObString lower;
    if ((ret = ObCharset::tolower(cs, ObString(last - first, text + first), lower, arena)) != OB_SUCCESS) break;
    result.emplace_back(lower.ptr(), lower.length());
    arena.reuse();
  }
  return ret == OB_ITER_END ? OB_SUCCESS : ret;
}

int check_document(ObCollationType coll, const std::string &input)
{
  const ObCharsetInfo *cs = ObCharset::get_charset(coll);
  ObDatum doc;
  doc.set_string(input.data(), input.size());
  std::vector<std::string> expected;
  int ret = reference_tokens(cs, doc, expected);
  if (ret != OB_SUCCESS) {std::fprintf(stderr,"reference returned %d\n",ret); return ret;}
  ObTextWhitespaceTokenizer tokenizer;
  ObBasicEnglishNormalizer normalizer;
  if ((ret = tokenizer.open(doc, cs)) != OB_SUCCESS) {std::fprintf(stderr,"open=%d\n",ret); return ret;}
  if ((ret = normalizer.init(cs, tokenizer)) != OB_SUCCESS) {std::fprintf(stderr,"normalizer.init=%d\n",ret); return ret;}
  std::vector<std::string> actual;
  ObDatum token;
  int64_t freq = 0;
  while ((ret = normalizer.get_next(token, freq)) == OB_SUCCESS) {
    if (freq != 1) {std::fprintf(stderr,"freq=%lld\n",(long long)freq); return 1;}
    actual.emplace_back(token.ptr_, static_cast<size_t>(token.len_));
  }
  if (ret != OB_ITER_END || expected != actual) {
    std::fprintf(stderr, "English normalization mismatch for collation %d\n", int(coll));
    return 1;
  }
  return OB_SUCCESS;
}

int main()
{
  if (ObCharset::init_charset() != OB_SUCCESS) return 1;
  const char *cases[] = {
    "database performance audit",
    "Database PERFORMANCE AuDiT",
    "...database!!! a-b@example.com 123.45 don't end.",
    " !!! -- () [] {} ",
    "ÀÉÖ àéö İstanbul İSTANBUL Ελληνικά ΕΛΛΗΝΙΚΑ ＡＢＣ 中文",
    "the quick brown fox jumps over the lazy dog",
    "one\ttwo\nthree\r\nfour",
  };
  for (auto coll : {CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN}) {
    for (const char *input : cases) {
      if (check_document(coll, input) != OB_SUCCESS) { std::fprintf(stderr, "case failed: %s\n", input); return 1; }
    }
    if (check_document(coll, std::string(8192, 'x') + " " + std::string(8192, 'X')) != OB_SUCCESS) return 1;
  }
  // The ASCII fast path borrows the document; the next token may still require
  // owned normalization memory. Exercise both paths in the same stream.
  const std::string input = "database DATABASE database";
  ObDatum doc;
  doc.set_string(input.data(), input.size());
  ObTextWhitespaceTokenizer tokenizer;
  ObBasicEnglishNormalizer normalizer;
  const auto *cs = ObCharset::get_charset(CS_TYPE_UTF8MB4_GENERAL_CI);
  if (tokenizer.open(doc, cs) != OB_SUCCESS || normalizer.init(cs, tokenizer) != OB_SUCCESS) { std::fprintf(stderr, "init failed\n"); return 1; }
  ObDatum token;
  int64_t freq = 0;
  if (normalizer.get_next(token, freq) != OB_SUCCESS || token.ptr_ != input.data()) { std::fprintf(stderr, "first slice mismatch ptr=%p input=%p token=%.*s\n", token.ptr_, input.data(), int(token.len_), token.ptr_); return 1; }
  if (normalizer.get_next(token, freq) != OB_SUCCESS || std::string(token.ptr_, token.len_) != "database") return 1;
  if (normalizer.get_next(token, freq) != OB_SUCCESS || token.ptr_ != input.data() + 18) { std::fprintf(stderr, "last slice mismatch\n"); return 1; }
  std::puts("English normalization: reference outputs, mixed fast/fallback paths and long tokens agree");
  return 0;
}
