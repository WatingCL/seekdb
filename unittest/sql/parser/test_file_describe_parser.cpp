/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "sql/parser/ob_parser.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"

namespace oceanbase
{
namespace sql
{
TEST(FileDescribeParser, PreservesExplainParameterCountAndFormat)
{
  ASSERT_EQ(common::OB_SUCCESS, common::ObCharset::init_charset());
  const char *queries[] = {
    "EXPLAIN SELECT 1", "EXPLAIN BASIC SELECT 1",
    "DESCRIBE SELECT * FROM read_csv('/tmp/not-opened-by-parser.csv')",
    "DESC SELECT 1", "DESCRIBE docs", "EXPLAIN docs"
  };
  for (int i = 0; i < 6; ++i) {
    common::ObArenaAllocator allocator;
    ObParser parser(allocator, 0);
    ParseResult result;
    ASSERT_EQ(common::OB_SUCCESS, parser.parse(common::ObString(queries[i]), result)) << queries[i];
    ASSERT_NE(nullptr, result.result_tree_);
    const ParseNode *stmt = result.result_tree_->children_[0];
    if (i >= 4) {
      EXPECT_EQ(T_SHOW_COLUMNS, stmt->type_);
      continue;
    }
    ASSERT_EQ(T_EXPLAIN, stmt->type_);
    EXPECT_EQ(0, stmt->value_);
    ASSERT_EQ(5, stmt->num_child_);
    if (i >= 2) {
      ASSERT_NE(nullptr, stmt->children_[0]);
      EXPECT_EQ(T_DEFAULT, stmt->children_[0]->type_);
      EXPECT_EQ(0x46494c45, stmt->children_[0]->value_);
    } else if (i == 1) {
      ASSERT_NE(nullptr, stmt->children_[0]);
      EXPECT_EQ(T_BASIC, stmt->children_[0]->type_);
    } else {
      EXPECT_EQ(nullptr, stmt->children_[0]);
    }
  }
}
}
}
