/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include "sql/engine/expr/ob_tokenize_result_cache.h"

using namespace oceanbase::common;
using oceanbase::sql::TokenizeResultCache;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (false)

void check_cache()
{
  TokenizeResultCache cache;
  const auto name = ObString::make_string("beng.1");
  const auto props = ObString::make_string("{}");
  const auto text = ObString::make_string("Alpha ALPHA");
  ObString result;
  auto get = [&](const ObString &n, const ObString &p, const ObString &t,
                 ObCollationType coll = CS_TYPE_UTF8MB4_BIN, int mode = 0) {
    return cache.get(n, p, t, coll, mode, result);
  };
  CHECK(!get(name, props, text));
  std::string transient("\x01\x00\xff\x02", 4);
  std::string transient_name(name.ptr(), name.length());
  std::string transient_props(props.ptr(), props.length());
  std::string transient_text(text.ptr(), text.length());
  cache.put(ObString(transient_name.size(), transient_name.data()),
            ObString(transient_props.size(), transient_props.data()),
            ObString(transient_text.size(), transient_text.data()), CS_TYPE_UTF8MB4_BIN, 0,
            ObString(transient.size(), transient.data()));
  transient.assign(100, '!');
  transient_name.assign(100, '!');
  transient_props.assign(100, '!');
  transient_text.assign(100, '!');
  CHECK(get(name, props, text));
  CHECK(result.length() == 4 && MEMCMP(result.ptr(), "\x01\x00\xff\x02", 4) == 0);
  CHECK(!get(ObString::make_string("beng.2"), props, text));
  CHECK(!get(name, ObString::make_string("[]"), text));
  CHECK(!get(name, props, ObString::make_string("alpha ALPHA")));
  CHECK(!get(name, props, text, CS_TYPE_UTF8MB4_GENERAL_CI));
  CHECK(!get(name, props, text, CS_TYPE_UTF8MB4_BIN, 1));
  for (const auto limit : {TokenizeResultCache::MAX_INPUT_BYTES, TokenizeResultCache::MAX_RESULT_BYTES}) {
    std::string boundary(limit, 'a');
    ObString value(boundary.size(), boundary.data());
    cache.put(name, props, limit == TokenizeResultCache::MAX_INPUT_BYTES ? value : text,
              CS_TYPE_UTF8MB4_BIN, 0, limit == TokenizeResultCache::MAX_RESULT_BYTES ? value : props);
    CHECK(get(name, props, limit == TokenizeResultCache::MAX_INPUT_BYTES ? value : text));
    boundary.push_back('b');
    value.assign_ptr(boundary.data(), static_cast<int64_t>(boundary.size()));
    cache.put(name, props, limit == TokenizeResultCache::MAX_INPUT_BYTES ? value : text,
              CS_TYPE_UTF8MB4_BIN, 0, limit == TokenizeResultCache::MAX_RESULT_BYTES ? value : props);
    CHECK(!get(name, props, text));
  }
  cache.put(ObString(), ObString(), ObString(), CS_TYPE_UTF8MB4_BIN, 0, ObString());
  CHECK(get(ObString(), ObString(), ObString()));
  CHECK(result.empty());
}

int main()
{
  check_cache();
  std::thread a(check_cache), b(check_cache);
  a.join();
  b.join();
  std::puts("Exact TOKENIZE result cache checks passed");
}
