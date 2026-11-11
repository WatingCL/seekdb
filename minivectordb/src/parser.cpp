#include "parser.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace mvdb {

namespace {

bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool isIdentChar(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

struct P {
  const std::string& s;
  size_t p = 0;
  ParseResult r;

  explicit P(const std::string& src) : s(src) {}

  bool fail(const std::string& code, const std::string& msg) {
    r.ok = false;
    r.code = code;
    r.msg = msg;
    return false;
  }
  void ws() {
    while (p < s.size() && isSpace(s[p])) ++p;
  }
  bool eof() {
    ws();
    return p >= s.size();
  }
  bool wordAt(size_t at, const char* kw) const {
    size_t n = strlen(kw);
    if (at + n > s.size()) return false;
    for (size_t k = 0; k < n; ++k) {
      char a = s[at + k];
      if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
      if (a != kw[k]) return false;
    }
    if (at + n < s.size() && isIdentChar(s[at + n])) return false;
    return true;
  }
  bool peekKw(const char* kw) {
    ws();
    return wordAt(p, kw);
  }
  bool acceptKw(const char* kw) {
    ws();
    if (!wordAt(p, kw)) return false;
    p += strlen(kw);
    return true;
  }
  bool acceptChar(char c) {
    ws();
    if (p < s.size() && s[p] == c) {
      ++p;
      return true;
    }
    return false;
  }
  bool expectChar(char c, const char* what) {
    if (!acceptChar(c)) return fail("syntax_error", std::string("expected ") + what);
    return true;
  }
  bool readIdent(std::string& out) {
    ws();
    if (p >= s.size() || !isIdentStart(s[p])) return fail("syntax_error", "expected identifier");
    size_t start = p;
    while (p < s.size() && isIdentChar(s[p])) ++p;
    out = s.substr(start, p - start);
    return true;
  }
  bool readStringLit(std::string& out) {
    ws();
    if (p >= s.size() || s[p] != '\'') return fail("syntax_error", "expected string literal");
    ++p;
    while (p < s.size()) {
      char c = s[p];
      if (c == '\'') {
        if (p + 1 < s.size() && s[p + 1] == '\'') {
          out.push_back('\'');
          p += 2;
          continue;
        }
        ++p;
        return true;
      }
      if (c == '\\') {
        ++p;
        if (p >= s.size()) return fail("syntax_error", "unterminated string");
        char e = s[p++];
        switch (e) {
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case '0': out.push_back('\0'); break;
          default: out.push_back(e); break;
        }
        continue;
      }
      out.push_back(c);
      ++p;
    }
    return fail("syntax_error", "unterminated string");
  }
  bool readNumber(Value& out) {
    ws();
    size_t start = p;
    if (p < s.size() && (s[p] == '-' || s[p] == '+')) ++p;
    bool digits = false;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
      ++p;
      digits = true;
    }
    bool isFloat = false;
    if (p < s.size() && s[p] == '.') {
      isFloat = true;
      ++p;
      while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
        ++p;
        digits = true;
      }
    }
    if (!digits) return fail("syntax_error", "expected number");
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
      size_t save = p;
      ++p;
      if (p < s.size() && (s[p] == '-' || s[p] == '+')) ++p;
      if (p < s.size() && s[p] >= '0' && s[p] <= '9') {
        isFloat = true;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
      } else {
        p = save;
      }
    }
    std::string tok = s.substr(start, p - start);
    if (isFloat) {
      out.k = VK::Dbl;
      out.d = strtod(tok.c_str(), nullptr);
    } else {
      char* endp = nullptr;
      errno = 0;
      long long v = strtoll(tok.c_str(), &endp, 10);
      if (errno == ERANGE || endp != tok.c_str() + tok.size()) {
        out.k = VK::Dbl;
        out.d = strtod(tok.c_str(), nullptr);
      } else {
        out.k = VK::Int;
        out.i = v;
      }
    }
    return true;
  }
  bool readLiteral(Value& out) {
    ws();
    if (p < s.size() && s[p] == '\'') return readStringLit(out.s), out.k = VK::Str, true;
    if (peekKw("NULL") || peekKw("TRUE") || peekKw("FALSE")) {
      return fail("unsupported_sql", "NULL/boolean literals are not supported");
    }
    return readNumber(out);
  }

  bool endOfStatement() {
    ws();
    if (p < s.size() && s[p] == ';') ++p;
    if (!eof()) return fail("syntax_error", "trailing tokens after statement");
    return true;
  }

  bool parseCondList(std::vector<Cond>& out) {
    while (true) {
      Cond c;
      Value lhs, rhs;
      std::string lhsCol, rhsCol;
      bool lhsIsCol = false, rhsIsCol = false;
      ws();
      if (p < s.size() && isIdentStart(s[p]) && !peekKw("AND")) {
        if (!readIdent(lhsCol)) return false;
        lhsIsCol = true;
      } else {
        if (!readLiteral(lhs)) return false;
      }
      CmpOp op;
      ws();
      if (acceptChar('=')) {
        op = CmpOp::Eq;
      } else if (p + 1 < s.size() && s[p] == '<' && s[p + 1] == '=') {
        p += 2;
        op = CmpOp::Le;
      } else if (p + 1 < s.size() && s[p] == '>' && s[p + 1] == '=') {
        p += 2;
        op = CmpOp::Ge;
      } else if (acceptChar('<')) {
        op = CmpOp::Lt;
      } else if (acceptChar('>')) {
        op = CmpOp::Gt;
      } else if (peekKw("!=") || (p + 1 < s.size() && s[p] == '!' && s[p + 1] == '=')) {
        return fail("unsupported_sql", "inequality operator is not supported");
      } else if (peekKw("LIKE") || peekKw("IN") || peekKw("BETWEEN") || peekKw("IS")) {
        return fail("unsupported_sql", "operator not supported");
      } else {
        return fail("syntax_error", "expected comparison operator");
      }
      ws();
      if (p < s.size() && isIdentStart(s[p]) && !peekKw("AND")) {
        if (!readIdent(rhsCol)) return false;
        rhsIsCol = true;
      } else {
        if (!readLiteral(rhs)) return false;
      }
      if (lhsIsCol && rhsIsCol) return fail("unsupported_sql", "column-to-column comparison is not supported");
      if (!lhsIsCol && !rhsIsCol) return fail("syntax_error", "condition needs a column side");
      if (lhsIsCol) {
        c.col = lhsCol;
        c.op = op;
        c.lit = rhs;
      } else {
        c.col = rhsCol;
        c.lit = lhs;
        switch (op) {
          case CmpOp::Eq: c.op = CmpOp::Eq; break;
          case CmpOp::Lt: c.op = CmpOp::Gt; break;
          case CmpOp::Le: c.op = CmpOp::Ge; break;
          case CmpOp::Gt: c.op = CmpOp::Lt; break;
          case CmpOp::Ge: c.op = CmpOp::Le; break;
        }
      }
      out.push_back(c);
      if (acceptKw("AND")) continue;
      if (peekKw("OR")) return fail("unsupported_sql", "OR is not supported");
      return true;
    }
  }

  bool parseCreateTable(Stmt& st) {
    st.k = Stmt::K::CreateTable;
    if (!readIdent(st.table)) return false;
    if (!expectChar('(', "(")) return false;
    bool pkSeen = false;
    while (true) {
      ws();
      if (peekKw("PRIMARY")) {
        acceptKw("PRIMARY");
        if (!acceptKw("KEY")) return fail("syntax_error", "expected KEY");
        if (!expectChar('(', "(")) return false;
        std::string pkCol;
        if (!readIdent(pkCol)) return false;
        if (!expectChar(')', ")")) return false;
        for (auto& c : st.cols)
          if (c.name == pkCol) c.pk = true;
        pkSeen = true;
      } else if (peekKw("FOREIGN") || peekKw("UNIQUE") || peekKw("CONSTRAINT") || peekKw("CHECK") ||
                 peekKw("KEY") || peekKw("INDEX")) {
        return fail("unsupported_sql", "table constraint not supported");
      } else {
        ColDef c;
        if (!readIdent(c.name)) return false;
        ws();
        if (acceptKw("BIGINT")) {
          c.type = ColType::Bigint;
        } else if (acceptKw("VARCHAR")) {
          c.type = ColType::Varchar;
          if (!expectChar('(', "(")) return false;
          Value n;
          if (!readNumber(n) || n.k != VK::Int || n.i < 1) return fail("invalid_schema", "bad VARCHAR length");
          c.n = n.i;
          if (!expectChar(')', ")")) return false;
        } else if (acceptKw("VECTOR")) {
          c.type = ColType::Vector;
          if (!expectChar('(', "(")) return false;
          Value n;
          if (!readNumber(n) || n.k != VK::Int || n.i < 1) return fail("invalid_schema", "bad VECTOR dimension");
          c.n = n.i;
          if (!expectChar(')', ")")) return false;
        } else {
          return fail("unsupported_sql", "column type not supported");
        }
        while (true) {
          ws();
          if (peekKw("PRIMARY")) {
            acceptKw("PRIMARY");
            if (!acceptKw("KEY")) return fail("syntax_error", "expected KEY");
            c.pk = true;
            pkSeen = true;
          } else if (peekKw("NOT") || peekKw("NULL") || peekKw("DEFAULT") || peekKw("UNIQUE") ||
                     peekKw("AUTO_INCREMENT") || peekKw("REFERENCES") || peekKw("COMMENT") ||
                     peekKw("CHECK") || peekKw("ON")) {
            return fail("unsupported_sql", "column constraint not supported");
          } else {
            break;
          }
        }
        for (const auto& o : st.cols)
          if (o.name == c.name) return fail("invalid_schema", "duplicate column name");
        st.cols.push_back(c);
      }
      if (acceptChar(',')) continue;
      if (!expectChar(')', ")")) return false;
      break;
    }
    int pkCount = 0;
    int vecCount = 0;
    int pkIdx = -1;
    for (size_t k = 0; k < st.cols.size(); ++k) {
      if (st.cols[k].pk) {
        ++pkCount;
        pkIdx = static_cast<int>(k);
      }
      if (st.cols[k].type == ColType::Vector) ++vecCount;
    }
    if (!pkSeen || pkCount != 1 || pkIdx < 0 || st.cols[pkIdx].type != ColType::Bigint)
      return fail("invalid_schema", "exactly one BIGINT PRIMARY KEY is required");
    if (vecCount > 1) return fail("invalid_schema", "at most one VECTOR column is allowed");
    return true;
  }

  bool parseCreateVectorIndex(Stmt& st) {
    st.k = Stmt::K::CreateVectorIndex;
    if (!acceptKw("INDEX")) return fail("syntax_error", "expected INDEX");
    if (!readIdent(st.name)) return false;
    if (!acceptKw("ON")) return fail("syntax_error", "expected ON");
    if (!readIdent(st.table)) return false;
    if (!expectChar('(', "(")) return false;
    if (!readIdent(st.col)) return false;
    if (!expectChar(')', ")")) return false;
    return true;
  }

  bool parseInsert(Stmt& st) {
    st.k = Stmt::K::Insert;
    if (!acceptKw("INTO")) return fail("syntax_error", "expected INTO");
    if (!readIdent(st.table)) return false;
    ws();
    if (p < s.size() && s[p] == '(') {
      ++p;
      while (true) {
        std::string c;
        if (!readIdent(c)) return false;
        st.insertCols.push_back(c);
        if (acceptChar(',')) continue;
        if (!expectChar(')', ")")) return false;
        break;
      }
    }
    if (!acceptKw("VALUES")) return fail("syntax_error", "expected VALUES");
    while (true) {
      if (!expectChar('(', "(")) return false;
      std::vector<Value> row;
      while (true) {
        Value v;
        if (!readLiteral(v)) return false;
        row.push_back(v);
        if (acceptChar(',')) continue;
        if (!expectChar(')', ")")) return false;
        break;
      }
      st.insertRows.push_back(std::move(row));
      if (acceptChar(',')) continue;
      return true;
    }
  }

  bool parseSelect(Stmt& st) {
    st.k = Stmt::K::Select;
    if (peekKw("DISTINCT") || peekKw("ALL")) return fail("unsupported_sql", "DISTINCT is not supported");
    while (true) {
      ws();
      if (acceptChar('*')) {
        SelItem it;
        it.k = SelItem::K::Star;
        st.items.push_back(it);
      } else if (peekKw("L2_DISTANCE")) {
        acceptKw("L2_DISTANCE");
        if (!expectChar('(', "(")) return false;
        SelItem it;
        it.k = SelItem::K::Dist;
        if (!readIdent(it.name)) return false;
        if (!expectChar(',', ",")) return false;
        std::string lit;
        if (!readStringLit(lit)) return false;
        std::string verr;
        if (!parseVectorLiteral(lit, it.q, verr)) return fail("invalid_vector", verr);
        if (!expectChar(')', ")")) return false;
        if (acceptKw("AS")) {
          if (!readIdent(it.alias)) return false;
        }
        st.items.push_back(it);
      } else if (isIdentStart(s[p])) {
        if (peekKw("COUNT") || peekKw("SUM") || peekKw("AVG") || peekKw("MIN") || peekKw("MAX"))
          return fail("unsupported_sql", "aggregates are not supported");
        SelItem it;
        it.k = SelItem::K::Col;
        if (!readIdent(it.name)) return false;
        if (acceptKw("AS")) {
          if (!readIdent(it.alias)) return false;
        }
        st.items.push_back(it);
      } else {
        return fail("syntax_error", "bad select item");
      }
      if (acceptChar(',')) continue;
      break;
    }
    if (!acceptKw("FROM")) return fail("syntax_error", "expected FROM");
    if (!readIdent(st.table)) return false;
    if (peekKw("JOIN") || peekKw("INNER") || peekKw("LEFT") || peekKw("RIGHT") || peekKw("CROSS"))
      return fail("unsupported_sql", "JOIN is not supported");
    if (acceptKw("WHERE")) {
      if (!parseCondList(st.where)) return false;
    }
    if (peekKw("GROUP") || peekKw("HAVING") || peekKw("UNION") || peekKw("WINDOW"))
      return fail("unsupported_sql", "clause not supported");
    if (acceptKw("ORDER")) {
      if (!acceptKw("BY")) return fail("syntax_error", "expected BY");
      while (true) {
        OrderKey ok;
        if (!readIdent(ok.ref)) return false;
        if (acceptKw("DESC")) {
          ok.desc = true;
        } else {
          acceptKw("ASC");
        }
        st.order.push_back(ok);
        if (acceptChar(',')) continue;
        break;
      }
    }
    if (acceptKw("LIMIT")) {
      Value n;
      if (!readNumber(n) || n.k != VK::Int || n.i < 0) return fail("syntax_error", "bad LIMIT");
      st.limit = n.i;
      ws();
      if (p < s.size() && (s[p] == ',' || peekKw("OFFSET")))
        return fail("unsupported_sql", "LIMIT offset is not supported");
    }
    if (peekKw("GROUP") || peekKw("HAVING") || peekKw("UNION") || peekKw("FOR") || peekKw("LOCK") ||
        peekKw("JOIN"))
      return fail("unsupported_sql", "clause not supported");
    return true;
  }

  bool parseUpdate(Stmt& st) {
    st.k = Stmt::K::Update;
    if (!readIdent(st.table)) return false;
    if (!acceptKw("SET")) return fail("syntax_error", "expected SET");
    while (true) {
      std::string col;
      if (!readIdent(col)) return false;
      if (!expectChar('=', "=")) return false;
      Value v;
      if (!readLiteral(v)) return false;
      st.sets.emplace_back(col, v);
      if (acceptChar(',')) continue;
      break;
    }
    if (acceptKw("WHERE")) {
      if (!parseCondList(st.where)) return false;
    }
    return true;
  }

  bool parseDelete(Stmt& st) {
    st.k = Stmt::K::Delete;
    if (!acceptKw("FROM")) return fail("syntax_error", "expected FROM");
    if (!readIdent(st.table)) return false;
    if (acceptKw("WHERE")) {
      if (!parseCondList(st.where)) return false;
    }
    return true;
  }
};

const char* kUnsupportedWords[] = {
    "ALTER",   "DROP",     "TRUNCATE", "RENAME",   "GRANT",    "REVOKE",  "SHOW",    "DESCRIBE",
    "EXPLAIN", "USE",      "SET",      "SAVEPOINT", "RELEASE", "LOCK",    "UNLOCK",  "ANALYZE",
    "OPTIMIZE", "CALL",    "PREPARE",  "EXECUTE",  "DEALLOCATE", "WITH",  "MERGE",   "REPLACE",
    "START",   "HANDLER",  "PURGE",    "RESET",    "FLUSH",    "LOAD",    "IMPORT",  "EXPORT",
    "SELECT",  nullptr};

}  // namespace

ParseResult parseSql(const std::string& sql) {
  P ps(sql);
  ps.r.ok = true;
  if (ps.eof()) return ps.fail("syntax_error", "empty statement"), ps.r;
  Stmt& st = ps.r.stmt;
  bool parsed = false;
  if (ps.acceptKw("CREATE")) {
    if (ps.acceptKw("TABLE")) {
      parsed = ps.parseCreateTable(st);
    } else if (ps.acceptKw("VECTOR")) {
      parsed = ps.parseCreateVectorIndex(st);
    } else {
      parsed = ps.fail("unsupported_sql", "CREATE form not supported");
    }
  } else if (ps.acceptKw("INSERT")) {
    parsed = ps.parseInsert(st);
  } else if (ps.acceptKw("SELECT")) {
    parsed = ps.parseSelect(st);
  } else if (ps.acceptKw("UPDATE")) {
    parsed = ps.parseUpdate(st);
  } else if (ps.acceptKw("DELETE")) {
    parsed = ps.parseDelete(st);
  } else if (ps.acceptKw("BEGIN")) {
    st.k = Stmt::K::Begin;
    ps.acceptKw("WORK");
    parsed = true;
  } else if (ps.acceptKw("COMMIT")) {
    st.k = Stmt::K::Commit;
    ps.acceptKw("WORK");
    parsed = true;
  } else if (ps.acceptKw("ROLLBACK")) {
    st.k = Stmt::K::Rollback;
    ps.acceptKw("WORK");
    parsed = true;
  } else {
    ps.ws();
    for (int k = 0; kUnsupportedWords[k]; ++k) {
      if (ps.peekKw(kUnsupportedWords[k])) {
        return ps.fail("unsupported_sql", std::string("statement not supported: ") + kUnsupportedWords[k]), ps.r;
      }
    }
    parsed = ps.fail("syntax_error", "unrecognized statement");
  }
  if (!parsed) return ps.r;
  if (!ps.endOfStatement()) return ps.r;
  ps.r.ok = true;
  return ps.r;
}

}  // namespace mvdb
