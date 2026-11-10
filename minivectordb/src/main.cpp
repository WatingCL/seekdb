#include "engine.hpp"
#include "json.hpp"
#include "parser.hpp"

#include <iostream>
#include <string>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace {

void logErr(const std::string& msg) { std::cerr << "[minivectordb] " << msg << std::endl; }

bool makeDirOne(const std::string& p) {
#ifdef _WIN32
  return _mkdir(p.c_str()) == 0;
#else
  return mkdir(p.c_str(), 0755) == 0;
#endif
}

bool dirExists(const std::string& p) {
#ifdef _WIN32
  DWORD attr = GetFileAttributesA(p.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool createDirectories(const std::string& p, std::string& err) {
  if (p.empty() || dirExists(p)) return true;
  size_t slash = p.find_last_of("/\\");
  if (slash != std::string::npos && slash + 1 < p.size() && slash > 0) {
    if (!createDirectories(p.substr(0, slash), err)) return false;
  }
  if (makeDirOne(p) || dirExists(p)) return true;
  err = "cannot create directory " + p;
  return false;
}

void sendLine(const mvdb::Json& j) {
  std::cout << j.dump() << "\n";
  std::cout.flush();
}

mvdb::Json boolean(bool v) {
  mvdb::Json j;
  j.t = mvdb::JType::Bool;
  j.b = v;
  return j;
}

mvdb::Json errResponse(const mvdb::Json& id, const std::string& code, const std::string& message) {
  mvdb::Json resp = mvdb::Json::object();
  resp.set("id", id);
  resp.set("ok", boolean(false));
  mvdb::Json err = mvdb::Json::object();
  err.set("code", mvdb::Json::str(code));
  err.set("message", mvdb::Json::str(message));
  resp.set("error", err);
  return resp;
}

mvdb::Json okResponse(const mvdb::Json& id) {
  mvdb::Json resp = mvdb::Json::object();
  resp.set("id", id);
  resp.set("ok", boolean(true));
  return resp;
}

mvdb::Json execResponse(const mvdb::Json& id, const mvdb::ExecResult& res) {
  if (!res.ok) return errResponse(id, res.code, res.msg);
  mvdb::Json resp = okResponse(id);
  if (res.resultSet) {
    mvdb::Json cols = mvdb::Json::array();
    for (const auto& c : res.columns) cols.arr.push_back(mvdb::Json::str(c));
    resp.set("columns", cols);
    mvdb::Json rows = mvdb::Json::array();
    for (const auto& r : res.rows) {
      mvdb::Json row = mvdb::Json::array();
      for (const auto& v : r) row.arr.push_back(v);
      rows.arr.push_back(row);
    }
    resp.set("rows", rows);
  } else if (!res.plain) {
    resp.set("affected_rows", mvdb::Json::integer(res.affected));
  }
  return resp;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  std::ios::sync_with_stdio(false);

  std::string dataDir = "/data";
  for (int k = 1; k < argc; ++k) {
    std::string a = argv[k];
    if (a == "--data-dir" && k + 1 < argc) dataDir = argv[++k];
  }

  std::string dirErr;
  if (!createDirectories(dataDir, dirErr)) {
    logErr(dirErr);
    return 1;
  }

  // Recovery runs before the handshake, so a driver that sends its first
  // request as soon as it sees ready is never answered from a half-built index.
  mvdb::Engine engine(dataDir);

  std::cout << "{\"type\":\"ready\"}\n";
  std::cout.flush();

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    mvdb::Json req;
    std::string perr;
    if (!mvdb::Json::parse(line, req, perr) || req.t != mvdb::JType::Object) {
      logErr("bad request line: " + perr);
      sendLine(errResponse(mvdb::Json(), "invalid_request", "request is not a JSON object"));
      continue;
    }
    const mvdb::Json* idp = req.find("id");
    mvdb::Json id = idp ? *idp : mvdb::Json();
    const mvdb::Json* opp = req.find("op");
    std::string op = (opp && opp->t == mvdb::JType::String) ? opp->s : std::string();

    if (op == "shutdown") {
      std::string serr;
      if (!engine.shutdown(serr)) {
        logErr("shutdown persistence failed: " + serr);
        sendLine(errResponse(id, "storage_error", serr));
        continue;
      }
      sendLine(okResponse(id));
      return 0;
    }
    if (op == "execute") {
      const mvdb::Json* sqlp = req.find("sql");
      if (!sqlp || sqlp->t != mvdb::JType::String) {
        sendLine(errResponse(id, "invalid_request", "execute needs a string sql field"));
        continue;
      }
      const mvdb::Json* modep = req.find("search_mode");
      std::string mode = (modep && modep->t == mvdb::JType::String) ? modep->s : "exact";
      mvdb::ParseResult pr = mvdb::parseSql(sqlp->s);
      if (!pr.ok) {
        sendLine(errResponse(id, pr.code, pr.msg));
        continue;
      }
      sendLine(execResponse(id, engine.exec(pr.stmt, mode)));
      continue;
    }
    sendLine(errResponse(id, "unsupported_op", "unknown op: " + op));
  }
  return 0;
}
