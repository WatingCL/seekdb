#pragma once
#include <string>
#include <utility>
#include <vector>

namespace mvdb {

enum class JType { Null, Bool, Int, Double, String, Array, Object };

struct Json {
  JType t = JType::Null;
  bool b = false;
  long long i = 0;
  double d = 0.0;
  std::string s;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  static Json object();
  static Json array();
  static Json str(std::string v);
  static Json integer(long long v);
  static Json real(double v);
  static Json boolean(bool v);

  const Json* find(const std::string& key) const;
  void set(const std::string& key, Json v);

  std::string dump() const;
  static bool parse(const std::string& text, Json& out, std::string& err);

 private:
  void dumpTo(std::string& out) const;
};

}  // namespace mvdb
