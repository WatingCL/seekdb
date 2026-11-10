/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// Standalone regression for the streaming reader; no database runtime required.
#include "sql/engine/basic/file_sql/file_reader.h"
#include <cassert>
#include <fstream>
using namespace oceanbase::sql::file_sql;
static void write(const std::string &path, const std::string &data) {
  std::ofstream f(path); f << data; f.close();
}
int main(int argc,char **argv) {
  char dir[] = "/tmp/file-reader-XXXXXX"; assert(mkdtemp(dir));
  std::string path=std::string(dir)+"/test.csv";
  write(path,"id,id,note,day\r\n1,2,\"hello,\nworld\",2024-02-29\r\n3,4,\"say \"\"hi\"\"\",\r\n");
  Reader reader; Options o; o.path=path;
  assert(reader.open(o,dir));assert(reader.columns.size()==4);assert(reader.columns[1].name=="id__2");
  assert(reader.columns[3].type==Type::DATE&&reader.columns[3].nullable);
  std::vector<Cell> row;bool end;assert(reader.next(row,end)&&!end&&row[2].text=="hello,\nworld");
  assert(reader.next(row,end)&&!end&&row[2].text=="say \"hi\""&&row[3].null_value);
  assert(reader.next(row,end)&&end);assert(reader.rescan());assert(reader.next(row,end)&&row[0].text=="1");
  Reader decoded;assert(decode(encode(reader),decoded));assert(decoded.open(decoded.options,dir,false));
  write(path,"id,id,note,day\n9,8,changed,2024-01-01\n");assert(!reader.next(row,end));assert(!decoded.rescan());
  write(path,"a\n\"broken\n");assert(!reader.open(o,dir));
  path=std::string(dir)+"/test.jsonl";o.path=path;o.format="json";
  write(path,"{\"id\":1,\"name\":\"a\\u4e2d\"}\n{\"id\":2,\"score\":1.5}\n");
  assert(reader.open(o,dir));assert(reader.columns.size()==3);assert(reader.columns[1].nullable);
  assert(reader.next(row,end)&&row[1].text=="a中"&&row[2].null_value);
  assert(reader.next(row,end)&&row[1].null_value&&row[2].text=="1.5");
  write(path,"{\"x\":{\"nested\":1}}\n");assert(!reader.open(o,dir));
  assert(infer(Cell("9223372036854775808"))==Type::VARCHAR);
  assert(infer(Cell("2023-02-29"))==Type::VARCHAR);
  assert(widen(Type::DOUBLE,Type::BIGINT)==widen(Type::BIGINT,Type::DOUBLE));
  std::string resolved,error;assert(!canonical("/etc/passwd",dir,resolved,error));
  if(argc>1){
    Reader parquet;Options po;po.path=argv[1];po.format="parquet";
    assert(parquet.open(po,"/tmp"));int count=0,sum=0;
    while(true){assert(parquet.next(row,end));if(end)break;sum+=std::atoi(row[0].text.c_str());++count;}
    assert(count==7&&sum==21);assert(parquet.rescan());assert(parquet.next(row,end)&&row[0].text=="0");
  }
  std::string bom=std::string(dir)+"/bom.csv";
  write(bom, std::string("\xEF\xBB\xBF")+"id\n1\n");
  o.path=bom;o.format="csv";assert(reader.open(o,dir));assert(reader.columns[0].name=="id");
  assert(reader.next(row,end)&&!end&&row[0].text=="1");unlink(bom.c_str());
  std::string fifo=std::string(dir)+"/pipe.csv";assert(mkfifo(fifo.c_str(),0600)==0);
  o.path=fifo;o.format="csv";assert(!reader.open(o,dir));unlink(fifo.c_str());
  // A replaced ancestor is rejected even if the final component is regular.
  std::string sub=std::string(dir)+"/sub";assert(mkdir(sub.c_str(),0700)==0);
  std::string moved=std::string(dir)+"/moved";assert(rename(sub.c_str(),moved.c_str())==0);
  write(moved+"/test.csv", "a\n1\n");
  assert(symlink(moved.c_str(),sub.c_str())==0);
  assert(open_canonical(sub+"/test.csv",false)<0);
  unlink(sub.c_str());unlink((moved+"/test.csv").c_str());rmdir(moved.c_str());
  unlink(path.c_str());unlink((std::string(dir)+"/test.csv").c_str());rmdir(dir);
}
