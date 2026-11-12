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

#include "object/ob_object.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "share/rc/ob_server_runtime.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace storage
{

////////////////////////////////////////////////////////////////////////////////
// class ObStopWordChecker
ObStopWordChecker::~ObStopWordChecker()
{
  destroy();
}

int ObStopWordChecker::init()
{
  int ret = OB_SUCCESS;
  
  

  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(stopword_set_.create(DEFAULT_STOPWORD_BUCKET_NUM, "StopWordSet", "StopWordSet"))) {
  } else {
    ObObjMeta stop_meta;
    stop_meta.set_varchar();
    stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI);

    stopword_type_.set_meta(stop_meta);
    const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
    for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
      ObFTWord stopword(STRLEN(ob_stop_word_list[i]), ob_stop_word_list[i], stopword_type_);
      if (OB_FAIL(stopword_set_.set_refactored(stopword))) {
      }
    }

    if (OB_SUCC(ret)) {
      inited_ = true;
    }
  }
  return ret;
}

void ObStopWordChecker::destroy()
{
  if (inited_) {
    stopword_set_.destroy();
    inited_ = false;
  }
}

int ObStopWordChecker::check_stopword(const ObFTWord &word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  
  
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (CHARSET_UTF8MB4
             == common::ObCharset::charset_type_by_coll(word.get_collation_type())) {
    // Same-charset fast path: with equal charsets charset_convert() aliases the
    // input bytes (no copy), so hashing the raw bytes under the stopword
    // collation yields exactly the same hash and comparison result as the
    // converted word. This keeps the per-token check allocation free while
    // preserving the case-insensitive semantics of the GENERAL_CI lookup.
    ObFTWord converted(word.get_word().get_string().length(),
                       word.get_word().get_string().ptr(),
                       stopword_type_);
    ret = stopword_set_.exist_refactored(converted);
    if (OB_HASH_NOT_EXIST == ret) {
      is_stopword = false;
      ret = OB_SUCCESS;
    } else if (OB_HASH_EXIST == ret) {
      is_stopword = true;
      ret = OB_SUCCESS;
    } else if (OB_SUCC(ret)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
    }
  } else {
    common::ObArenaAllocator allocator(lib::ObMemAttr("ChkStopWord"));
    common::ObString cmp_str;
    // do nothing set out with in if type is the same.
    if (OB_FAIL(common::ObCharset::charset_convert(
                                       allocator,
                                       word.get_word().get_string(),
                                       word.get_collation_type(),
                                       stopword_type_.get_collation_type(),
                                       cmp_str))) {
    } else {
      ObFTWord converted(cmp_str.length(), cmp_str.ptr(), stopword_type_);
      ret = stopword_set_.exist_refactored(converted);
      if (OB_HASH_NOT_EXIST == ret) {
        is_stopword = false;
        ret = OB_SUCCESS;
      } else if (OB_HASH_EXIST == ret) {
        is_stopword = true;
        ret = OB_SUCCESS;
      } else if (OB_SUCC(ret)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// class ObAddWord


namespace
{
// Existing-key callback: folds this occurrence's count into the existing map
// entry. When the stored key already carries this occurrence's bytes -- the
// common case, a word repeated verbatim -- the count is updated in place and
// the stored key is left alone (no copy, no re-point). When the bytes differ
// even though the keys compare equal under the collation (e.g. two different
// emoji under utf8mb4_general_ci) the pair has to be replaced afterwards so
// that the key bytes follow the last occurrence, which is what the upstream
// set_refactored(word, count, 1) overwrite did. The replacement cannot run
// inside this callback because the callback holds the bucket write lock.
// The callback receives the stored (key, value) pair, so it can compare
// the retained bytes without a second map lookup.
class WordCountUpdater
{
public:
  WordCountUpdater(const ObFTWord &word, const int64_t word_freq)
    : word_(word),
      word_freq_(word_freq),
      need_replace_(false),
      updated_count_(0)
  {}
  int operator()(common::hash::HashMapPair<ObFTWord, int64_t> &pair)
  {
    const ObString stored = pair.first.get_word().get_string();
    const ObString incoming = word_.get_word().get_string();
    if (stored.compare_equal(incoming)) {
      pair.second += word_freq_;
    } else {
      need_replace_ = true;
      updated_count_ = pair.second + word_freq_;
    }
    return OB_SUCCESS;
  }
  bool need_replace() const { return need_replace_; }
  int64_t get_updated_count() const { return updated_count_; }
private:
  // the key that would be stored if this occurrence has to replace the entry
  const ObFTWord &word_;
  int64_t word_freq_;
  bool need_replace_;
  int64_t updated_count_;
};
// Called only on a new map entry: short normalized tokens live on the stack,
// and analyzer tokens may be overwritten by the next get_next_token().
class WordKeyMaterializer
{
public:
  WordKeyMaterializer(ObIAllocator &allocator, const ObObjMeta &meta, bool owns_bytes)
    : allocator_(allocator), meta_(meta), owns_bytes_(owns_bytes) {}
  int operator()(common::hash::HashMapPair<ObFTWord, int64_t> &pair)
  {
    int ret = OB_SUCCESS;
    if (owns_bytes_) return ret; // long-token fallback already made a stable copy
    const ObString word = pair.first.get_word().get_string();
    char *bytes = static_cast<char *>(allocator_.alloc(word.length()));
    if (OB_ISNULL(bytes)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(bytes, word.ptr(), word.length());
      pair.first = ObFTWord(word.length(), bytes, meta_);
    }
    return ret;
  }
private:
  ObIAllocator &allocator_;
  const ObObjMeta &meta_;
  bool owns_bytes_;
};
}  // anonymous namespace

template <class MapType>
ObAddWordT<MapType>::ObAddWordT(
    const ObFTParserProperty &property,
    const ObObjMeta &meta,
    const ObAddWordFlag &flag,
    common::ObIAllocator &allocator,
    MapType &word_map)
  : word_meta_(meta),
    allocator_(allocator),
    word_map_(word_map),
    min_max_word_cnt_(0),
    non_stopword_cnt_(0),
    stopword_cnt_(0),
    min_token_size_(property.min_token_size_),
    max_token_size_(property.max_token_size_),
    flag_(flag)
{
}

template <class MapType>
int ObAddWordT<MapType>::process_word(
    const char *word,
    const int64_t word_len,
    const int64_t char_cnt,
    const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  bool is_stopword = false;
  ObFTWord src_word(word_len, word, word_meta_);
  ObFTWord dst_word;
  bool owns_bytes = false;
  char case_buffer[256]; // valid through stop-word checking and map insertion
  if (OB_ISNULL(word) || OB_UNLIKELY(0 >= word_len || 0 >= char_cnt || 0 >= word_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_min_max_word(char_cnt)) {
    ++min_max_word_cnt_;
  } else if (OB_FAIL(casedown_word(src_word, dst_word, case_buffer, sizeof(case_buffer), owns_bytes))) {
  } else if (OB_FAIL(check_stopword(dst_word, is_stopword))) {
  } else if (OB_UNLIKELY(is_stopword)) {
    ++stopword_cnt_;
  } else if (OB_FAIL(groupby_word(dst_word, word_freq, owns_bytes))) {
  } else {
    non_stopword_cnt_ += word_freq;
  }
  return ret;
}

template <class MapType>
bool ObAddWordT<MapType>::is_min_max_word(const int64_t c_len) const
{
  return flag_.min_max_word() && (c_len < min_token_size_ || c_len > max_token_size_);
}

template <class MapType>
int ObAddWordT<MapType>::casedown_word(
    const ObFTWord &src, ObFTWord &dst, char *buffer, int64_t capacity, bool &owns_bytes)
{
  int ret = OB_SUCCESS;
  owns_bytes = false;
  if (OB_UNLIKELY(src.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.casedown()) {
    const ObString input = src.get_word().get_string();
    const ObCharsetInfo *cs = ObCharset::get_charset(word_meta_.get_collation_type());
    if (OB_ISNULL(cs) || OB_ISNULL(cs->cset) || cs->casedn_multiply <= 0) {
      ret = OB_ERR_UNEXPECTED;
    } else if (input.length() <= capacity / cs->casedn_multiply) {
      const size_t length = cs->cset->casedn(cs, const_cast<char *>(input.ptr()),
                                           input.length(), buffer, capacity);
      dst = ObFTWord(length, buffer, word_meta_);
    } else {
      ObString dst_str;
      if (OB_FAIL(ObCharset::tolower(word_meta_.get_collation_type(), input, dst_str, allocator_))) {
      } else {
        dst = ObFTWord(dst_str.length(), dst_str.ptr(), word_meta_);
        owns_bytes = true;
      }
    }
  } else {
    dst = src;
  }
  return ret;
}

template <class MapType>
int ObAddWordT<MapType>::check_stopword(const ObFTWord &ft_word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ft_word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.stopword()) {
    ObStopWordChecker *stop_word_checker = ObFTParseData::instance().stop_word_checker();
    if (OB_ISNULL(stop_word_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(stop_word_checker->check_stopword(ft_word, is_stopword))) {
    }
  }
  return ret;
}

template <class MapType>
int ObAddWordT<MapType>::groupby_word(
    const ObFTWord &word, const int64_t word_freq, bool owns_bytes)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(word.empty() || word_freq <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!flag_.groupby_word()) {
    ObFTWord stable_word;
    if (owns_bytes) {
      stable_word = word;
    } else if (OB_FAIL(dup_word(word, stable_word))) {
    }
    if (OB_SUCC(ret) && OB_FAIL(word_map_.set_refactored(stable_word, 1/*word count*/))) {
    }
  } else {
    // Hash once; only a new or byte-distinct collation-equal key needs owned
    // bytes. Verbatim repeats update the count without any allocation.
    WordCountUpdater updater(word, word_freq);
    WordKeyMaterializer materializer(allocator_, word_meta_, owns_bytes);
    if (OB_FAIL(word_map_.set_or_update_with_prepare(word, 1, updater, materializer))) {
    } else if (updater.need_replace()) {
      ObFTWord stable_word;
      if (owns_bytes) {
        stable_word = word;
      } else if (OB_FAIL(dup_word(word, stable_word))) {
      }
      if (OB_SUCC(ret) && OB_FAIL(word_map_.set_refactored(stable_word,
                                                          updater.get_updated_count(), 1))) {
      }
    }
  }

  return ret;
}

template <class MapType>
int ObAddWordT<MapType>::dup_word(const ObFTWord &src, ObFTWord &dst)
{
  int ret = OB_SUCCESS;
  const ObString src_str = src.get_word().get_string();
  char *buf = nullptr;
  if (OB_UNLIKELY(0 >= src_str.length())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator_.alloc(src_str.length())))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, src_str.ptr(), src_str.length());
    dst = ObFTWord(src_str.length(), buf, word_meta_);
  }
  return ret;
}

template class ObAddWordT<ObFTWordMap>;
template class ObAddWordT<ObFTWordMapLocked>;

} // end namespace storage
} // end namespace oceanbase
