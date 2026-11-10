/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */
#pragma once

// This reader owns one read-only descriptor and one record, never all file rows.
// SQL evaluation, sorting, aggregation and joins remain in the native executor.
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <glob.h>
#include <limits.h>

namespace oceanbase { namespace sql { namespace file_sql {

enum class Type { NIL, BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE, DATETIME };
struct Cell {
  std::string text;
  bool null_value = true;
  bool json_string = false;
  Cell() {}
  explicit Cell(const std::string &v) : text(v), null_value(false) {}
};
struct Column {
  std::string name;
  Type type = Type::NIL;
  bool nullable = false;
};
struct Options {
  std::string path, format = "csv", nullstr;
  char delimiter = ',', quote = '"', escape = '"';
  bool header = true;
};
struct Snapshot {
  uint64_t dev = 0, inode = 0, size = 0, mtime = 0, ctime = 0;
  static Snapshot from(const struct stat &s) {
    Snapshot v;
    v.dev=s.st_dev; v.inode=s.st_ino; v.size=s.st_size;
#ifdef __APPLE__
    v.mtime = uint64_t(s.st_mtimespec.tv_sec)*1000000000ULL+s.st_mtimespec.tv_nsec;
    v.ctime = uint64_t(s.st_ctimespec.tv_sec)*1000000000ULL+s.st_ctimespec.tv_nsec;
#else
    v.mtime = uint64_t(s.st_mtim.tv_sec)*1000000000ULL+s.st_mtim.tv_nsec;
    v.ctime = uint64_t(s.st_ctim.tv_sec)*1000000000ULL+s.st_ctim.tv_nsec;
#endif
    return v;
  }
  bool operator==(const Snapshot &v) const {
    return dev==v.dev && inode==v.inode && size==v.size && mtime==v.mtime && ctime==v.ctime;
  }
};
inline std::string lower(std::string v) {
  for (char &c:v) if(c>='A'&&c<='Z') c += 'a'-'A';
  return v;
}
inline bool valid_utf8(const std::string &s) {
  for(size_t i=0;i<s.size();) {
    unsigned char c=s[i++]; if(c<0x80) continue;
    int n= c>=0xC2&&c<=0xDF?1:c>=0xE0&&c<=0xEF?2:c>=0xF0&&c<=0xF4?3:-1;
    if(n<0||i+n>s.size()) return false;
    uint32_t cp=c&((1U<<(6-n))-1);
    for(int j=0;j<n;++j) {unsigned char d=s[i++];if((d&0xC0)!=0x80)return false;cp=(cp<<6)|(d&63);}
    if((n==1&&cp<128)||(n==2&&cp<2048)||(n==3&&cp<65536)||cp>0x10FFFF||(cp>=0xD800&&cp<=0xDFFF))return false;
  }
  return true;
}
inline bool digits(const std::string &v,size_t at,size_t n) {
  if(at+n>v.size())return false;
  for(size_t i=at;i<at+n;++i)if(v[i]<'0'||v[i]>'9')return false;
  return true;
}
inline bool date(const std::string &v) {
  if(v.size()!=10||v[4]!='-'||v[7]!='-'||!digits(v,0,4)||!digits(v,5,2)||!digits(v,8,2))return false;
  int y=std::atoi(v.substr(0,4).c_str()),m=std::atoi(v.substr(5,2).c_str()),d=std::atoi(v.substr(8,2).c_str());
  static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
  return y>=1&&m>=1&&m<=12&&d>=1&&d<=days[m-1]+(m==2&&y%4==0&&(y%100!=0||y%400==0));
}
inline bool datetime(const std::string &v) {
  return v.size()==19&&date(v.substr(0,10))&&(v[10]==' '||v[10]=='T')&&v[13]==':'&&v[16]==':'&&
    digits(v,11,2)&&digits(v,14,2)&&digits(v,17,2)&&std::atoi(v.substr(11,2).c_str())<24&&
    std::atoi(v.substr(14,2).c_str())<60&&std::atoi(v.substr(17,2).c_str())<60;
}
inline Type infer(const Cell &v) {
  if(v.null_value)return Type::NIL;
  const std::string &s=v.text;
  if(v.json_string){if(date(s))return Type::DATE;if(datetime(s))return Type::DATETIME;return Type::VARCHAR;}
  if(s=="true"||s=="false")return Type::BOOLEAN;
  size_t i=(!s.empty()&&(s[0]=='-'||s[0]=='+'))?1:0;
  bool integer=i<s.size();for(size_t j=i;j<s.size();++j)integer&=s[j]>='0'&&s[j]<='9';
  if(integer){char *end=nullptr;errno=0;std::strtoll(s.c_str(),&end,10);if(errno!=ERANGE&&end&&!*end)return Type::BIGINT;return Type::VARCHAR;}
  if(!s.empty()&&(s.find('.')!=std::string::npos||s.find('e')!=std::string::npos||s.find('E')!=std::string::npos)) {
    char *end=nullptr;errno=0;double d=std::strtod(s.c_str(),&end);
    if(end==s.c_str()+s.size()&&errno!=ERANGE&&std::isfinite(d))return Type::DOUBLE;
  }
  if(date(s))return Type::DATE;
  if(datetime(s))return Type::DATETIME;
  return Type::VARCHAR;
}
inline Type widen(Type a,Type b) {
  if(a==Type::NIL)return b;if(b==Type::NIL||a==b)return a;
  if((a==Type::BIGINT&&b==Type::DOUBLE)||(a==Type::DOUBLE&&b==Type::BIGINT))return Type::DOUBLE;
  if((a==Type::DATE&&b==Type::DATETIME)||(a==Type::DATETIME&&b==Type::DATE))return Type::DATETIME;
  return Type::VARCHAR;
}
inline bool canonical(const std::string &path,const std::string &root,std::string &out,std::string &error) {
  char real[PATH_MAX],allowed[PATH_MAX];
  if(path.find('\0')!=std::string::npos||root.empty()||lower(root)=="null"||!::realpath(root.c_str(),allowed)||!::realpath(path.c_str(),real)){
    error="file SQL path is unavailable or secure_file_priv disallows access";return false;
  }
  std::string r(allowed);out=real;
  if(r!="/"&&out!=r&&(out.size()<=r.size()||out.compare(0,r.size(),r)!=0||out[r.size()]!='/')){
    error="file SQL path escapes secure_file_priv";return false;
  }
  return true;
}

#include "parquet_reader.h"

// Open a canonical absolute path without following any path component that
// may have been replaced with a symlink after the secure_file_priv check.
inline int open_canonical(const std::string &path, bool directory) {
  if (path.empty() || path[0] != '/') { errno = EINVAL; return -1; }
  int parent = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent < 0) return -1;
  size_t start = 1;
  while (start < path.size()) {
    const size_t slash = path.find('/', start);
    const bool last = slash == std::string::npos;
    const std::string component = path.substr(start, last ? path.size()-start : slash-start);
    if (component.empty() || component == "." || component == "..") {
      ::close(parent); errno = EINVAL; return -1;
    }
    const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                      ((!last || directory) ? O_DIRECTORY : O_NONBLOCK);
    const int child = ::openat(parent, component.c_str(), flags);
    const int saved_errno = errno;
    ::close(parent);
    if (child < 0) { errno = saved_errno; return -1; }
    parent = child;
    if (last) return parent;
    start = slash + 1;
  }
  return parent;
}

class FlatJson {
public:
  explicit FlatJson(const std::string &s):s_(s),p_(0){}
  bool parse(std::vector<std::pair<std::string,Cell>> &out,std::string &error) {
    ws();if(!take('{'))return fail(error);
    ws();if(take('}')){ws();return p_==s_.size();}
    std::set<std::string> keys;
    for(;;){
      std::string key;if(!string(key))return fail(error);
      if(!keys.insert(key).second){error="duplicate JSON field";return false;}
      ws();if(!take(':'))return fail(error);ws();Cell v;
      if(p_<s_.size()&&s_[p_]=='"'){if(!string(v.text))return fail(error);v.null_value=false;v.json_string=true;}
      else {
        size_t start=p_;while(p_<s_.size()&&s_[p_]!=','&&s_[p_]!='}'&&!(s_[p_]==' '||s_[p_]=='\t'||s_[p_]=='\r'||s_[p_]=='\n'))++p_;
        std::string token=s_.substr(start,p_-start);
        if(token=="null"){}else if(token=="true"||token=="false"){v=Cell(token);}
        else if(number(token)){v=Cell(token);}else return fail(error);
      }
      out.push_back(std::make_pair(key,v));ws();if(take('}'))break;if(!take(','))return fail(error);ws();
    }
    ws();if(p_!=s_.size())return fail(error);return true;
  }
private:
  void ws(){while(p_<s_.size()&&(s_[p_]==' '||s_[p_]=='\t'||s_[p_]=='\r'||s_[p_]=='\n'))++p_;}
  bool take(char c){if(p_<s_.size()&&s_[p_]==c){++p_;return true;}return false;}
  bool fail(std::string &e){e="invalid flat JSON object (nested values and arrays are unsupported), byte "+std::to_string(p_+1);return false;}
  static bool number(const std::string &s){
    size_t i=0;if(i<s.size()&&s[i]=='-')++i;if(i>=s.size())return false;
    if(s[i]=='0')++i;else {if(s[i]<'1'||s[i]>'9')return false;while(i<s.size()&&s[i]>='0'&&s[i]<='9')++i;}
    if(i<s.size()&&s[i]=='.'){++i;size_t a=i;while(i<s.size()&&s[i]>='0'&&s[i]<='9')++i;if(i==a)return false;}
    if(i<s.size()&&(s[i]=='e'||s[i]=='E')){++i;if(i<s.size()&&(s[i]=='+'||s[i]=='-'))++i;size_t a=i;while(i<s.size()&&s[i]>='0'&&s[i]<='9')++i;if(i==a)return false;}
    return i==s.size();
  }
  static void utf8(uint32_t cp,std::string &out){
    if(cp<128)out.push_back(cp);else if(cp<2048){out.push_back(0xC0|(cp>>6));out.push_back(0x80|(cp&63));}
    else if(cp<65536){out.push_back(0xE0|(cp>>12));out.push_back(0x80|((cp>>6)&63));out.push_back(0x80|(cp&63));}
    else{out.push_back(0xF0|(cp>>18));out.push_back(0x80|((cp>>12)&63));out.push_back(0x80|((cp>>6)&63));out.push_back(0x80|(cp&63));}
  }
  bool hex(uint32_t &cp){cp=0;for(int i=0;i<4;++i){if(p_>=s_.size())return false;char c=s_[p_++];int v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;if(v<0)return false;cp=(cp<<4)|v;}return true;}
  bool string(std::string &out){
    ws();if(!take('"'))return false;while(p_<s_.size()){
      unsigned char c=s_[p_++];if(c=='"')return valid_utf8(out);if(c<32)return false;
      if(c!='\\'){out.push_back(c);continue;}if(p_>=s_.size())return false;c=s_[p_++];
      if(c=='"'||c=='\\'||c=='/')out.push_back(c);
      else if(c=='b')out.push_back('\b');else if(c=='f')out.push_back('\f');else if(c=='n')out.push_back('\n');else if(c=='r')out.push_back('\r');else if(c=='t')out.push_back('\t');
      else if(c=='u'){uint32_t cp;if(!hex(cp))return false;if(cp>=0xD800&&cp<=0xDBFF){if(!take('\\')||!take('u'))return false;uint32_t lo;if(!hex(lo)||lo<0xDC00||lo>0xDFFF)return false;cp=65536+((cp-0xD800)<<10)+(lo-0xDC00);}else if(cp>=0xDC00&&cp<=0xDFFF)return false;utf8(cp,out);}
      else return false;
    }return false;
  }
  const std::string &s_;size_t p_;
};

class Reader {
public:
  static constexpr size_t MAX_RECORD=16*1024*1024,MAX_COLUMNS=1024,MAX_FILES=100000;
  Reader():file_(nullptr),record_(0),inferred_(false),glob_pos_(0){}
  ~Reader(){close();}
  Options options;
  Snapshot snapshot;
  std::vector<Column> columns;
  std::string error;
  bool open(const Options &opts,const std::string &root,bool infer_schema=true) {
    close();options=opts;root_=root;record_=0;error.clear();
    if(options.format=="glob")return open_glob();
    std::string resolved;if(!canonical(options.path,root_,resolved,error))return false;options.path=resolved;
    int fd=open_canonical(resolved,false);struct stat st,path_st;
    if(fd<0){error="cannot open read-only file";return false;}
    if(::fstat(fd,&st)||!S_ISREG(st.st_mode)||::stat(resolved.c_str(),&path_st)||st.st_ino!=path_st.st_ino||st.st_dev!=path_st.st_dev){::close(fd);error="not a stable regular file";return false;}
    Snapshot current=Snapshot::from(st);
    if(!infer_schema&&!(current==snapshot)){::close(fd);error="file changed since schema inference";return false;}
    snapshot=current;file_=::fdopen(fd,"rb");if(!file_){::close(fd);error="cannot create file stream";return false;}
    if(options.format!="csv"&&options.format!="json"&&options.format!="parquet"){error="unsupported file format";close();return false;}
    if(options.format=="parquet"){if(!parquet_.init(file_,columns)){error=parquet_.error;close();return false;}return true;}
    if(infer_schema){columns.clear();column_map_.clear();if(!infer_all()){close();return false;}}
    inferred_=true;return rewind_data();
  }
  bool next(std::vector<Cell> &row,bool &end) {
    end=false;row.clear();if(options.format=="glob"){
      if(glob_pos_>=files_.size()){end=true;return true;}row.push_back(Cell(files_[glob_pos_++]));return true;
    }
    if(!unchanged())return false;
    if(options.format=="parquet"){bool ok=parquet_.next(row,end);if(!ok)error=parquet_.error;return ok&&unchanged();}
    if(options.format=="csv"){
      if(!csv(row,end))return false;if(end)return unchanged();
      if(row.size()!=columns.size()){error="CSV field count differs from schema at record "+std::to_string(record_);return false;}
    }else{
      std::vector<std::pair<std::string,Cell>> fields;if(!json(fields,end))return false;if(end)return unchanged();
      row.resize(columns.size());for(const auto &field:fields){auto it=column_map_.find(field.first);if(it==column_map_.end()){error="JSON field appeared after schema inference";return false;}row[it->second]=field.second;}
    }
    return unchanged();
  }
  void project(const std::set<size_t> &ids){parquet_.projection=ids;parquet_.projection_set=true;}
  bool rescan(){if(options.format=="parquet")return unchanged()&&parquet_.rescan();if(options.format=="glob"){glob_pos_=0;return true;}return unchanged()&&rewind_data();}
  void close(){if(file_){std::fclose(file_);file_=nullptr;}files_.clear();glob_pos_=0;}
private:
  bool unchanged(){struct stat st,ps;if(!file_||::fstat(fileno(file_),&st)||::stat(options.path.c_str(),&ps)||!(Snapshot::from(st)==snapshot)||!(Snapshot::from(ps)==snapshot)){error="file changed during query";return false;}return true;}
  bool rewind_data(){if(!file_||::fseeko(file_,0,SEEK_SET)){error="cannot rewind file";return false;}record_=0;column_map_.clear();for(size_t i=0;i<columns.size();++i)column_map_[columns[i].name]=i;
    if(options.format=="csv"&&options.header){std::vector<Cell> header;bool end;if(!csv(header,end))return false;}return true;}
  bool open_glob(){
    // Validate the directory before filesystem enumeration and each result after it.
    size_t wildcard=options.path.find_first_of("*?[");size_t slash=options.path.rfind('/',wildcard==std::string::npos?options.path.size():wildcard);
    if(slash==std::string::npos){error="glob needs an explicit directory";return false;}std::string dir=options.path.substr(0,slash);if(dir.empty())dir="/";
    std::string resolved;if(!canonical(dir,root_,resolved,error))return false;
    glob_t matches{};int rc=::glob(options.path.c_str(),GLOB_NOSORT,nullptr,&matches);
    if(rc!=0&&rc!=GLOB_NOMATCH){::globfree(&matches);error="glob failed";return false;}
    if(matches.gl_pathc>MAX_FILES){::globfree(&matches);error="glob result limit exceeded";return false;}
    bool ok=true;for(size_t i=0;ok&&i<matches.gl_pathc;++i){std::string p;if(!canonical(matches.gl_pathv[i],root_,p,error)){ok=false;break;}struct stat s;if(!::stat(p.c_str(),&s)&&S_ISREG(s.st_mode))files_.push_back(p);}
    ::globfree(&matches);if(!ok)return false;std::sort(files_.begin(),files_.end());columns={{"file",Type::VARCHAR,false}};return true;
  }
  bool infer_all(){
    std::vector<Cell> row;bool end=false;size_t rows=0;
    if(options.format=="csv"){
      if(!csv(row,end))return false;if(end){error="empty CSV has no schema";return false;}
      if(row.size()>MAX_COLUMNS){error="column limit exceeded";return false;}
      std::set<std::string> names;for(size_t i=0;i<row.size();++i){std::string base=options.header?row[i].text:"column"+std::to_string(i+1);if(base.empty())base="column"+std::to_string(i+1);std::string name=base;int suffix=2;while(!names.insert(lower(name)).second)name=base+"__"+std::to_string(suffix++);columns.push_back({name,Type::NIL,false});}
      if(!options.header){for(size_t i=0;i<row.size();++i){columns[i].type=infer(row[i]);columns[i].nullable=row[i].null_value;}++rows;}
      while(!end){if(!csv(row,end))return false;if(end)break;if(row.size()!=columns.size()){error="CSV field count mismatch at record "+std::to_string(record_);return false;}for(size_t i=0;i<row.size();++i){columns[i].type=widen(columns[i].type,infer(row[i]));columns[i].nullable|=row[i].null_value;}++rows;}
    }else{
      while(!end){std::vector<std::pair<std::string,Cell>> fields;if(!json(fields,end))return false;if(end)break;std::set<size_t> found;
        for(const auto &field:fields){auto pos=column_map_.find(field.first);size_t i;if(pos==column_map_.end()){if(columns.size()>=MAX_COLUMNS){error="column limit exceeded";return false;}i=columns.size();column_map_[field.first]=i;columns.push_back({field.first,Type::NIL,rows>0});}else i=pos->second;found.insert(i);columns[i].type=widen(columns[i].type,infer(field.second));columns[i].nullable|=field.second.null_value;}
        for(size_t i=0;i<columns.size();++i)if(!found.count(i))columns[i].nullable=true;++rows;
      }if(columns.empty()){error="JSON file has no columns";return false;}
    }
    return unchanged();
  }
  bool csv(std::vector<Cell> &row,bool &end){
    if(record_==0&&ftello(file_)==0){unsigned char bom[3];size_t n=fread(bom,1,3,file_);if(n!=3||bom[0]!=0xEF||bom[1]!=0xBB||bom[2]!=0xBF)fseeko(file_,0,SEEK_SET);}
    row.clear();end=false;std::string field;bool quoted=false,after=false,was_quoted=false,any=false;size_t bytes=0;
    for(;;){int c=std::fgetc(file_);if(c!=EOF){any=true;if(++bytes>MAX_RECORD){error="CSV record limit exceeded";return false;}}
      if(c==EOF){if(std::ferror(file_)){error="CSV read error";return false;}if(quoted){error="unterminated CSV quote at record "+std::to_string(record_+1);return false;}if(!any&&row.empty()&&field.empty()){end=true;return true;}if(!valid_utf8(field)){error="invalid UTF-8 CSV field";return false;}row.push_back(make_cell(field,was_quoted));++record_;return true;}
      if(quoted){if(c==options.escape&&options.escape!=options.quote){int d=std::fgetc(file_);if(d==EOF){error="trailing CSV escape";return false;}field.push_back(d);++bytes;}
        else if(c==options.quote){int d=std::fgetc(file_);if(d==options.quote&&options.escape==options.quote){field.push_back(c);++bytes;}else{quoted=false;after=true;if(d!=EOF)std::ungetc(d,file_);}}
        else field.push_back(c);
      }else if(c==options.delimiter){if(!valid_utf8(field)){error="invalid UTF-8 CSV field";return false;}row.push_back(make_cell(field,was_quoted));if(row.size()>=MAX_COLUMNS){error="column limit exceeded";return false;}field.clear();after=false;was_quoted=false;}
      else if(c=='\n'||c=='\r'){if(c=='\r'){int d=std::fgetc(file_);if(d!='\n'&&d!=EOF)std::ungetc(d,file_);}if(!valid_utf8(field)){error="invalid UTF-8 CSV field";return false;}row.push_back(make_cell(field,was_quoted));++record_;return true;}
      else if(after){error="unexpected byte after CSV closing quote at record "+std::to_string(record_+1);return false;}
      else if(c==options.quote){if(!field.empty()){error="unexpected CSV quote";return false;}quoted=true;was_quoted=true;}
      else field.push_back(c);
    }
  }
  Cell make_cell(const std::string &v,bool quoted){return !quoted&&v==options.nullstr?Cell():Cell(v);}
  bool json(std::vector<std::pair<std::string,Cell>> &fields,bool &end){
    std::string line;end=false;for(;;){int c=std::fgetc(file_);if(c==EOF){if(std::ferror(file_)){error="JSONL read error";return false;}if(line.empty()){end=true;return true;}break;}if(c=='\n')break;line.push_back(c);if(line.size()>MAX_RECORD){error="JSONL record limit exceeded";return false;}}
    ++record_;if(!line.empty()&&line.back()=='\r')line.pop_back();if(!valid_utf8(line)){error="invalid UTF-8 JSONL";return false;}FlatJson parser(line);if(!parser.parse(fields,error)){error+=" at record "+std::to_string(record_);return false;}return true;
  }
  ParquetReader parquet_;
  FILE *file_;size_t record_;bool inferred_;std::string root_;
  std::map<std::string,size_t> column_map_;std::vector<std::string> files_;size_t glob_pos_;
  Reader(const Reader &)=delete;Reader &operator=(const Reader &)=delete;
};

// Length-delimited plan payload, independent of SQL literal escaping and NULs.
inline void put(std::string &out,const std::string &v){uint64_t n=v.size();for(int i=0;i<8;++i)out.push_back((n>>(8*i))&255);out+=v;}
inline bool get(const std::string &in,size_t &p,std::string &v){if(p+8>in.size())return false;uint64_t n=0;for(int i=0;i<8;++i)n|=uint64_t(static_cast<unsigned char>(in[p++]))<<(8*i);if(n>in.size()-p)return false;v=in.substr(p,n);p+=n;return true;}
inline std::string encode(const Reader &r){std::string s="\001FILESQL1";put(s,r.options.path);put(s,r.options.format);put(s,r.options.nullstr);put(s,std::string({r.options.delimiter,r.options.quote,r.options.escape,char(r.options.header)}));const Snapshot &v=r.snapshot;put(s,std::to_string(v.dev));put(s,std::to_string(v.inode));put(s,std::to_string(v.size));put(s,std::to_string(v.mtime));put(s,std::to_string(v.ctime));for(const Column &c:r.columns){put(s,c.name);put(s,std::to_string(int(c.type)));put(s,c.nullable?"1":"0");}return s;}
inline bool is_payload(const std::string &s){return s.size()>=9&&s.compare(0,9,"\001FILESQL1")==0;}
inline bool decode(const std::string &s,Reader &r){if(!is_payload(s))return false;size_t p=9;std::string chars,v;if(!get(s,p,r.options.path)||!get(s,p,r.options.format)||!get(s,p,r.options.nullstr)||!get(s,p,chars)||chars.size()!=4)return false;r.options.delimiter=chars[0];r.options.quote=chars[1];r.options.escape=chars[2];r.options.header=chars[3];uint64_t *fields[]={&r.snapshot.dev,&r.snapshot.inode,&r.snapshot.size,&r.snapshot.mtime,&r.snapshot.ctime};for(auto f:fields){if(!get(s,p,v))return false;*f=std::strtoull(v.c_str(),nullptr,10);}r.columns.clear();while(p<s.size()){Column c;if(!get(s,p,c.name)||!get(s,p,v))return false;int t=std::atoi(v.c_str());if(t<0||t>6)return false;c.type=Type(t);if(!get(s,p,v))return false;c.nullable=v=="1";r.columns.push_back(c);if(r.columns.size()>Reader::MAX_COLUMNS)return false;}return true;}

}}}
