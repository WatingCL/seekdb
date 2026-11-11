/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase {
namespace storage {
class ObIKTokenChain;

// Arbitration only looks up chains by their starting byte offset. A small,
// ordered array avoids allocating hash buckets and nodes for every document.
// No token or chain is owned here; the arbitrator's arena owns their lifetime.
class IkChainLookup
{
public:
  explicit IkChainLookup(common::ObIAllocator &allocator)
    : entries_(sizeof(Entry) * 32, common::ObWrapperAllocator(allocator)) {}

  int insert(int64_t offset, ObIKTokenChain *chain)
  {
    int ret = common::OB_SUCCESS;
    const int64_t position = lower_bound(offset);
    if (position < entries_.count() && entries_.at(position).offset_ == offset) {
      ret = common::OB_HASH_EXIST;
    } else if (OB_FAIL(entries_.push_back(Entry{offset, chain}))) {
    } else {
      // Normally chains arrive in increasing order, so this loop is empty.
      // Retain map semantics for out-of-order inserts as well.
      for (int64_t i = entries_.count() - 1; i > position; --i) {
        entries_.at(i) = entries_.at(i - 1);
      }
      entries_.at(position) = Entry{offset, chain};
    }
    return ret;
  }

  int find(int64_t offset, ObIKTokenChain *&chain) const
  {
    const int64_t position = lower_bound(offset);
    int ret = common::OB_HASH_NOT_EXIST;
    if (position < entries_.count() && entries_.at(position).offset_ == offset) {
      chain = entries_.at(position).chain_;
      ret = common::OB_SUCCESS;
    }
    return ret;
  }

  void reset() { entries_.reset(); }
  int64_t size() const { return entries_.count(); }

private:
  struct Entry {
    int64_t offset_;
    ObIKTokenChain *chain_;
    TO_STRING_KV(K_(offset), KP_(chain));
  };

  int64_t lower_bound(int64_t offset) const
  {
    int64_t low = 0, high = entries_.count();
    while (low < high) {
      const int64_t mid = low + (high - low) / 2;
      if (entries_.at(mid).offset_ < offset) low = mid + 1;
      else high = mid;
    }
    return low;
  }

  common::ObSEArray<Entry, 16, common::ObWrapperAllocator> entries_;
};
} // namespace storage
} // namespace oceanbase
