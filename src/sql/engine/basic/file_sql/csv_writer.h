/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "file_reader.h"
#include <atomic>
namespace oceanbase { namespace sql { namespace file_sql {
class CsvWriter {
public:
  CsvWriter():directory_(-1),file_(nullptr),owns_temporary_(false){}
  ~CsvWriter(){if(file_)fclose(file_);if(directory_>=0){if(owns_temporary_&&!temporary_.empty())unlinkat(directory_,temporary_.c_str(),0);::close(directory_);}}
  std::string error;
  bool open(const Options &options,const std::string &root){
    options_=options;
    if(options.nullstr.find(options.delimiter)!=std::string::npos ||
       options.nullstr.find(options.quote)!=std::string::npos ||
       options.nullstr.find_first_of("\r\n")!=std::string::npos || !valid_utf8(options.nullstr))
      return fail("COPY NULL text cannot contain CSV structural characters");
    size_t slash=options.path.rfind('/');
    if(slash==std::string::npos||options.path.find('\0')!=std::string::npos)return fail("COPY requires an absolute local path");
    final_=options.path.substr(slash+1);if(final_.empty()||final_=="."||final_=="..")return fail("invalid COPY target name");
    std::string parent=options.path.substr(0,slash),canonical_dir;if(parent.empty())parent="/";
    if(!canonical(parent,root,canonical_dir,error))return false;
    directory_=open_canonical(canonical_dir,true);
    struct stat actual,current;if(directory_<0||fstat(directory_,&actual)||stat(canonical_dir.c_str(),&current)||actual.st_ino!=current.st_ino||actual.st_dev!=current.st_dev)return fail("COPY directory changed");
    struct stat target;if(fstatat(directory_,final_.c_str(),&target,AT_SYMLINK_NOFOLLOW)==0||errno!=ENOENT)return fail("COPY target already exists or cannot be inspected");
    static std::atomic<uint64_t> sequence{0};int fd=-1;
    for(int i=0;i<100;++i){temporary_=".seekdb-copy-"+std::to_string(getpid())+"-"+std::to_string(sequence.fetch_add(1));fd=openat(directory_,temporary_.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd>=0){owns_temporary_=true;break;}if(errno!=EEXIST)return fail("cannot create COPY temporary file");}
    if(fd<0)return fail("COPY temporary-file collision limit exceeded");file_=fdopen(fd,"wb");if(!file_){::close(fd);return fail("cannot open COPY stream");}return true;
  }
  bool row(const std::vector<Cell> &values){
    size_t bytes=0;for(size_t i=0;i<values.size();++i){if(i&&fputc(options_.delimiter,file_)==EOF)return fail("COPY write failed");const Cell &c=values[i];if(!c.null_value&&!valid_utf8(c.text))return fail("COPY requires UTF-8 text");bytes+=c.text.size();if(bytes>Reader::MAX_RECORD)return fail("COPY record limit exceeded");
      if(c.null_value){if(fwrite(options_.nullstr.data(),1,options_.nullstr.size(),file_)!=options_.nullstr.size())return fail("COPY write failed");}
      else{if(fputc(options_.quote,file_)==EOF)return fail("COPY write failed");for(char ch:c.text){if((ch==options_.quote||(ch==options_.escape&&options_.escape!=options_.quote))&&fputc(options_.escape,file_)==EOF)return fail("COPY write failed");if(fputc(static_cast<unsigned char>(ch),file_)==EOF)return fail("COPY write failed");}if(fputc(options_.quote,file_)==EOF)return fail("COPY write failed");}}
    return fputc('\n',file_)!=EOF||fail("COPY write failed");
  }
  bool finish(){
    if(!file_||fflush(file_)||fsync(fileno(file_)))return fail("COPY flush failed");
    if(fclose(file_)){file_=nullptr;return fail("COPY close failed");}file_=nullptr;
    // linkat is an atomic no-replace publication, including concurrent writers.
    if(linkat(directory_,temporary_.c_str(),directory_,final_.c_str(),0))return fail("COPY target exists or publication failed");
    if(unlinkat(directory_,temporary_.c_str(),0))return fail("COPY temporary-file cleanup failed");temporary_.clear();owns_temporary_=false;return true;
  }
private:
  bool fail(const char *message){error=message;return false;}
  int directory_;FILE *file_;bool owns_temporary_;Options options_;std::string final_,temporary_;
  CsvWriter(const CsvWriter &)=delete;CsvWriter &operator=(const CsvWriter &)=delete;
};
}}}
