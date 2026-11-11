/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <limits>
#include <map>
#include "storage/fts/ik/ik_chain_lookup.h"
#include "storage/fts/ik/ob_ik_token.h"
#include "lib/allocator/page_arena.h"
#include "lib/utility/data_buffer.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

TEST(IkCharacterClassification, ChainLookupMatchesMap)
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
    EXPECT_EQ(inserted ? OB_SUCCESS : OB_HASH_EXIST, lookup.insert(offset, value));
    ObIKTokenChain *found = nullptr;
    ASSERT_EQ(OB_SUCCESS, lookup.find(offset, found));
    EXPECT_EQ(reference.at(offset), found);
  }
  EXPECT_EQ(reference.size(), lookup.size());
  for (int64_t offset = -550; offset <= 550; ++offset) {
    ObIKTokenChain *found = &a;
    const auto it = reference.find(offset);
    EXPECT_EQ(it == reference.end() ? OB_HASH_NOT_EXIST : OB_SUCCESS, lookup.find(offset, found));
    EXPECT_EQ(it == reference.end() ? &a : it->second, found);
  }
  for (const int64_t offset : {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()}) {
    ASSERT_EQ(OB_SUCCESS, lookup.insert(offset, &c));
    ObIKTokenChain *found = nullptr;
    ASSERT_EQ(OB_SUCCESS, lookup.find(offset, found));
    EXPECT_EQ(&c, found);
  }
  lookup.reset();
  EXPECT_EQ(0, lookup.size());
  ASSERT_EQ(OB_SUCCESS, lookup.insert(0, &b));
  ObIKTokenChain *found = nullptr;
  ASSERT_EQ(OB_SUCCESS, lookup.find(0, found));
  EXPECT_EQ(&b, found);
}

TEST(IkCharacterClassification, ChainLookupOrderedAndDuplicateKeys)
{
  ObArenaAllocator allocator;
  ObIKTokenChain a(allocator), b(allocator);
  IkChainLookup lookup(allocator);
  for (int64_t i = 0; i < 256; ++i) ASSERT_EQ(OB_SUCCESS, lookup.insert(i * 3, &a));
  EXPECT_EQ(OB_HASH_EXIST, lookup.insert(300, &b));
  ObIKTokenChain *found = nullptr;
  ASSERT_EQ(OB_SUCCESS, lookup.find(300, found));
  EXPECT_EQ(&a, found);
  EXPECT_EQ(OB_HASH_NOT_EXIST, lookup.find(301, found));
}

TEST(IkCharacterClassification, ChainLookupAllocationFailureKeepsEntries)
{
  ObArenaAllocator allocator;
  ObIKTokenChain value(allocator);
  char storage[1];
  ObDataBuffer no_heap(storage, 0);
  IkChainLookup lookup(no_heap);
  for (int64_t i = 0; i < 16; ++i) ASSERT_EQ(OB_SUCCESS, lookup.insert(i, &value));
  EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, lookup.insert(-1, &value));
  EXPECT_EQ(16, lookup.size());
  for (int64_t i = 0; i < 16; ++i) {
    ObIKTokenChain *found = nullptr;
    ASSERT_EQ(OB_SUCCESS, lookup.find(i, found));
    EXPECT_EQ(&value, found);
  }
  ObIKTokenChain *missing = nullptr;
  EXPECT_EQ(OB_HASH_NOT_EXIST, lookup.find(-1, missing));
  lookup.reset();
  EXPECT_EQ(OB_SUCCESS, lookup.insert(-1, &value));
}
