#pragma once
#include <string>
#include <vector>

#include "value.hpp"

namespace mvdb {

struct Cond {
  CmpOp op = CmpOp::Eq;
  std::string col;
  Value lit;
};

struct SelItem {
  enum class K { Star, Col, Dist } k = K::Col;
  std::string name;   // column name, or alias for Dist
  std::string alias;  // optional alias for Dist
  std::vector<float> q;
};

struct OrderKey {
  std::string ref;  // distance alias or column name
  bool desc = false;
};

struct Stmt {
  enum class K {
    CreateTable,
    CreateVectorIndex,
    Insert,
    Select,
    Update,
    Delete,
    Begin,
    Commit,
    Rollback
  } k = K::Select;
  std::string table;
  std::string name;  // index name
  std::string col;   // index column
  std::vector<ColDef> cols;
  std::vector<std::string> insertCols;  // optional column list
  std::vector<std::vector<Value>> insertRows;
  std::vector<SelItem> items;
  std::vector<Cond> where;
  std::vector<OrderKey> order;
  long long limit = -1;
  std::vector<std::pair<std::string, Value>> sets;
};

struct ParseResult {
  bool ok = false;
  std::string code;  // error code when !ok
  std::string msg;
  Stmt stmt;
};

ParseResult parseSql(const std::string& sql);

}  // namespace mvdb
