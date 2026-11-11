#include "storage.hpp"

#include <cstring>
#include <utility>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "json.hpp"

namespace mvdb {

namespace {

const size_t kMaxRecord = 64u << 20;
const size_t kHeaderSize = 8;
const size_t kPrefixSize = 17;  // op byte + 8-byte xid + 8-byte primary key
const size_t kChunkBytes = 1u << 20;

void putU32(std::string& out, uint32_t v) {
  char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
               static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
  out.append(b, 4);
}

void putI64(std::string& out, long long v) {
  uint64_t u = static_cast<uint64_t>(v);
  for (int k = 0; k < 8; ++k) out.push_back(static_cast<char>((u >> (8 * k)) & 0xFF));
}

void putF32(std::string& out, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  putU32(out, u);
}

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

long long getI64(const uint8_t* p) {
  uint64_t u = 0;
  for (int k = 7; k >= 0; --k) u = (u << 8) | p[k];
  return static_cast<long long>(u);
}

float getF32(const uint8_t* p) {
  uint32_t u = getU32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

bool syncFile(FILE* fp) {
#ifdef _WIN32
  return _commit(_fileno(fp)) == 0;
#else
  return fsync(fileno(fp)) == 0;
#endif
}

bool truncateTo(FILE* fp, uint64_t size) {
#ifdef _WIN32
  return _chsize_s(_fileno(fp), static_cast<long long>(size)) == 0;
#else
  return ftruncate(fileno(fp), static_cast<off_t>(size)) == 0;
#endif
}

// long is 32-bit on Windows, so a log above 2 GiB needs the wide variants.
int seek64(FILE* fp, uint64_t off, int whence) {
#ifdef _WIN32
  return _fseeki64(fp, static_cast<__int64>(off), whence);
#else
  return fseeko(fp, static_cast<off_t>(off), whence);
#endif
}

uint64_t tell64(FILE* fp) {
#ifdef _WIN32
  __int64 v = _ftelli64(fp);
  return v < 0 ? 0 : static_cast<uint64_t>(v);
#else
  off_t v = ftello(fp);
  return v < 0 ? 0 : static_cast<uint64_t>(v);
#endif
}

const char* colTypeName(ColType t) {
  switch (t) {
    case ColType::Bigint: return "bigint";
    case ColType::Varchar: return "varchar";
    case ColType::Vector: return "vector";
  }
  return "bigint";
}

bool colTypeFromName(const std::string& s, ColType& out) {
  if (s == "bigint") out = ColType::Bigint;
  else if (s == "varchar") out = ColType::Varchar;
  else if (s == "vector") out = ColType::Vector;
  else return false;
  return true;
}

bool fileExists(const std::string& path) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;
  std::fclose(fp);
  return true;
}

// Byte offset just past the last record whose header, body and CRC are intact.
// Reads are buffered: a per-record seek would cost one syscall per row.
uint64_t intactEnd(FILE* fp) {
  std::vector<uint8_t> buf(kChunkBytes);
  uint64_t bufOff = 0;
  size_t bufLen = 0;
  auto load = [&](uint64_t off, size_t want) -> const uint8_t* {
    if (want > buf.size()) buf.resize(want);
    if (bufLen >= want && bufOff <= off && off + want <= bufOff + bufLen) return buf.data() + (off - bufOff);
    bufOff = off;
    if (seek64(fp, off, SEEK_SET) != 0) {
      bufLen = 0;
      return nullptr;
    }
    bufLen = std::fread(buf.data(), 1, buf.size(), fp);
    return bufLen < want ? nullptr : buf.data() + (off - bufOff);
  };
  uint64_t pos = 0;
  while (true) {
    const uint8_t* header = load(pos, kHeaderSize);
    if (!header) break;
    uint32_t len = getU32(header);
    uint32_t crc = getU32(header + 4);
    if (len < kPrefixSize || len > kMaxRecord) break;
    const uint8_t* payload = load(pos + kHeaderSize, len);
    if (!payload || crc32Of(payload, len) != crc) break;
    pos += kHeaderSize + len;
  }
  return pos;
}

}  // namespace

uint32_t crc32Of(const void* data, size_t len) {
  static uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    built = true;
  }
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void encodeRow(const std::vector<Value>& row, const std::vector<ColDef>& schema, std::string& out) {
  for (size_t k = 0; k < schema.size(); ++k) {
    const Value& v = row[k];
    switch (schema[k].type) {
      case ColType::Bigint: putI64(out, v.i); break;
      case ColType::Varchar:
        putU32(out, static_cast<uint32_t>(v.s.size()));
        out.append(v.s);
        break;
      case ColType::Vector:
        for (float f : v.v) putF32(out, f);
        break;
    }
  }
}

bool decodeRow(const uint8_t* p, size_t avail, const std::vector<ColDef>& schema, std::vector<Value>& out) {
  size_t used = 0;
  out.assign(schema.size(), Value());
  for (size_t k = 0; k < schema.size(); ++k) {
    switch (schema[k].type) {
      case ColType::Bigint:
        if (used + 8 > avail) return false;
        out[k].k = VK::Int;
        out[k].i = getI64(p + used);
        used += 8;
        break;
      case ColType::Varchar: {
        if (used + 4 > avail) return false;
        uint32_t n = getU32(p + used);
        used += 4;
        if (used + n > avail) return false;
        out[k].k = VK::Str;
        out[k].s.assign(reinterpret_cast<const char*>(p + used), n);
        used += n;
        break;
      }
      case ColType::Vector: {
        size_t bytes = static_cast<size_t>(schema[k].n) * 4;
        if (used + bytes > avail) return false;
        out[k].k = VK::Vec;
        out[k].v.resize(static_cast<size_t>(schema[k].n));
        for (size_t d = 0; d < out[k].v.size(); ++d) out[k].v[d] = getF32(p + used + 4 * d);
        used += bytes;
        break;
      }
    }
  }
  return used == avail;
}

std::string encodeCatalog(const std::vector<CatalogEntry>& entries) {
  Json root = Json::object();
  Json tables = Json::array();
  for (const CatalogEntry& e : entries) {
    Json t = Json::object();
    t.set("name", Json::str(e.name));
    t.set("vec_index", Json::boolean(e.hasVecIndex));
    Json cols = Json::array();
    for (const ColDef& c : e.schema) {
      Json col = Json::object();
      col.set("name", Json::str(c.name));
      col.set("type", Json::str(colTypeName(c.type)));
      col.set("n", Json::integer(c.n));
      col.set("pk", Json::boolean(c.pk));
      cols.arr.push_back(col);
    }
    t.set("columns", cols);
    tables.arr.push_back(t);
  }
  root.set("tables", tables);
  return root.dump() + "\n";
}

bool decodeCatalog(const std::string& text, std::vector<CatalogEntry>& out, std::string& err) {
  Json root;
  if (!Json::parse(text, root, err)) return false;
  const Json* tables = root.find("tables");
  if (!tables || tables->t != JType::Array) {
    err = "catalog has no tables array";
    return false;
  }
  for (const Json& t : tables->arr) {
    CatalogEntry e;
    const Json* name = t.find("name");
    const Json* cols = t.find("columns");
    if (!name || name->t != JType::String || !cols || cols->t != JType::Array) {
      err = "malformed catalog entry";
      return false;
    }
    e.name = name->s;
    const Json* vi = t.find("vec_index");
    e.hasVecIndex = (vi && vi->t == JType::Bool && vi->b);
    for (const Json& c : cols->arr) {
      const Json* cn = c.find("name");
      const Json* ct = c.find("type");
      if (!cn || cn->t != JType::String || !ct || ct->t != JType::String) {
        err = "malformed column in catalog";
        return false;
      }
      ColDef d;
      d.name = cn->s;
      if (!colTypeFromName(ct->s, d.type)) {
        err = "unknown column type: " + ct->s;
        return false;
      }
      const Json* n = c.find("n");
      if (n && n->t == JType::Int) d.n = n->i;
      const Json* pk = c.find("pk");
      d.pk = (pk && pk->t == JType::Bool && pk->b);
      e.schema.push_back(d);
    }
    out.push_back(std::move(e));
  }
  return true;
}

bool writeFileAtomic(const std::string& path, const std::string& bytes, std::string& err) {
  std::string tmp = path + ".tmp";
  FILE* fp = std::fopen(tmp.c_str(), "wb");
  if (!fp) {
    err = "cannot open " + tmp;
    return false;
  }
  bool io = std::fwrite(bytes.data(), 1, bytes.size(), fp) == bytes.size();
  io = io && std::fflush(fp) == 0;
  io = io && syncFile(fp);
  std::fclose(fp);
  if (!io) {
    err = "cannot write " + tmp;
    return false;
  }
#ifdef _WIN32
  if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    err = "cannot replace " + path;
    return false;
  }
#else
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    err = "cannot replace " + path;
    return false;
  }
#endif
  return true;
}

bool replayTxnLog(const std::string& path, uint64_t& maxCommitted, ScanInfo& info, std::string& err) {
  LogReader rd;
  if (!rd.open(path, err)) {
    if (!fileExists(path)) {
      err.clear();
      return true;  // nothing has been committed yet
    }
    return false;
  }
  rd.setVerify(true);
  RecView v;
  const uint8_t* body = nullptr;
  size_t bodyLen = 0;
  while (rd.next(v, body, bodyLen)) {
    if (v.xid > info.maxXid) info.maxXid = v.xid;
    if (v.op == 'T' && v.xid > maxCommitted) maxCommitted = v.xid;
    info.committedEnd = v.payloadOff + v.payloadLen;
  }
  if (rd.stoppedOnDamage()) {
    err = "damaged record in " + path;
    return false;
  }
  return true;
}

bool replayTableLog(const std::string& path, uint64_t maxCommitted, std::map<long long, RowState>& index,
                    ScanInfo& info, std::string& err) {
  LogReader rd;
  if (!rd.open(path, err)) {
    if (!fileExists(path)) {
      err.clear();
      return true;
    }
    return false;
  }
  rd.setVerify(true);
  RecView v;
  const uint8_t* body = nullptr;
  size_t bodyLen = 0;
  while (rd.next(v, body, bodyLen)) {
    if (v.xid > info.maxXid) info.maxXid = v.xid;
    if (v.xid > maxCommitted) continue;  // this transaction never reached its commit record
    info.committedEnd = v.payloadOff + v.payloadLen;
    if (v.op == 'D') {
      index.erase(v.pk);
      continue;
    }
    RowState s;
    s.pos.off = v.payloadOff;
    s.pos.len = v.payloadLen;
    index[v.pk] = s;
  }
  if (rd.stoppedOnDamage()) {
    err = "damaged record in " + path;
    return false;
  }
  return true;
}

AppendLog::~AppendLog() { close(); }

bool AppendLog::open(const std::string& path, uint64_t limit, std::string& err) {
  FILE* created = std::fopen(path.c_str(), "ab");
  if (!created) {
    err = "cannot open log " + path;
    return false;
  }
  std::fclose(created);
  fp_ = std::fopen(path.c_str(), "r+b");
  if (!fp_) {
    err = "cannot open log " + path;
    return false;
  }
  // Drop a torn tail, plus any intact records recovery refused to apply, so a
  // later restart cannot mistake them for committed ones. Recovery already
  // verified every record it accepted, so its committed end is authoritative.
  seek64(fp_, 0, SEEK_END);
  uint64_t size = tell64(fp_);
  pos_ = (limit == UINT64_MAX) ? intactEnd(fp_) : (limit < size ? limit : size);
  if (size > pos_ && !truncateTo(fp_, pos_)) {
    err = "cannot truncate " + path;
    return false;
  }
  seek64(fp_, pos_, SEEK_SET);
  return true;
}

RecPos AppendLog::append(char op, uint64_t xid, long long pk, const std::string& body) {
  std::string payload;
  payload.reserve(kPrefixSize + body.size());
  payload.push_back(static_cast<char>(op));
  for (int k = 0; k < 8; ++k) payload.push_back(static_cast<char>((xid >> (8 * k)) & 0xFF));
  for (int k = 0; k < 8; ++k) payload.push_back(static_cast<char>((static_cast<uint64_t>(pk) >> (8 * k)) & 0xFF));
  payload += body;

  std::string rec;
  rec.reserve(kHeaderSize + payload.size());
  putU32(rec, static_cast<uint32_t>(payload.size()));
  putU32(rec, crc32Of(payload.data(), payload.size()));
  rec += payload;

  RecPos at;
  at.off = pos_ + kHeaderSize;
  at.len = static_cast<uint32_t>(payload.size());
  if (seek64(fp_, pos_, SEEK_SET) != 0 || std::fwrite(rec.data(), 1, rec.size(), fp_) != rec.size()) failed_ = true;
  pos_ += rec.size();
  dirty_ = true;
  return at;
}

bool AppendLog::flush(std::string& err) const {
  if (!fp_ || !dirty_) return true;
  if (failed_ || std::fflush(fp_) != 0) {
    err = "cannot flush log";
    return false;
  }
  dirty_ = false;
  return true;
}

bool AppendLog::sync(std::string& err) {
  if (!fp_) return true;
  if (failed_ || std::fflush(fp_) != 0 || !syncFile(fp_)) {
    err = "cannot fsync log";
    return false;
  }
  dirty_ = false;
  return true;
}

bool AppendLog::truncateBack(uint64_t size, std::string& err) {
  if (!fp_ || size >= pos_) return true;
  // Buffered bytes past the new end must reach the OS before it can cut them.
  if (std::fflush(fp_) != 0 || !truncateTo(fp_, size)) {
    err = "cannot truncate log";
    return false;
  }
  pos_ = size;
  failed_ = false;
  dirty_ = false;
  seek64(fp_, pos_, SEEK_SET);
  return true;
}

void AppendLog::close() {
  if (fp_) {
    std::fclose(fp_);
    fp_ = nullptr;
  }
}

LogReader::~LogReader() { close(); }

bool LogReader::open(const std::string& path, std::string& err) {
  fp_ = std::fopen(path.c_str(), "rb");
  if (!fp_) {
    err = "cannot read log " + path;
    return false;
  }
  buf_.assign(kChunkBytes, 0);
  bufOff_ = 0;
  bufLen_ = 0;
  pos_ = 0;
  end_ = UINT64_MAX;
  verify_ = false;
  damaged_ = false;
  return true;
}

bool LogReader::fill(uint64_t off, size_t want) {
  if (want > buf_.size()) buf_.resize(want);
  if (bufLen_ >= want && bufOff_ <= off && off + want <= bufOff_ + bufLen_) return true;
  bufOff_ = off;
  if (seek64(fp_, off, SEEK_SET) != 0) {
    bufLen_ = 0;
    return false;
  }
  bufLen_ = std::fread(buf_.data(), 1, buf_.size(), fp_);
  return bufLen_ >= want;
}

bool LogReader::next(RecView& v, const uint8_t*& body, size_t& bodyLen) {
  if (!fp_ || pos_ + kHeaderSize > end_) return false;
  if (!fill(pos_, kHeaderSize)) return false;
  const uint8_t* h = buf_.data() + (pos_ - bufOff_);
  uint32_t len = getU32(h);
  uint32_t crc = getU32(h + 4);
  if (len < kPrefixSize || len > kMaxRecord) return false;
  uint64_t payloadOff = pos_ + kHeaderSize;
  if (payloadOff + len > end_) return false;
  if (!fill(payloadOff, len)) return false;
  const uint8_t* p = buf_.data() + (payloadOff - bufOff_);
  if (verify_ && crc32Of(p, len) != crc) {
    damaged_ = true;
    return false;
  }
  v.op = static_cast<char>(p[0]);
  v.xid = static_cast<uint64_t>(getI64(p + 1));
  v.pk = getI64(p + 9);
  v.payloadOff = payloadOff;
  v.payloadLen = len;
  body = p + kPrefixSize;
  bodyLen = static_cast<size_t>(len) - kPrefixSize;
  pos_ = payloadOff + len;
  return true;
}

void LogReader::close() {
  if (fp_) {
    std::fclose(fp_);
    fp_ = nullptr;
  }
  bufLen_ = 0;
}

}  // namespace mvdb
