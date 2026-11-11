#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace mvdb {

namespace {

const char* kMetaFile = "/meta.json";
const char* kTxnFile = "/txn.log";

std::string joinPath(const std::string& dir, const std::string& name) { return dir + "/" + name; }

bool readWholeFile(const std::string& path, std::string& out) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;
  std::fseek(fp, 0, SEEK_END);
  long n = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  out.resize(n > 0 ? static_cast<size_t>(n) : 0);
  size_t got = out.empty() ? 0 : std::fread(&out[0], 1, out.size(), fp);
  std::fclose(fp);
  return got == out.size();
}

Json valueToJson(const Value& v) {
  switch (v.k) {
    case VK::Int: return Json::integer(v.i);
    case VK::Str: return Json::str(v.s);
    case VK::Vec: {
      Json a = Json::array();
      for (float f : v.v) a.arr.push_back(Json::real(static_cast<double>(f)));
      return a;
    }
    case VK::Dbl: return Json::real(v.d);
  }
  return Json();
}

Json doubleToJson(double d) { return Json::real(d); }

// A record is the current version of its primary key only if the index still
// points at it: updates and deletes repoint or retire the entry.
bool liveVersion(const std::map<long long, RowState>& index, const RecView& v) {
  auto it = index.find(v.pk);
  return it != index.end() && !it->second.deleted && it->second.pos.off == v.payloadOff;
}

bool convertValue(const Value& raw, const ColDef& c, Value& out, std::string& code, std::string& msg) {
  switch (c.type) {
    case ColType::Bigint:
      if (raw.k != VK::Int) {
        code = "type_mismatch";
        msg = "column " + c.name + " expects BIGINT";
        return false;
      }
      out = raw;
      return true;
    case ColType::Varchar:
      if (raw.k != VK::Str) {
        code = "type_mismatch";
        msg = "column " + c.name + " expects VARCHAR";
        return false;
      }
      if (unicodeLen(raw.s) > static_cast<size_t>(c.n)) {
        code = "value_too_long";
        msg = "column " + c.name + " exceeds VARCHAR(" + std::to_string(c.n) + ")";
        return false;
      }
      out = raw;
      return true;
    case ColType::Vector: {
      if (raw.k != VK::Str) {
        code = "type_mismatch";
        msg = "column " + c.name + " expects VECTOR literal string";
        return false;
      }
      std::vector<float> v;
      std::string err;
      if (!parseVectorLiteral(raw.s, v, err)) {
        code = "invalid_vector";
        msg = err;
        return false;
      }
      if (static_cast<long long>(v.size()) != c.n) {
        code = "dimension_mismatch";
        msg = "vector dimension mismatch: expected " + std::to_string(c.n) + ", got " + std::to_string(v.size());
        return false;
      }
      out.k = VK::Vec;
      out.v = std::move(v);
      return true;
    }
  }
  code = "type_mismatch";
  msg = "unknown column type";
  return false;
}

}  // namespace

Engine::Engine(std::string dataDir) : dataDir_(std::move(dataDir)) { recover(); }

void Engine::recover() {
  std::vector<CatalogEntry> entries;
  std::string meta;
  if (readWholeFile(joinPath(dataDir_, kMetaFile), meta)) {
    std::string err;
    if (!decodeCatalog(meta, entries, err)) std::cerr << "[minivectordb] bad catalog: " << err << "\n";
  }

  uint64_t maxCommitted = 0;
  uint64_t maxXid = 0;
  std::string err;
  std::string txnPath = joinPath(dataDir_, kTxnFile);
  ScanInfo txnInfo;
  if (!replayTxnLog(txnPath, maxCommitted, txnInfo, err)) std::cerr << "[minivectordb] " << err << "\n";
  maxXid = txnInfo.maxXid;
  err.clear();
  if (!txnLog_.open(txnPath, txnInfo.committedEnd, err)) std::cerr << "[minivectordb] " << err << "\n";
  err.clear();

  for (const CatalogEntry& e : entries) {
    Table& t = tables_[e.name];
    t.name = e.name;
    t.path = joinPath(dataDir_, e.name + ".log");
    t.schema = e.schema;
    t.hasVecIndex = e.hasVecIndex;
    for (size_t k = 0; k < t.schema.size(); ++k) {
      if (t.schema[k].pk) t.pkIdx = static_cast<int>(k);
      if (t.schema[k].type == ColType::Vector) t.vecIdx = static_cast<int>(k);
    }
    // Scan before opening: open trims whatever the scan refused to apply, so an
    // interrupted transaction cannot survive into the next run.
    ScanInfo info;
    if (!replayTableLog(t.path, maxCommitted, t.index, info, err)) std::cerr << "[minivectordb] " << err << "\n";
    err.clear();
    if (info.maxXid > maxXid) maxXid = info.maxXid;
    if (!t.log.open(t.path, info.committedEnd, err)) std::cerr << "[minivectordb] " << err << "\n";
    err.clear();
  }
  nextXid_ = maxXid + 1;
}

bool Engine::writeCatalog(std::string& err) {
  std::vector<CatalogEntry> entries;
  for (const auto& kv : tables_) {
    CatalogEntry e;
    e.name = kv.second.name;
    e.schema = kv.second.schema;
    e.hasVecIndex = kv.second.hasVecIndex;
    entries.push_back(std::move(e));
  }
  return writeFileAtomic(joinPath(dataDir_, kMetaFile), encodeCatalog(entries), err);
}

Engine::Mark Engine::mark() const {
  Mark m;
  m.undo = undo_.size();
  for (const auto& kv : tables_) m.logPos.emplace(kv.first, kv.second.log.pos());
  return m;
}

bool Engine::openReader(const Table& t, LogReader& rd, ExecResult& err) {
  // The scan reads through its own handle, so anything this transaction has
  // staged but not yet committed must leave the write buffer first.
  std::string ioErr;
  if (!t.log.flush(ioErr) || !rd.open(t.path, ioErr)) {
    err = makeError("storage_error", ioErr);
    return false;
  }
  rd.setEnd(t.log.pos());
  return true;
}

void Engine::stageRow(Table& t, long long pk, const std::vector<Value>& row) {
  std::string body;
  encodeRow(row, t.schema, body);
  auto it = t.index.find(pk);
  undo_.push_back(UndoOp{t.name, pk, it != t.index.end(), it != t.index.end() ? it->second : RowState{}});
  RowState next;
  next.pos = t.log.append('I', stmtXid_, pk, body);
  if (it != t.index.end()) it->second = next;
  else t.index.emplace(pk, next);
}

void Engine::stageDelete(Table& t, long long pk) {
  auto it = t.index.find(pk);
  if (it == t.index.end() || it->second.deleted) return;
  undo_.push_back(UndoOp{t.name, pk, true, it->second});
  it->second.pos = t.log.append('D', stmtXid_, pk, std::string());
  it->second.deleted = true;
}

void Engine::rollbackTo(const Mark& m) {
  std::string err;
  for (const auto& kv : m.logPos) {
    auto tit = tables_.find(kv.first);
    if (tit == tables_.end()) continue;
    if (!tit->second.log.truncateBack(kv.second, err)) std::cerr << "[minivectordb] " << err << "\n";
    err.clear();
  }
  for (size_t k = undo_.size(); k-- > m.undo;) {
    auto tit = tables_.find(undo_[k].table);
    if (tit == tables_.end()) continue;
    if (undo_[k].had) tit->second.index[undo_[k].pk] = undo_[k].prev;
    else tit->second.index.erase(undo_[k].pk);
  }
  if (undo_.size() > m.undo) undo_.resize(m.undo);
}

bool Engine::commitFrom(uint64_t xid, size_t undoFrom, std::string& err) {
  std::vector<Table*> touched;
  for (size_t k = undoFrom; k < undo_.size(); ++k) {
    auto tit = tables_.find(undo_[k].table);
    if (tit == tables_.end()) continue;
    bool seen = false;
    for (Table* p : touched)
      if (p == &tit->second) seen = true;
    if (!seen) touched.push_back(&tit->second);
  }
  if (touched.empty()) return true;  // nothing staged, so nothing to make durable
  for (Table* p : touched)
    if (!p->log.sync(err)) return false;
  txnLog_.append('T', xid, 0, std::string());
  return txnLog_.sync(err);
}

bool Engine::shutdown(std::string& err) {
  if (inTx_) {
    rollbackTo(txMark_);
    inTx_ = false;
  }
  for (auto& kv : tables_)
    if (!kv.second.log.sync(err)) return false;
  return txnLog_.sync(err);
}

double Engine::l2(const std::vector<float>& a, const std::vector<float>& b) {
  double sum = 0.0;
  for (size_t k = 0; k < a.size(); ++k) {
    double d = static_cast<double>(a[k]) - static_cast<double>(b[k]);
    sum += d * d;
  }
  return std::sqrt(sum);
}

ExecResult Engine::makeError(const std::string& code, const std::string& msg) {
  ExecResult r;
  r.ok = false;
  r.code = code;
  r.msg = msg;
  return r;
}

bool Engine::condEval(const Cond& c, const std::vector<Value>& row, const Table& t, bool& matched,
                      std::string& code, std::string& msg) {
  matched = false;
  int idx = -1;
  for (size_t k = 0; k < t.schema.size(); ++k)
    if (t.schema[k].name == c.col) idx = static_cast<int>(k);
  if (idx < 0) {
    code = "unknown_column";
    msg = "unknown column in condition: " + c.col;
    return false;
  }
  const ColDef& cd = t.schema[idx];
  if (cd.type == ColType::Vector) {
    code = "unsupported_sql";
    msg = "vector columns cannot be used in WHERE";
    return false;
  }
  int cmp = 0;
  if (cd.type == ColType::Bigint) {
    if (c.lit.k != VK::Int) {
      code = "type_mismatch";
      msg = "condition on BIGINT column " + c.col + " needs an integer literal";
      return false;
    }
    cmp = row[idx].i < c.lit.i ? -1 : (row[idx].i > c.lit.i ? 1 : 0);
  } else {
    if (c.lit.k != VK::Str) {
      code = "type_mismatch";
      msg = "condition on VARCHAR column " + c.col + " needs a string literal";
      return false;
    }
    cmp = cmpStr(row[idx].s, c.lit.s);
  }
  switch (c.op) {
    case CmpOp::Eq: matched = cmp == 0; break;
    case CmpOp::Lt: matched = cmp < 0; break;
    case CmpOp::Le: matched = cmp <= 0; break;
    case CmpOp::Gt: matched = cmp > 0; break;
    case CmpOp::Ge: matched = cmp >= 0; break;
  }
  return true;
}

WhereVerdict Engine::evalWhere(const std::vector<Cond>& where, const std::vector<Value>& row, const Table& t,
                               ExecResult& err) {
  for (const Cond& c : where) {
    bool matched = false;
    std::string code, msg;
    if (!condEval(c, row, t, matched, code, msg)) {
      err = makeError(code, msg);
      return WhereVerdict::Error;
    }
    if (!matched) return WhereVerdict::NoMatch;
  }
  return WhereVerdict::Match;
}

ExecResult Engine::doBegin() {
  if (inTx_) return makeError("transaction_active", "a transaction is already open");
  inTx_ = true;
  txXid_ = nextXid_++;
  txMark_ = mark();
  ExecResult r;
  r.ok = true;
  r.plain = true;
  return r;
}

ExecResult Engine::doCommit() {
  if (!inTx_) {
    ExecResult r;
    r.ok = true;
    r.plain = true;
    return r;
  }
  std::string err;
  if (!commitFrom(txXid_, txMark_.undo, err)) {
    rollbackTo(txMark_);
    inTx_ = false;
    return makeError("storage_error", err);
  }
  inTx_ = false;
  undo_.clear();
  ExecResult r;
  r.ok = true;
  r.plain = true;
  return r;
}

ExecResult Engine::doRollback() {
  if (inTx_) {
    rollbackTo(txMark_);
    inTx_ = false;
  }
  ExecResult r;
  r.ok = true;
  r.plain = true;
  return r;
}

ExecResult Engine::exec(const Stmt& st, const std::string& searchMode) {
  const bool mutating = st.k == Stmt::K::Insert || st.k == Stmt::K::Update || st.k == Stmt::K::Delete;
  if (mutating) {
    stmtMark_ = mark();
    stmtXid_ = inTx_ ? txXid_ : nextXid_++;
  }

  ExecResult r;
  switch (st.k) {
    case Stmt::K::CreateTable: r = doCreateTable(st); break;
    case Stmt::K::CreateVectorIndex: r = doCreateVectorIndex(st); break;
    case Stmt::K::Insert: r = doInsert(st); break;
    case Stmt::K::Select: return doSelect(st, searchMode);
    case Stmt::K::Update: r = doUpdate(st); break;
    case Stmt::K::Delete: r = doDelete(st); break;
    case Stmt::K::Begin: return doBegin();
    case Stmt::K::Commit: return doCommit();
    case Stmt::K::Rollback: return doRollback();
  }

  if (!r.ok) {
    if (mutating) rollbackTo(stmtMark_);
    return r;
  }
  if (!mutating || inTx_) return r;
  std::string err;
  if (!commitFrom(stmtXid_, stmtMark_.undo, err)) {
    rollbackTo(stmtMark_);
    return makeError("storage_error", err);
  }
  undo_.clear();
  return r;
}

ExecResult Engine::doCreateTable(const Stmt& st) {
  if (tables_.count(st.table)) return makeError("duplicate_table", "table already exists: " + st.table);
  Table& t = tables_[st.table];
  t.name = st.table;
  t.path = joinPath(dataDir_, st.table + ".log");
  t.schema = st.cols;
  for (size_t k = 0; k < t.schema.size(); ++k) {
    if (t.schema[k].pk) t.pkIdx = static_cast<int>(k);
    if (t.schema[k].type == ColType::Vector) t.vecIdx = static_cast<int>(k);
  }
  std::string err;
  if (!t.log.open(t.path, UINT64_MAX, err)) {
    tables_.erase(st.table);
    return makeError("storage_error", err);
  }
  if (!writeCatalog(err)) {
    t.log.close();
    tables_.erase(st.table);
    return makeError("storage_error", err);
  }
  ExecResult r;
  r.ok = true;
  r.plain = true;
  return r;
}

ExecResult Engine::doCreateVectorIndex(const Stmt& st) {
  auto it = tables_.find(st.table);
  if (it == tables_.end()) return makeError("unknown_table", st.table);
  int idx = -1;
  for (size_t k = 0; k < it->second.schema.size(); ++k)
    if (it->second.schema[k].name == st.col) idx = static_cast<int>(k);
  if (idx < 0) return makeError("unknown_column", st.col);
  if (it->second.schema[idx].type != ColType::Vector)
    return makeError("invalid_schema", "index column is not VECTOR");
  it->second.hasVecIndex = true;
  std::string err;
  if (!writeCatalog(err)) return makeError("storage_error", err);
  ExecResult r;
  r.ok = true;
  r.plain = true;
  return r;
}

ExecResult Engine::doInsert(const Stmt& st) {
  auto tit = tables_.find(st.table);
  if (tit == tables_.end()) return makeError("unknown_table", st.table);
  Table& t = tit->second;
  std::vector<int> colMap;
  if (st.insertCols.empty()) {
    for (size_t k = 0; k < t.schema.size(); ++k) colMap.push_back(static_cast<int>(k));
  } else {
    for (const auto& name : st.insertCols) {
      int idx = -1;
      for (size_t k = 0; k < t.schema.size(); ++k)
        if (t.schema[k].name == name) idx = static_cast<int>(k);
      if (idx < 0) return makeError("unknown_column", name);
      colMap.push_back(idx);
    }
    if (colMap.size() != t.schema.size())
      return makeError("column_count_mismatch", "column list must cover all columns");
  }
  long long staged = 0;
  for (const auto& rawRow : st.insertRows) {
    if (rawRow.size() != colMap.size())
      return makeError("column_count_mismatch", "row has " + std::to_string(rawRow.size()) + " values");
    std::vector<Value> row(t.schema.size());
    for (size_t k = 0; k < rawRow.size(); ++k) {
      std::string code, msg;
      if (!convertValue(rawRow[k], t.schema[colMap[k]], row[colMap[k]], code, msg)) return makeError(code, msg);
    }
    long long pk = row[t.pkIdx].i;
    auto it = t.index.find(pk);
    if (it != t.index.end() && !it->second.deleted)
      return makeError("duplicate_key", "duplicate primary key: " + std::to_string(pk));
    ++staged;
    stageRow(t, pk, row);
  }
  ExecResult r;
  r.ok = true;
  r.affected = staged;
  return r;
}

ExecResult Engine::doSelect(const Stmt& st, const std::string& mode) {
  (void)mode;
  auto tit = tables_.find(st.table);
  if (tit == tables_.end()) return makeError("unknown_table", st.table);
  const Table& t = tit->second;

  struct Proj {
    enum class K { Col, Dist } k;
    int colIdx = -1;
    std::vector<float> q;
    std::string outName;
  };
  std::vector<Proj> projs;
  for (const auto& item : st.items) {
    if (item.k == SelItem::K::Star) {
      for (size_t k = 0; k < t.schema.size(); ++k) {
        Proj p;
        p.k = Proj::K::Col;
        p.colIdx = static_cast<int>(k);
        p.outName = t.schema[k].name;
        projs.push_back(p);
      }
    } else if (item.k == SelItem::K::Col) {
      int idx = -1;
      for (size_t k = 0; k < t.schema.size(); ++k)
        if (t.schema[k].name == item.name) idx = static_cast<int>(k);
      if (idx < 0) return makeError("unknown_column", item.name);
      Proj p;
      p.k = Proj::K::Col;
      p.colIdx = idx;
      p.outName = item.alias.empty() ? item.name : item.alias;
      projs.push_back(p);
    } else {
      int idx = -1;
      for (size_t k = 0; k < t.schema.size(); ++k)
        if (t.schema[k].name == item.name) idx = static_cast<int>(k);
      if (idx < 0) return makeError("unknown_column", item.name);
      if (t.schema[idx].type != ColType::Vector)
        return makeError("type_mismatch", "L2_DISTANCE needs a VECTOR column");
      Proj p;
      p.k = Proj::K::Dist;
      p.colIdx = idx;
      p.q = item.q;
      p.outName = item.alias.empty() ? "distance" : item.alias;
      projs.push_back(p);
    }
  }

  // Resolve ORDER BY before scanning, so a bad reference is reported even when
  // the table holds no rows.
  struct Key {
    int projIdx = -1;  // distance projection, or -1 for a stored column
    int colIdx = -1;
    ColType ct = ColType::Bigint;
    bool desc = false;
  };
  std::vector<Key> keys;
  bool needRow = false;
  for (const auto& ok : st.order) {
    Key key;
    for (size_t k = 0; k < projs.size(); ++k)
      if (projs[k].k == Proj::K::Dist && projs[k].outName == ok.ref) key.projIdx = static_cast<int>(k);
    if (key.projIdx < 0) {
      for (size_t k = 0; k < t.schema.size(); ++k)
        if (t.schema[k].name == ok.ref) key.colIdx = static_cast<int>(k);
      if (key.colIdx < 0) return makeError("unknown_column", ok.ref);
      if (t.schema[key.colIdx].type == ColType::Vector)
        return makeError("unsupported_sql", "cannot order by vector column");
      key.ct = t.schema[key.colIdx].type;
      needRow = true;
    }
    key.desc = ok.desc;
    keys.push_back(key);
  }

  struct Cand {
    long long pk = 0;
    std::vector<Json> out;
    std::vector<Value> row;  // kept only when an ORDER BY key reads a column
  };
  std::vector<Cand> cands;
  LogReader rd;
  ExecResult ioErr;
  if (!openReader(t, rd, ioErr)) return ioErr;
  RecView rec;
  const uint8_t* body = nullptr;
  size_t bodyLen = 0;
  std::vector<Value> row;

  // ORDER BY <distance> ASC LIMIT k is the vector TopK shape. A bounded heap
  // keeps k rows resident instead of one candidate per table row, and only the
  // survivors are turned into JSON.
  const bool topK = keys.size() == 1 && keys[0].projIdx >= 0 && !keys[0].desc && st.limit > 0;
  if (topK) {
    struct Entry {
      double dist;
      long long pk;
      std::vector<Value> row;
    };
    auto better = [](double d1, long long p1, double d2, long long p2) {
      if (d1 != d2) return d1 < d2;
      return p1 < p2;
    };
    auto byEntry = [&](const Entry& a, const Entry& b) { return better(a.dist, a.pk, b.dist, b.pk); };
    const size_t k = static_cast<size_t>(st.limit);
    const Proj& dp = projs[keys[0].projIdx];
    std::vector<Entry> heap;
    heap.reserve(k < 1024 ? k : 1024);
    while (rd.next(rec, body, bodyLen)) {
      if (!liveVersion(t.index, rec)) continue;
      if (!decodeRow(body, bodyLen, t.schema, row))
        return makeError("storage_error", "corrupt row for key " + std::to_string(rec.pk));
      ExecResult whereErr;
      WhereVerdict verdict = evalWhere(st.where, row, t, whereErr);
      if (verdict == WhereVerdict::Error) return whereErr;
      if (verdict != WhereVerdict::Match) continue;
      double d = l2(row[dp.colIdx].v, dp.q);
      if (heap.size() < k) {
        heap.push_back(Entry{d, rec.pk, row});
        std::push_heap(heap.begin(), heap.end(), byEntry);
      } else if (better(d, rec.pk, heap.front().dist, heap.front().pk)) {
        std::pop_heap(heap.begin(), heap.end(), byEntry);
        heap.back() = Entry{d, rec.pk, row};
        std::push_heap(heap.begin(), heap.end(), byEntry);
      }
    }
    std::sort_heap(heap.begin(), heap.end(), byEntry);
    cands.reserve(heap.size());
    for (Entry& e : heap) {
      Cand c;
      c.pk = e.pk;
      c.out.reserve(projs.size());
      for (size_t j = 0; j < projs.size(); ++j) {
        if (projs[j].k == Proj::K::Col) c.out.push_back(valueToJson(e.row[projs[j].colIdx]));
        else c.out.push_back(doubleToJson(l2(e.row[projs[j].colIdx].v, projs[j].q)));
      }
      cands.push_back(std::move(c));
    }
  } else {
    while (rd.next(rec, body, bodyLen)) {
      if (!liveVersion(t.index, rec)) continue;
      if (!decodeRow(body, bodyLen, t.schema, row))
        return makeError("storage_error", "corrupt row for key " + std::to_string(rec.pk));
      ExecResult whereErr;
      WhereVerdict verdict = evalWhere(st.where, row, t, whereErr);
      if (verdict == WhereVerdict::Error) return whereErr;
      if (verdict == WhereVerdict::NoMatch) continue;
      Cand c;
      c.pk = rec.pk;
      c.out.reserve(projs.size());
      for (size_t j = 0; j < projs.size(); ++j) {
        if (projs[j].k == Proj::K::Col) c.out.push_back(valueToJson(row[projs[j].colIdx]));
        else c.out.push_back(doubleToJson(l2(row[projs[j].colIdx].v, projs[j].q)));
      }
      if (needRow) c.row = row;
      cands.push_back(std::move(c));
    }
  }

  if (keys.empty()) {
    // The log yields rows in write order; report them by primary key.
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.pk < b.pk; });
  } else if (!topK) {
    std::stable_sort(cands.begin(), cands.end(), [&](const Cand& a, const Cand& b) {
      for (const Key& key : keys) {
        int cmp = 0;
        if (key.projIdx >= 0) {
          double da = a.out[key.projIdx].d;
          double db = b.out[key.projIdx].d;
          cmp = da < db ? -1 : (da > db ? 1 : 0);
        } else if (key.ct == ColType::Bigint) {
          long long ia = a.row[key.colIdx].i;
          long long ib = b.row[key.colIdx].i;
          cmp = ia < ib ? -1 : (ia > ib ? 1 : 0);
        } else {
          cmp = cmpStr(a.row[key.colIdx].s, b.row[key.colIdx].s);
        }
        if (cmp != 0) return key.desc ? cmp > 0 : cmp < 0;
      }
      return a.pk < b.pk;
    });
  }

  ExecResult r;
  r.ok = true;
  r.resultSet = true;
  for (const auto& p : projs) r.columns.push_back(p.outName);
  size_t n = cands.size();
  if (st.limit >= 0 && static_cast<size_t>(st.limit) < n) n = static_cast<size_t>(st.limit);
  for (size_t k = 0; k < n; ++k) r.rows.push_back(std::move(cands[k].out));
  return r;
}

ExecResult Engine::doUpdate(const Stmt& st) {
  auto tit = tables_.find(st.table);
  if (tit == tables_.end()) return makeError("unknown_table", st.table);
  Table& t = tit->second;
  std::vector<std::pair<int, Value>> sets;
  for (const auto& s : st.sets) {
    int idx = -1;
    for (size_t k = 0; k < t.schema.size(); ++k)
      if (t.schema[k].name == s.first) idx = static_cast<int>(k);
    if (idx < 0) return makeError("unknown_column", s.first);
    if (idx == t.pkIdx) return makeError("unsupported_sql", "primary key update is not supported");
    Value converted;
    std::string code, msg;
    if (!convertValue(s.second, t.schema[idx], converted, code, msg)) return makeError(code, msg);
    sets.emplace_back(idx, std::move(converted));
  }
  long long affected = 0;
  LogReader rd;
  ExecResult ioErr;
  if (!openReader(t, rd, ioErr)) return ioErr;
  RecView rec;
  const uint8_t* body = nullptr;
  size_t bodyLen = 0;
  std::vector<Value> row;
  while (rd.next(rec, body, bodyLen)) {
    if (!liveVersion(t.index, rec)) continue;
    if (!decodeRow(body, bodyLen, t.schema, row))
      return makeError("storage_error", "corrupt row for key " + std::to_string(rec.pk));
    ExecResult whereErr;
    WhereVerdict verdict = evalWhere(st.where, row, t, whereErr);
    if (verdict == WhereVerdict::Error) return whereErr;
    if (verdict != WhereVerdict::Match) continue;
    for (const auto& s : sets) row[s.first] = s.second;
    stageRow(t, rec.pk, row);  // the reader stops at the pre-update end
    ++affected;
  }
  ExecResult r;
  r.ok = true;
  r.affected = affected;
  return r;
}

ExecResult Engine::doDelete(const Stmt& st) {
  auto tit = tables_.find(st.table);
  if (tit == tables_.end()) return makeError("unknown_table", st.table);
  Table& t = tit->second;
  long long affected = 0;
  LogReader rd;
  ExecResult ioErr;
  if (!openReader(t, rd, ioErr)) return ioErr;
  RecView rec;
  const uint8_t* body = nullptr;
  size_t bodyLen = 0;
  std::vector<Value> row;
  while (rd.next(rec, body, bodyLen)) {
    if (!liveVersion(t.index, rec)) continue;
    if (!decodeRow(body, bodyLen, t.schema, row))
      return makeError("storage_error", "corrupt row for key " + std::to_string(rec.pk));
    ExecResult whereErr;
    WhereVerdict verdict = evalWhere(st.where, row, t, whereErr);
    if (verdict == WhereVerdict::Error) return whereErr;
    if (verdict != WhereVerdict::Match) continue;
    stageDelete(t, rec.pk);
    ++affected;
  }
  ExecResult r;
  r.ok = true;
  r.affected = affected;
  return r;
}

}  // namespace mvdb
