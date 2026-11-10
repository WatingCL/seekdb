#pragma once
#include <map>
#include <string>
#include <vector>

#include "json.hpp"
#include "parser.hpp"
#include "storage.hpp"

namespace mvdb {

enum class WhereVerdict { Match, NoMatch, Error };

struct ExecResult {
  bool ok = false;
  std::string code;
  std::string msg;
  bool plain = false;  // success without columns/affected_rows
  bool resultSet = false;  // SELECT: always report columns and rows, even when empty
  long long affected = 0;
  std::vector<std::string> columns;
  std::vector<std::vector<Json>> rows;
};

class Engine {
 public:
  explicit Engine(std::string dataDir);

  ExecResult exec(const Stmt& st, const std::string& searchMode);
  // Rolls back any open transaction and syncs the logs before a clean exit.
  bool shutdown(std::string& err);

 private:
  struct Table {
    std::string name;
    std::string path;  // row log
    std::vector<ColDef> schema;
    int pkIdx = -1;
    int vecIdx = -1;
    bool hasVecIndex = false;
    std::map<long long, RowState> index;  // rows live in the log, keyed by offset
    AppendLog log;
  };

  // The index entry a staged write replaced, so a rollback can put it back.
  struct UndoOp {
    std::string table;
    long long pk = 0;
    bool had = false;
    RowState prev;
  };
  // Log positions at a savepoint: rolling back trims every record appended after
  // them, which is what makes an aborted transaction id unusable again.
  struct Mark {
    size_t undo = 0;
    std::map<std::string, uint64_t> logPos;
  };

  static ExecResult makeError(const std::string& code, const std::string& msg);
  static bool condEval(const Cond& c, const std::vector<Value>& row, const Table& t, bool& matched,
                       std::string& code, std::string& msg);
  static WhereVerdict evalWhere(const std::vector<Cond>& where, const std::vector<Value>& row, const Table& t,
                                ExecResult& err);
  static double l2(const std::vector<float>& a, const std::vector<float>& b);

  ExecResult doCreateTable(const Stmt& st);
  ExecResult doCreateVectorIndex(const Stmt& st);
  ExecResult doInsert(const Stmt& st);
  ExecResult doSelect(const Stmt& st, const std::string& mode);
  ExecResult doUpdate(const Stmt& st);
  ExecResult doDelete(const Stmt& st);
  ExecResult doBegin();
  ExecResult doCommit();
  ExecResult doRollback();

  void recover();
  bool writeCatalog(std::string& err);
  Mark mark() const;
  // Opens a scan over t's rows as of now; records appended after this point stay
  // invisible to it, so a mutating scan cannot chase its own writes.
  bool openReader(const Table& t, LogReader& rd, ExecResult& err);
  // Appends the record and repoints the index; the write is not durable until
  // the transaction commits.
  void stageRow(Table& t, long long pk, const std::vector<Value>& row);
  void stageDelete(Table& t, long long pk);
  void rollbackTo(const Mark& m);
  // Syncs the logs touched since m and records the commit point.
  bool commitFrom(uint64_t xid, size_t undoFrom, std::string& err);

  std::string dataDir_;
  std::map<std::string, Table> tables_;
  AppendLog txnLog_;

  bool inTx_ = false;
  uint64_t txXid_ = 0;
  uint64_t stmtXid_ = 0;
  uint64_t nextXid_ = 1;
  Mark txMark_;
  Mark stmtMark_;
  std::vector<UndoOp> undo_;
};

}  // namespace mvdb
