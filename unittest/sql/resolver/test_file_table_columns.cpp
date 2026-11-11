/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/resolver/expr/ob_raw_expr.h"
#include "sql/engine/basic/file_sql/file_reader.h"

namespace oceanbase
{
namespace sql
{
TEST(FileTableColumns, ResolvesBoundSchemaWithoutPlCollection)
{
  common::ObArenaAllocator allocator;
  ObResolverParams params;
  params.allocator_ = &allocator;
  // Deliberately no session, schema checker or PL package: a native file table
  // resolves its names entirely from the schema already bound into the plan.
  file_sql::Reader reader;
  reader.columns.push_back({"id", file_sql::Type::BIGINT, false});
  reader.columns.push_back({"Label", file_sql::Type::VARCHAR, true});
  reader.columns.push_back({"id__2", file_sql::Type::BIGINT, false});
  const std::string payload = file_sql::encode(reader);
  common::ObObj value;
  value.set_varchar(common::ObString(payload.size(), payload.data()));
  ObConstRawExpr expr(value, T_VARCHAR);
  TableItem table;
  table.type_ = TableItem::FUNCTION_TABLE;
  table.function_table_expr_ = &expr;
  common::ObSEArray<common::ObString, 4> names;
  ASSERT_EQ(common::OB_SUCCESS,
      ObResolverUtils::get_all_function_table_column_names(table, params, names));
  ASSERT_EQ(3, names.count());
  EXPECT_EQ(common::ObString("Label"), names.at(1));
  EXPECT_EQ(common::OB_SUCCESS,
      ObResolverUtils::check_function_table_column_exist(table, params, common::ObString("label")));
  EXPECT_EQ(common::OB_SUCCESS,
      ObResolverUtils::check_function_table_column_exist(table, params, common::ObString("id__2")));
  EXPECT_EQ(common::OB_ERR_BAD_FIELD_ERROR,
      ObResolverUtils::check_function_table_column_exist(table, params, common::ObString("missing")));
}
}
}
