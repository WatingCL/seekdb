#pragma once
#include <string>
#include <vector>

namespace mvdb {

enum class ColType { Bigint, Varchar, Vector };

struct ColDef {
  std::string name;
  ColType type = ColType::Bigint;
  long long n = 0;
  bool pk = false;
};

enum class VK { Int, Dbl, Str, Vec };

struct Value {
  VK k = VK::Int;
  long long i = 0;
  double d = 0.0;
  std::string s;
  std::vector<float> v;
};

enum class CmpOp { Eq, Lt, Le, Gt, Ge };

// Counts Unicode code points (UTF-8 continuation bytes excluded).
size_t unicodeLen(const std::string& s);

// Code-point order == unsigned byte order for UTF-8.
int cmpStr(const std::string& a, const std::string& b);

// Parses '[1, 2e-3, -4]' style literals; validates finite and float32-representable.
bool parseVectorLiteral(const std::string& text, std::vector<float>& out, std::string& err);

}  // namespace mvdb
