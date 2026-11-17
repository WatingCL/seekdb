/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_ik_ft_parser.h"
#include "storage/fts/ob_beng_ft_parser.h"
#include "storage/fts/ob_fts_parser_property.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_dict_iterator.h"
#include "lib/container/ob_vector.h"
#define private public
#include "storage/fts/dict/ob_ft_range_dict.h"
#undef private

using namespace oceanbase::common;
using namespace oceanbase::storage;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (false)

class CountingAllocator final : public ObWrapperAllocator
{
public:
  explicit CountingAllocator(ObIAllocator &arena) : ObWrapperAllocator(arena) {}
  void *alloc(int64_t size) override { ++calls_; return fail_ ? nullptr : ObWrapperAllocator::alloc(size); }
  int64_t calls_ = 0;
  bool fail_ = false;
};

template <class Map>
void check_normalization(ObCollationType collation, bool casedown)
{
  ObArenaAllocator arena;
  CountingAllocator allocator(arena);
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(collation);
  ObFTParserProperty property;
  ObAddWordFlag flags;
  flags.set_groupby_word();
  if (casedown) flags.set_casedown();
  Map actual, expected;
  CHECK(OB_SUCCESS == actual.create(37, "FTSTest"));
  CHECK(OB_SUCCESS == expected.create(37, "FTSTest"));
  ObAddWordT<Map> add(property, meta, flags, allocator, actual);
  const std::vector<std::string> words = {
      "Alpha", "ALPHA", "alpha", "中文", "中文", "ＢＡＳＥ", "ｂａｓｅ",
      "😀", "😁", "😀", "CafÉ", "CAFé", std::string(300, 'A'), std::string(300, 'a')};
  uint64_t checksum = 0;
  for (const auto &input : words) {
    // Simulate an analyzer which overwrites its token buffer after every call.
    std::string transient = input;
    CHECK(OB_SUCCESS == add.process_word(transient.data(), transient.size(), 3, 1));
    transient.assign(transient.size(), '!');
    ObString normalized(input.size(), input.data());
    if (casedown) {
      CHECK(OB_SUCCESS == ObCharset::tolower(collation, normalized, normalized, arena));
    }
    char *stable = static_cast<char *>(arena.alloc(normalized.length()));
    CHECK(stable != nullptr);
    MEMCPY(stable, normalized.ptr(), normalized.length());
    ObFTWord key(normalized.length(), stable, meta);
    int64_t count = 0;
    const int ret = expected.get_refactored(key, count);
    CHECK(ret == OB_SUCCESS || ret == OB_HASH_NOT_EXIST);
    CHECK(OB_SUCCESS == expected.set_refactored(key, count + 1, 1));
  }
  CHECK(actual.size() == expected.size());
  auto a = actual.begin();
  for (auto e = expected.begin(); e != expected.end(); ++e, ++a) {
    CHECK(a != actual.end());
    CHECK(a->first.get_word().get_string().compare_equal(e->first.get_word().get_string()));
    CHECK(a->second == e->second);
    const auto word = a->first.get_word().get_string();
    for (int64_t i = 0; i < word.length(); ++i) checksum = checksum * 131 + static_cast<unsigned char>(word.ptr()[i]);
    checksum = checksum * 131 + a->second;
  }
  std::printf("normalization_%d_%d_checksum: %llu\n", collation, casedown,
              static_cast<unsigned long long>(checksum));
}

void check_allocation_and_failure()
{
  ObArenaAllocator arena;
  CountingAllocator allocator(arena);
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ObFTParserProperty property;
  ObAddWordFlag flags;
  flags.set_groupby_word();
  flags.set_casedown();
  ObFTWordMap map;
  CHECK(OB_SUCCESS == map.create(37, "FTSTest"));
  ObAddWord add(property, meta, flags, allocator, map);
  CHECK(OB_SUCCESS == add.process_word("ALPHA", 5, 5, 1));
  const int64_t after_first = allocator.calls_;
  for (int i = 0; i < 1000; ++i) CHECK(OB_SUCCESS == add.process_word("Alpha", 5, 5, 1));
  std::printf("repeated_token_allocations: %lld\n", allocator.calls_ - after_first);
#ifndef FTS_BASELINE
  CHECK(allocator.calls_ == after_first);
  allocator.fail_ = true;
  CHECK(OB_SUCCESS == add.process_word("alpha", 5, 5, 1));
  CHECK(OB_ALLOCATE_MEMORY_FAILED == add.process_word("other", 5, 5, 1));
  CHECK(map.size() == 1);
  int64_t count = 0;
  CHECK(OB_SUCCESS == map.get_refactored(ObFTWord(5, "alpha", meta), count));
  CHECK(count == 1002);
  allocator.fail_ = false;
  ObFTWordMap collisions;
  CHECK(OB_SUCCESS == collisions.create(37, "FTSTest"));
  ObAddWord collision_add(property, meta, flags, allocator, collisions);
  CHECK(OB_SUCCESS == collision_add.process_word("😀", 4, 1, 1));
  allocator.fail_ = true;
  CHECK(OB_ALLOCATE_MEMORY_FAILED == collision_add.process_word("😁", 4, 1, 1));
  CHECK(OB_SUCCESS == collisions.get_refactored(ObFTWord(4, "😀", meta), count));
  CHECK(count == 1);
  CHECK(collisions.begin()->first.get_word().get_string().compare_equal(ObString("😀")));
  allocator.fail_ = false;
  CHECK(OB_SUCCESS == collision_add.process_word("😁", 4, 1, 1));
  CHECK(OB_SUCCESS == collisions.get_refactored(ObFTWord(4, "😀", meta), count));
  CHECK(count == 2);
  CHECK(collisions.begin()->first.get_word().get_string().compare_equal(ObString("😁")));
  const std::string long_word(300, 'A');
  const int64_t before_long = allocator.calls_;
  CHECK(OB_SUCCESS == add.process_word(long_word.data(), long_word.size(), 300, 1));
  CHECK(allocator.calls_ == before_long + 1); // reuse the owned fallback result


#endif
}

void check_prepare_map_operation()
{
  oceanbase::common::hash::ObHashMap<int64_t, int64_t> map;
  int preparations = 0, updates = 0;
  bool fail = false;
  auto prepare = [&](auto &pair) {
    ++preparations;
    pair.second = 42;
    return fail ? OB_ALLOCATE_MEMORY_FAILED : OB_SUCCESS;
  };
  auto update = [&](auto &pair) { ++updates; ++pair.second; };
  CHECK(OB_NOT_INIT == map.set_or_update_with_prepare(1, 0, update, prepare));
  CHECK(OB_SUCCESS == map.create(37, "FTSTest"));
  fail = true;
  CHECK(OB_ALLOCATE_MEMORY_FAILED == map.set_or_update_with_prepare(1, 0, update, prepare));
  CHECK(map.size() == 0);
  fail = false;
  CHECK(OB_SUCCESS == map.set_or_update_with_prepare(1, 0, update, prepare));
  fail = true; // Existing keys must not run prepare, even if it would fail.
  CHECK(OB_SUCCESS == map.set_or_update_with_prepare(1, 0, update, prepare));
  CHECK(preparations == 2 && updates == 1 && map.size() == 1);
  int64_t value = 0;
  CHECK(OB_SUCCESS == map.get_refactored(1, value));
  CHECK(value == 43);
}

void check_concurrent_prepare()
{
  oceanbase::common::hash::ObHashMap<int64_t, int64_t> map;
  CHECK(OB_SUCCESS == map.create(37, "FTSTest"));
  std::atomic<int> preparations{0};
  auto prepare = [&](auto &) { ++preparations; return OB_SUCCESS; };
  auto update = [](auto &pair) { ++pair.second; };
  std::vector<std::thread> workers;
  for (int thread = 0; thread < 4; ++thread) {
    workers.emplace_back([&]() {
      for (int i = 0; i < 1000; ++i) {
        CHECK(OB_SUCCESS == map.set_or_update_with_prepare(i % 16, 1, update, prepare));
      }
    });
  }
  for (auto &worker : workers) worker.join();
  CHECK(preparations == 16 && map.size() == 16);
  int64_t sum = 0;
  for (const auto &entry : map) sum += entry.second;
  CHECK(sum == 4000);
}

void check_ranges()
{
  ObArenaAllocator arena;
  ObFTDictDesc desc(ObString("test"), ObFTDictType::DICT_IK_MAIN,
                    CHARSET_UTF8MB4, CS_TYPE_UTF8MB4_BIN);
  ObFTRangeDict dict(arena, nullptr, desc);
  // Inclusive overlapping ranges and a gap exercise first-match semantics.
  const char *starts[] = {"a", "c", "k", "中"};
  const char *ends[] = {"d", "f", "m", "文"};
  char sentinels[4];
  for (int i = 0; i < 4; ++i) {
    ObFTRangeDict::ObFTRange range;
    CHECK(OB_SUCCESS == range.start_.set_word(starts[i], std::strlen(starts[i])));
    CHECK(OB_SUCCESS == range.end_.set_word(ends[i], std::strlen(ends[i])));
    range.dict_ = reinterpret_cast<ObIFTDict *>(&sentinels[i]);
    CHECK(OB_SUCCESS == dict.range_dicts_.push_back(range));
  }
  std::vector<std::string> probes = {"", "a", "c", "d", "f", "g", "k", "m", "n", "中", "文", "😀"};
  for (int c = 1; c < 128; ++c) probes.emplace_back(1, c);
  for (int pass = 0; pass < 3; ++pass) {
#ifndef FTS_BASELINE
    dict.ranges_ordered_ = pass != 2; // also exercise the unordered fallback
#endif
    MEMSET(dict.range_memo_, 0, sizeof(dict.range_memo_));
    for (const auto &probe : probes) {
      ObIFTDict *expected = nullptr;
      const ObString key(probe.size(), probe.data());
      for (const auto &range : dict.range_dicts_) {
        if (ObCharset::strcmp(CS_TYPE_UTF8MB4_BIN, range.start_.get_word(), key) <= 0
            && ObCharset::strcmp(CS_TYPE_UTF8MB4_BIN, range.end_.get_word(), key) >= 0) {
          expected = range.dict_;
          break;
        }
      }
      for (int repeat = 0; repeat < 2; ++repeat) {
        ObIFTDict *found = nullptr;
        CHECK((expected ? OB_SUCCESS : OB_ENTRY_NOT_EXIST) == dict.find_first_char_range(key, found));
        CHECK(expected == found);
      }
    }
  }
}

void check_bounded_parser_scratch()
{
#ifndef FTS_BASELINE
  ObArenaAllocator scratch, objects;
  ObIKFTParser ik(scratch, objects, nullptr);
  ObBEngFTParser beng(scratch, objects);
  ik.set_pooled();
  beng.set_pooled();
  auto check = [&](auto &parser) {
    for (int i = 0; i < 1000; ++i) {
      CHECK(scratch.alloc(1024) != nullptr);
      parser.release_to_pool();
      CHECK(scratch.used() == 0);
      CHECK(scratch.total() > 0 && scratch.total() <= 64 * 1024);
    }
    CHECK(scratch.alloc(256 * 1024) != nullptr);
    parser.release_to_pool();
    CHECK(scratch.total() == 0);
    CHECK(scratch.alloc(1024) != nullptr);
    parser.release_to_pool();
    CHECK(scratch.total() > 0 && scratch.total() <= 64 * 1024);
  };
  check(ik);
  check(beng);
#endif
}

void check_parser_setup_cache()
{
  const std::vector<std::pair<std::string, std::string>> configs = {
    {"beng.1", ""}, {"space.1", ""}, {"ngram.1", ""}, {"ngram2.1", ""},
    {"ik.1", "{\"ik_mode\":\"smart\"}"}, {"ik.1", "{\"ik_mode\":\"max_word\"}"},
    {"beng.1", "{\"min_token_size\":3,\"max_token_size\":10}"},
    {"ngram.1", "{\"ngram_token_size\":3}"},
    {"ngram.1", "{ \"ngram_token_size\" : 3 }"},
    {"ik.1", "{\"ik_mode\":\"SMART\"}"},
    {"beng.2", ""}
  };
  auto run = [&]() {
    for (const auto &config : configs) {
      for (int repeat = 0; repeat < 3; ++repeat) {
        ObArenaAllocator arena;
        CountingAllocator allocator(arena);
        ObFTParseHelper helper;
        // The cache must own the key, even after the original caller dies.
        std::string name = config.first, props = config.second;
        CHECK(OB_SUCCESS == helper.init(&allocator, ObString(name.size(), name.data()),
                                       ObString(props.size(), props.data())));
        name.assign(name.size(), '!');
        props.assign(props.size(), '!');
        bool same = false;
        CHECK(OB_SUCCESS == helper.check_is_the_same(
            ObString(config.first.size(), config.first.data()),
            ObString(config.second.size(), config.second.data()), same));
        CHECK(same);
        // Independently parse both configurations using the original comparison
        // algorithm; include different parsers and semantically changed options.
        ObFTParser expected_parser;
        ObFTParserProperty expected_property;
        CHECK(OB_SUCCESS == expected_parser.parse_from_str(config.first.data(), config.first.size()));
        CHECK(OB_SUCCESS == expected_property.parse_for_parser_helper(expected_parser,
            ObString(config.second.size(), config.second.data())));
        for (const auto &other : configs) {
          ObFTParser other_parser;
          ObFTParserProperty other_property;
          CHECK(OB_SUCCESS == other_parser.parse_from_str(other.first.data(), other.first.size()));
          CHECK(OB_SUCCESS == other_property.parse_for_parser_helper(other_parser,
              ObString(other.second.size(), other.second.data())));
          same = true;
          CHECK(OB_SUCCESS == helper.check_is_the_same(
              ObString(other.first.size(), other.first.data()),
              ObString(other.second.size(), other.second.data()), same));
          CHECK(same == (expected_parser == other_parser && expected_property.is_equal(other_property)));
        }
        CHECK(OB_SUCCESS != helper.check_is_the_same(ObString::make_string("ik.1"),
              ObString::make_string("{\"ik_mode\":\"invalid\"}"), same));
        CHECK(OB_SUCCESS != helper.check_is_the_same(ObString(), ObString(), same));
        CHECK(OB_INIT_TWICE == helper.init(&allocator, ObString::make_string("beng.1"), ObString()));
        ObFTWordMap words;
        CHECK(OB_SUCCESS == words.create(2, "FTSTest"));
        ObIJsonBase *json = nullptr;
        const auto before = allocator.calls_;
        CHECK(OB_SUCCESS == helper.make_detail_json(words, 0, json));
        CHECK(json != nullptr && allocator.calls_ > before); // current allocator, including on hits
        helper.reset();
        CHECK(OB_SUCCESS == helper.init(&allocator,
            ObString(config.first.size(), config.first.data()),
            ObString(config.second.size(), config.second.data())));
      }
    }
    ObArenaAllocator arena;
    ObFTParseHelper helper;
    CHECK(OB_INVALID_ARGUMENT == helper.init(nullptr, ObString::make_string("beng.1"), ObString()));
    CHECK(OB_INVALID_ARGUMENT == helper.init(&arena, ObString(), ObString()));
    CHECK(OB_SUCCESS != helper.init(&arena, ObString::make_string("ik.1"),
                                   ObString::make_string("{\"ik_mode\":\"invalid\"}")));
    CHECK(OB_SUCCESS == helper.init(&arena, ObString::make_string("beng.1"), ObString()));
    helper.reset();
    ObFTParseHelper large_helper;
    const std::string large = std::string(2048, ' ') + "{\"ngram_token_size\":4}";
    CHECK(OB_SUCCESS == large_helper.init(&arena, ObString::make_string("ngram.1"),
                                   ObString(large.size(), large.data())));
    bool same = false;
    CHECK(OB_SUCCESS == large_helper.check_is_the_same(ObString::make_string("ngram.1"),
          ObString::make_string("{\"ngram_token_size\":4}"), same));
    CHECK(same);
  };
  run();
  std::thread first(run), second(run);
  first.join();
  second.join();
  std::puts("Parser setup cache regression checks passed");
}

void benchmark_parser_setup()
{
  ObArenaAllocator arena;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 100000; ++i) {
    ObFTParseHelper helper;
    CHECK(OB_SUCCESS == helper.init(&arena, ObString::make_string("ik.1"),
                                   ObString::make_string("{\"ik_mode\":\"smart\"}")));
  }
  std::printf("parser_setup_100000_calls_sec: %.6f\n",
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  ObFTParseHelper helper;
  CHECK(OB_SUCCESS == helper.init(&arena, ObString::make_string("ik.1"),
                                 ObString::make_string("{\"ik_mode\":\"smart\"}")));
  const auto compare_start = std::chrono::steady_clock::now();
  for (int i = 0; i < 100000; ++i) {
    bool same = false;
    CHECK(OB_SUCCESS == helper.check_is_the_same(ObString::make_string("ik.1"),
          ObString::make_string("{\"ik_mode\":\"smart\"}"), same));
    CHECK(same);
  }
  std::printf("parser_compare_100000_calls_sec: %.6f\n",
      std::chrono::duration<double>(std::chrono::steady_clock::now() - compare_start).count());
}

void benchmark_normalization()
{
  for (bool repeated : {false, true}) {
    const auto start = std::chrono::steady_clock::now();
    for (int document = 0; document < 100000; ++document) {
      ObArenaAllocator arena;
      ObObjMeta meta;
      meta.set_varchar();
      meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
      ObFTParserProperty property;
      ObAddWordFlag flags;
      flags.set_casedown();
      flags.set_groupby_word();
      ObFTWordMap map;
      CHECK(OB_SUCCESS == map.create(37, "FTSTest"));
      ObAddWord add(property, meta, flags, arena, map);
      for (int i = 0; i < 20; ++i) {
        char token[] = "tokenA";
        token[5] += repeated ? i % 3 : i;
        CHECK(OB_SUCCESS == add.process_word(token, 6, 6, 1));
      }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("normalize_%s_100000_documents_sec: %.6f\n", repeated ? "repeated" : "unique", seconds);
  }
}

int main(int argc, char **argv)
{
  ObCharset::init_charset();
  for (auto coll : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI}) {
    for (bool casedown : {false, true}) {
      check_normalization<ObFTWordMap>(coll, casedown);
      check_normalization<ObFTWordMapLocked>(coll, casedown);
    }
  }
  if (argc > 1 && std::strcmp(argv[1], "--benchmark") == 0) {
    benchmark_normalization();
    benchmark_parser_setup();
  }
  check_parser_setup_cache();
  check_allocation_and_failure();
  check_bounded_parser_scratch();
  check_prepare_map_operation();
  check_concurrent_prepare();
  check_ranges();
  std::puts("Reference optimization regression checks passed");
}
