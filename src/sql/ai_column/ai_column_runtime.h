/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "lib/string/ob_string.h"
#include "sql/parser/parse_node.h"
namespace oceanbase {
namespace share { namespace schema { class ObTableSchema; } }
namespace sql {
class ObIInnerSQLConnection;
struct ObResolverParams;
class ObStmt;
int resolve_ai_column_retry(ObResolverParams &params, const ::ParseNode &tree, ObStmt *&stmt);
class ObSQLSessionInfo;
bool ai_column_is_internal();
class AiColumnInternalGuard
{
public:
  explicit AiColumnInternalGuard(ObSQLSessionInfo *session = nullptr);
  ~AiColumnInternalGuard();
private:
  ObSQLSessionInfo *session_;
public:
  AiColumnInternalGuard(const AiColumnInternalGuard &) = delete;
  AiColumnInternalGuard &operator=(const AiColumnInternalGuard &) = delete;
};
int reconcile_ai_column_table(ObIInnerSQLConnection &connection,
    const share::schema::ObTableSchema &table, const common::ObString &database);
}
}
