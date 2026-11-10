// Append-only record logs with a single global commit point.
//
// Layout under the data directory:
//   meta.json     table definitions (rewritten atomically on CREATE TABLE)
//   txn.log       one record per committed transaction id
//   <table>.log   row operations, each tagged with the transaction that wrote it
//
// Every record is length-prefixed and CRC-checked:
//   u32le payload length | u32le crc32(payload) | payload
// and a payload is op byte | u64le xid | i64le primary key | body, where body
// is the full row encoding for 'I', empty for 'D', and empty for the 'T'
// commit record in txn.log.
//
// A transaction appends its row records, fsyncs the table logs, then appends
// and fsyncs its 'T' record. Recovery applies a row record only if its xid is
// at or below the highest committed id, so a crash anywhere before the commit
// record leaves nothing behind and a crash inside one leaves no partial
// transaction.
//
// Row bodies stay on disk: the engine keeps only a primary-key -> record
// offset index in memory, so a dataset larger than the memory budget is
// streamed from the log instead of being loaded whole.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "value.hpp"

namespace mvdb {

uint32_t crc32Of(const void* data, size_t len);

// Positional row encoding driven by the table schema; no per-field type tags.
void encodeRow(const std::vector<Value>& row, const std::vector<ColDef>& schema, std::string& out);
bool decodeRow(const uint8_t* p, size_t avail, const std::vector<ColDef>& schema, std::vector<Value>& out);

struct CatalogEntry {
  std::string name;
  std::vector<ColDef> schema;
  bool hasVecIndex = false;
};

std::string encodeCatalog(const std::vector<CatalogEntry>& entries);
bool decodeCatalog(const std::string& text, std::vector<CatalogEntry>& out, std::string& err);

// Writes path.tmp, fsyncs, then atomically replaces path.
bool writeFileAtomic(const std::string& path, const std::string& bytes, std::string& err);

// A record as seen while scanning: enough to test "is this the live version of
// pk" against the engine's index without decoding the body.
struct RecView {
  char op = 0;
  uint64_t xid = 0;
  long long pk = 0;
  uint64_t payloadOff = 0;  // byte offset of the payload within the log file
  uint32_t payloadLen = 0;
};

// Where a just-appended record lives, so the engine can read its row back.
struct RecPos {
  uint64_t off = 0;
  uint32_t len = 0;
};

// Transaction ids are allocated and committed in order by this single writer,
// so the committed set is a prefix: everything at or below maxCommitted counts,
// anything above it belongs to a transaction that never reached its commit
// record. The scan reports the highest id seen anywhere, so the next start-up
// cannot reuse one, and the end of the committed run, so an interrupted log can
// be trimmed back to exactly what recovery accepted.
struct ScanInfo {
  uint64_t maxXid = 0;
  uint64_t committedEnd = 0;
};

bool replayTxnLog(const std::string& path, uint64_t& maxCommitted, ScanInfo& info, std::string& err);

// One primary key's current whereabouts: the record holding its row, or the
// delete record that retired it. This index is the only per-row state the
// engine keeps in memory.
struct RowState {
  RecPos pos;
  bool deleted = false;
};

// Rebuilds the index from a table log without decoding any row, applying only
// records whose transaction reached the commit point.
bool replayTableLog(const std::string& path, uint64_t maxCommitted, std::map<long long, RowState>& index,
                    ScanInfo& info, std::string& err);

class AppendLog {
 public:
  AppendLog() = default;
  AppendLog(const AppendLog&) = delete;
  AppendLog& operator=(const AppendLog&) = delete;
  ~AppendLog();
  // Drops everything at or beyond limit, which must be a record boundary.
  bool open(const std::string& path, uint64_t limit, std::string& err);
  RecPos append(char op, uint64_t xid, long long pk, const std::string& body);
  // Pushes buffered records to the OS without an fsync, so a scan through a
  // second handle can see what this transaction has already staged.
  bool flush(std::string& err) const;
  bool sync(std::string& err);  // fflush + fsync
  bool truncateBack(uint64_t size, std::string& err);
  uint64_t pos() const { return pos_; }
  void close();

 private:
  FILE* fp_ = nullptr;
  uint64_t pos_ = 0;
  bool failed_ = false;
  mutable bool dirty_ = false;
};

// Forward-only, chunk-buffered walk over a log. Scans use this instead of one
// random read per row, and skip the CRC: the log they read was written by this
// process, and verifying it is recovery's job.
class LogReader {
 public:
  LogReader() = default;
  LogReader(const LogReader&) = delete;
  LogReader& operator=(const LogReader&) = delete;
  ~LogReader();
  bool open(const std::string& path, std::string& err);
  // Stops the walk at a byte offset, so records appended after the snapshot
  // began stay invisible to it.
  void setEnd(uint64_t end) { end_ = end; }
  // Recovery also checks each record's CRC and stops at the first bad one.
  void setVerify(bool on) { verify_ = on; }
  bool stoppedOnDamage() const { return damaged_; }
  // body points into the reader's buffer and is only valid until the next call.
  bool next(RecView& v, const uint8_t*& body, size_t& bodyLen);
  void close();

 private:
  bool fill(uint64_t off, size_t want);

  FILE* fp_ = nullptr;
  std::vector<uint8_t> buf_;
  uint64_t bufOff_ = 0;
  size_t bufLen_ = 0;
  uint64_t pos_ = 0;
  uint64_t end_ = UINT64_MAX;
  bool verify_ = false;
  bool damaged_ = false;
};

}  // namespace mvdb
