/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_sql/csv_writer.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
using namespace oceanbase::sql::file_sql;
namespace file_csv_writer_test
{
class TemporaryDirectory
{
public:
  TemporaryDirectory() { char name[] = "/tmp/file-copy-XXXXXX"; const char *p = mkdtemp(name); if (p) path = p; }
  ~TemporaryDirectory() { if (!path.empty()) { std::error_code error; std::filesystem::remove_all(path, error); } }
  std::string path;
};
}

TEST(FileSqlCsvWriter, RoundTripNoReplaceAndTemporaryOwnership) {
file_csv_writer_test::TemporaryDirectory temporary;
ASSERT_FALSE(temporary.path.empty());
const std::string &dir=temporary.path;Options o;o.path=std::string(dir)+"/out.csv";o.nullstr="NULL";{CsvWriter w;ASSERT_TRUE(w.open(o,dir));ASSERT_TRUE(w.row({Cell("id"),Cell("note")}));ASSERT_TRUE(w.row({Cell("1"),Cell("hello,\n\"中\"")}));ASSERT_TRUE(w.row({Cell("2"),Cell()}));ASSERT_TRUE(w.finish());}{CsvWriter w;ASSERT_TRUE(!w.open(o,dir));}Reader r;ASSERT_TRUE(r.open(o,dir));std::vector<Cell> row;bool end;ASSERT_TRUE(r.next(row,end)&&row[1].text=="hello,\n\"中\"");ASSERT_TRUE(r.next(row,end)&&row[1].null_value);o.path=std::string(dir)+"/failed.csv";{CsvWriter w;ASSERT_TRUE(w.open(o,dir));ASSERT_TRUE(w.row({Cell("partial")}));}ASSERT_TRUE(access(o.path.c_str(),F_OK));
// A collision must never cause cleanup to unlink another writer's file.
std::vector<std::string> collisions;
for(int i=0;i<201;++i){std::string p=std::string(dir)+"/.seekdb-copy-"+std::to_string(getpid())+"-"+std::to_string(i);std::ofstream f(p);f<<"owned by someone else";collisions.push_back(p);}
o.path=std::string(dir)+"/collision.csv";
{CsvWriter w;w.open(o,dir);}
for(const auto &p:collisions){std::ifstream f(p);std::string value;std::getline(f,value);ASSERT_TRUE(value=="owned by someone else");unlink(p.c_str());}
unlink((std::string(dir)+"/out.csv").c_str());rmdir(dir.c_str());}
