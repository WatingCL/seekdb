/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include "storage/fts/utils/unicode_utils.h"
#define CHECK_EQ(a, b) do { if ((a) != (b)) { std::fprintf(stderr, "check failed at line %d\n", __LINE__); std::abort(); } } while (false)
#include <limits>
#include <string>
#include "lib/charset/ob_charset.h"
#include "storage/fts/ik/ob_ik_arbitrator.h"
#include "storage/fts/ik/ob_ik_processor.h"
#include <map>
#include "storage/fts/ik/ik_chain_lookup.h"
#include "storage/fts/ik/ob_ik_token.h"
#include "lib/allocator/page_arena.h"
#include "lib/utility/data_buffer.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

void ChainLookupMatchesMap()
{
  ObArenaAllocator allocator;
  ObIKTokenChain a(allocator), b(allocator), c(allocator);
  ObIKTokenChain *values[] = {&a, &b, &c};
  IkChainLookup lookup(allocator);
  std::map<int64_t, ObIKTokenChain *> reference;
  uint32_t state = 0x9e3779b9;
  for (int i = 0; i < 2000; ++i) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    const int64_t offset = int64_t(state % 1024) - 512;
    auto *value = values[i % 3];
    const bool inserted = reference.emplace(offset, value).second;
    CHECK_EQ(inserted ? OB_SUCCESS : OB_HASH_EXIST, lookup.insert(offset, value));
    ObIKTokenChain *found = nullptr;
    CHECK_EQ(OB_SUCCESS, lookup.find(offset, found));
    CHECK_EQ(reference.at(offset), found);
  }
  CHECK_EQ(reference.size(), lookup.size());
  for (int64_t offset = -550; offset <= 550; ++offset) {
    ObIKTokenChain *found = &a;
    const auto it = reference.find(offset);
    CHECK_EQ(it == reference.end() ? OB_HASH_NOT_EXIST : OB_SUCCESS, lookup.find(offset, found));
    CHECK_EQ(it == reference.end() ? &a : it->second, found);
  }
  for (const int64_t offset : {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()}) {
    CHECK_EQ(OB_SUCCESS, lookup.insert(offset, &c));
    ObIKTokenChain *found = nullptr;
    CHECK_EQ(OB_SUCCESS, lookup.find(offset, found));
    CHECK_EQ(&c, found);
  }
  lookup.reset();
  CHECK_EQ(0, lookup.size());
  CHECK_EQ(OB_SUCCESS, lookup.insert(0, &b));
  ObIKTokenChain *found = nullptr;
  CHECK_EQ(OB_SUCCESS, lookup.find(0, found));
  CHECK_EQ(&b, found);
}

void ChainLookupOrderedAndDuplicateKeys()
{
  ObArenaAllocator allocator;
  ObIKTokenChain a(allocator), b(allocator);
  IkChainLookup lookup(allocator);
  for (int64_t i = 0; i < 256; ++i) CHECK_EQ(OB_SUCCESS, lookup.insert(i * 3, &a));
  CHECK_EQ(OB_HASH_EXIST, lookup.insert(300, &b));
  ObIKTokenChain *found = nullptr;
  CHECK_EQ(OB_SUCCESS, lookup.find(300, found));
  CHECK_EQ(&a, found);
  CHECK_EQ(OB_HASH_NOT_EXIST, lookup.find(301, found));
}

void ChainLookupAllocationFailureKeepsEntries()
{
  ObArenaAllocator allocator;
  ObIKTokenChain value(allocator);
  char storage[1];
  ObDataBuffer no_heap(storage, 0);
  IkChainLookup lookup(no_heap);
  for (int64_t i = 0; i < 16; ++i) CHECK_EQ(OB_SUCCESS, lookup.insert(i, &value));
  CHECK_EQ(OB_ALLOCATE_MEMORY_FAILED, lookup.insert(-1, &value));
  CHECK_EQ(16, lookup.size());
  for (int64_t i = 0; i < 16; ++i) {
    ObIKTokenChain *found = nullptr;
    CHECK_EQ(OB_SUCCESS, lookup.find(i, found));
    CHECK_EQ(&value, found);
  }
  ObIKTokenChain *missing = nullptr;
  CHECK_EQ(OB_HASH_NOT_EXIST, lookup.find(-1, missing));
  lookup.reset();
  CHECK_EQ(OB_SUCCESS, lookup.insert(-1, &value));
}

// Exercise the actual arbitration/output path across the inline-array boundary.
void ArbitrationPreservesOffsets()
{
  for (bool smart : {false, true}) {
    ObArenaAllocator allocator;
    std::string text;
    for (int i = 0; i < 64; ++i) text += "中文 ";
    TokenizeContext context(CS_TYPE_UTF8MB4_GENERAL_CI, allocator,
                           text.data(), text.size(), smart);
    CHECK_EQ(OB_SUCCESS, context.init());
    for (int i = 0; i < 64; ++i) {
      CHECK_EQ(OB_SUCCESS, context.add_token(text.data(), i * 7, 6, 2,
                                            ObIKTokenType::IK_CHINESE_TOKEN));
    }
    ObIKArbitrator arbitrator;
    CHECK_EQ(OB_SUCCESS, arbitrator.process(context));
    CHECK_EQ(OB_SUCCESS, arbitrator.output_result(context));
    CHECK_EQ(64, context.result_list().size());
    for (int i = 0; i < 64; ++i) {
      const auto &token = context.result_list().get_first();
      CHECK_EQ(i * 7, token.offset_);
      CHECK_EQ(6, token.length_);
      CHECK_EQ(2, token.char_cnt_);
      CHECK_EQ(OB_SUCCESS, context.result_list().pop_front());
    }
  }
}

uint64_t OverlappingArbitrationChecksum()
{
  uint64_t checksum = 0;
  uint32_t state = 0x9e3779b9;
  for (bool smart : {false, true}) {
    for (int sample = 0; sample < 100; ++sample) {
      ObArenaAllocator allocator;
      std::string text;
      for (int i = 0; i < 8; ++i) text += "中文数字 ";
      TokenizeContext context(CS_TYPE_UTF8MB4_GENERAL_CI, allocator,
                             text.data(), text.size(), smart);
      CHECK_EQ(OB_SUCCESS, context.init());
      for (int group = 0; group < 8; ++group) {
        for (int position = 0; position < 4; ++position) {
          state ^= state << 13; state ^= state >> 17; state ^= state << 5;
          const int length = 1 + state % (4 - position);
          CHECK_EQ(OB_SUCCESS, context.add_token(text.data(), group * 13 + position * 3,
                                                length * 3, length,
                                                ObIKTokenType::IK_CHINESE_TOKEN));
        }
      }
      ObIKArbitrator arbitrator;
      CHECK_EQ(OB_SUCCESS, arbitrator.process(context));
      CHECK_EQ(OB_SUCCESS, arbitrator.output_result(context));
      while (!context.result_list().empty()) {
        const auto &token = context.result_list().get_first();
        checksum = checksum * 131 + token.offset_ * 17 + token.length_;
        CHECK_EQ(OB_SUCCESS, context.result_list().pop_front());
      }
    }
  }
  return checksum;
}

int main(int argc, char **argv)
{
  ObCharset::init_charset();
  ArbitrationPreservesOffsets();
  // Captured independently from the original 8e972d7 arbitration implementation.
  const uint64_t checksum = OverlappingArbitrationChecksum();
  CHECK_EQ(UINT64_C(3054835577729102278), checksum);
  std::printf("overlapping_arbitration_checksum: %llu\n",
              static_cast<unsigned long long>(checksum));
  ChainLookupMatchesMap();
  ChainLookupOrderedAndDuplicateKeys();
  ChainLookupAllocationFailureKeepsEntries();
  const char32_t numerals[] = U"一二两三四五六七八九十零壹贰叁肆伍陆柒捌玖拾百千万亿拾佰仟萬億兆卅廿";
  for (uint32_t code = 0; code <= 0x10FFFF; ++code) {
    bool expected = false;
    for (const auto numeral : numerals) {
      if (numeral != 0 && code == numeral) expected = true;
    }
    CHECK_EQ(expected, ObUnicodeBlockUtils::is_unicode_cn_number(code));
  }
  std::puts("Chain lookup, arbitration and exhaustive Unicode numeral checks passed");
  if (argc > 1 && std::strcmp(argv[1], "--benchmark") == 0) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 300000; ++i) ArbitrationPreservesOffsets();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("arbitration_600000_documents_sec: %.6f\n", seconds);
  }
}
