/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
// Included inside file_sql after Cell/Column declarations. Format decoding only;
// predicate evaluation and relational operators belong to the seekdb executor.
struct CompactValue {
  int64_t integer=0;std::string bytes;std::map<int,CompactValue> fields;std::vector<CompactValue> list;
  const CompactValue &field(int id)const{auto it=fields.find(id);static const CompactValue empty;return it==fields.end()?empty:it->second;}
};
class CompactReader {
public:
  CompactReader(const std::string &data,size_t start=0):pos(start),data_(data){}
  bool structure(CompactValue &value,int depth=0){
    if(depth>32)return false;int id=0;
    while(pos<data_.size()){unsigned char h=data_[pos++];if(h==0)return true;int delta=h>>4,type=h&15;
      if(delta)id+=delta;else{uint64_t v;if(!varint(v))return false;id=zigzag(v);}
      CompactValue next;if(!read(type,next,depth+1))return false;value.fields.emplace(id,std::move(next));
    }return false;
  }
  size_t pos;
private:
  static int64_t zigzag(uint64_t v){return static_cast<int64_t>((v>>1)^uint64_t(-int64_t(v&1)));}
  bool varint(uint64_t &v){v=0;for(int i=0;i<10&&pos<data_.size();++i){unsigned char c=data_[pos++];if(i==9&&c>1)return false;v|=uint64_t(c&127)<<(7*i);if(!(c&128))return true;}return false;}
  bool read(int type,CompactValue &v,int depth){
    if(depth>32)return false;uint64_t n=0;
    switch(type){
    case 1:v.integer=1;return true;case 2:v.integer=0;return true;
    case 3:if(pos>=data_.size())return false;v.integer=static_cast<int8_t>(data_[pos++]);return true;
    case 4:case 5:case 6:if(!varint(n))return false;v.integer=zigzag(n);return true;
    case 7:if(pos+8>data_.size())return false;pos+=8;return true;
    case 8:if(!varint(n)||n>data_.size()-pos)return false;v.bytes=data_.substr(pos,n);pos+=n;return true;
    case 9:case 10:{if(pos>=data_.size())return false;unsigned char h=data_[pos++];n=h>>4;if(n==15&&!varint(n))return false;if(n>1000000)return false;v.list.reserve(n);for(size_t i=0;i<n;++i){CompactValue c;if(!read(h&15,c,depth+1))return false;v.list.push_back(std::move(c));}return true;}
    case 11:{if(!varint(n)||n>1000000)return false;if(!n)return true;if(pos>=data_.size())return false;unsigned char h=data_[pos++];for(size_t i=0;i<n;++i){CompactValue k,x;if(!read(h>>4,k,depth+1)||!read(h&15,x,depth+1))return false;}return true;}
    case 12:return structure(v,depth+1);
    default:return false;
    }
  }
  const std::string &data_;
};
inline uint64_t little(const char *p,size_t n){uint64_t x=0;for(size_t i=0;i<n;++i)x|=uint64_t(static_cast<unsigned char>(p[i]))<<(8*i);return x;}
inline bool snappy_decode(const std::string &in,std::string &out,size_t expected){
  size_t p=0;uint64_t length=0;int shift=0;while(p<in.size()&&shift<35){unsigned char c=in[p++];length|=uint64_t(c&127)<<shift;if(!(c&128))break;shift+=7;}
  if(length!=expected||length>64*1024*1024)return false;out.clear();out.reserve(length);
  while(p<in.size()&&out.size()<length){unsigned char tag=in[p++];size_t n=0,off=0;
    if((tag&3)==0){n=(tag>>2)+1;if(n>=61){size_t bytes=n-60;if(bytes>4||p+bytes>in.size())return false;n=little(in.data()+p,bytes)+1;p+=bytes;}if(n>length-out.size()||p+n>in.size())return false;out.append(in,p,n);p+=n;}
    else{if((tag&3)==1){n=4+((tag>>2)&7);if(p>=in.size())return false;off=((tag&0xE0)<<3)|static_cast<unsigned char>(in[p++]);}
      else{size_t bytes=(tag&3)==2?2:4;n=1+(tag>>2);if(p+bytes>in.size())return false;off=little(in.data()+p,bytes);p+=bytes;}
      if(off==0||off>out.size()||n>length-out.size())return false;for(size_t i=0;i<n;++i)out.push_back(out[out.size()-off]);}
  }return out.size()==length&&p==in.size();
}
inline bool read_varint(const std::string &data,size_t &p,uint64_t &v){v=0;for(int i=0;i<10&&p<data.size();++i){unsigned char c=data[p++];if(i==9&&c>1)return false;v|=uint64_t(c&127)<<(7*i);if(!(c&128))return true;}return false;}
inline bool decode_levels(const std::string &data,size_t &p,int bits,size_t count,std::vector<uint32_t> &out){
  out.clear();if(bits<0||bits>32)return false;
  while(out.size()<count){uint64_t h;if(!read_varint(data,p,h)||h==0)return false;
    if(!(h&1)){size_t n=h>>1,bytes=(bits+7)/8;if(n>count-out.size()||p+bytes>data.size())return false;uint32_t value=little(data.data()+p,bytes);p+=bytes;out.insert(out.end(),n,value);}
    else{uint64_t n=(h>>1)*8;if(n>10000000)return false;size_t bytes=(n*bits+7)/8;if(p+bytes>data.size())return false;for(size_t i=0;i<n&&out.size()<count;++i){uint64_t value=0;size_t bit=i*bits;for(int j=0;j<bits;++j)value|=uint64_t((static_cast<unsigned char>(data[p+(bit+j)/8])>>((bit+j)%8))&1)<<j;out.push_back(value);}p+=bytes;}
  }return true;
}

class ParquetReader {
  struct Schema {int physical=0,converted=-1,length=0;bool optional=false;std::string name;Type type=Type::VARCHAR;};
  struct Chunk {uint64_t offset=0,values=0;int codec=0;};
  struct Group {uint64_t rows=0;std::vector<Chunk> chunks;};
  struct Cursor {uint64_t offset=0;std::vector<Cell> dictionary,page;size_t at=0;};
public:
  std::set<size_t> projection;
  bool projection_set = false;
  std::string error;
  bool init(FILE *file,std::vector<Column> &columns){
    file_=file;schema_.clear();groups_.clear();cursors_.clear();group_=0;row_=0;
    if(fseeko(file_,0,SEEK_END))return fail("Parquet seek failed");off_t size=ftello(file_);std::string tail;
    if(size<12||!bytes(size-8,8,tail)||tail.substr(4)!="PAR1")return fail("invalid Parquet magic");
    uint64_t len=little(tail.data(),4);if(len>16*1024*1024||len>uint64_t(size-12))return fail("Parquet footer limit exceeded");
    std::string footer;if(!bytes(size-8-len,len,footer))return false;CompactValue meta;CompactReader compact(footer);
    if(!compact.structure(meta))return fail("invalid Parquet compact footer");const auto &elements=meta.field(2).list;
    if(elements.empty()||elements[0].field(5).integer!=int64_t(elements.size()-1))return fail("nested Parquet schema is unsupported");
    if(elements.size()>1025)return fail("Parquet column limit exceeded");
    for(size_t i=1;i<elements.size();++i){const auto &e=elements[i];Schema s;s.name=e.field(4).bytes;s.physical=e.field(1).integer;s.optional=e.field(3).integer==1;s.length=e.field(2).integer;
      if(e.field(3).integer==2||e.field(5).integer!=0)return fail("nested or repeated Parquet fields are unsupported");
      auto converted=e.fields.find(6);s.converted=converted==e.fields.end()?-1:converted->second.integer;
      const auto logical=e.fields.find(10);
      if(s.converted==5||(logical!=e.fields.end()&&logical->second.fields.count(5)))
        return fail("Parquet decimal values are unsupported");
      if(logical!=e.fields.end()) {
        if(logical->second.fields.count(6))s.converted=6;
        auto timestamp=logical->second.fields.find(8);
        if(timestamp!=logical->second.fields.end()) {
          const auto &unit=timestamp->second.field(2);
          if(unit.fields.count(1))s.converted=9;
          else if(unit.fields.count(2))s.converted=10;
          else if(unit.fields.count(3))s.converted=22;
          else return fail("unknown Parquet timestamp unit");
        }
      }
      switch(s.physical){case 0:s.type=Type::BOOLEAN;break;case 1:case 2:s.type=Type::BIGINT;break;case 3:s.type=Type::DATETIME;break;case 4:case 5:s.type=Type::DOUBLE;break;case 6:case 7:s.type=Type::VARCHAR;break;default:return fail("unsupported Parquet physical type");}
      if(s.converted==6)s.type=Type::DATE;if(s.converted==9||s.converted==10||s.converted==22)s.type=Type::DATETIME;
      if(!valid_utf8(s.name)||s.name.empty())return fail("invalid Parquet column name");schema_.push_back(s);
    }
    for(const auto &g:meta.field(4).list){Group group;group.rows=g.field(3).integer;if(group.rows>uint64_t(std::numeric_limits<int64_t>::max()))return fail("invalid Parquet row count");
      for(const auto &c:g.field(1).list){if(!c.field(1).bytes.empty())return fail("external Parquet column chunks are unsupported");const auto &m=c.field(3);Chunk chunk;chunk.values=m.field(5).integer;chunk.codec=m.field(4).integer;chunk.offset=m.field(9).integer;uint64_t dict=m.field(11).integer;if(dict&&dict<chunk.offset)chunk.offset=dict;
        if(chunk.offset<4||chunk.offset>=uint64_t(size)||chunk.values!=group.rows||(chunk.codec!=0&&chunk.codec!=1))return fail("unsupported Parquet chunk or codec");group.chunks.push_back(chunk);}
      if(group.chunks.size()!=schema_.size())return fail("Parquet column count mismatch");groups_.push_back(std::move(group));
    }
    columns.clear();for(const auto &s:schema_)columns.push_back({s.name,s.type,s.optional});reset_group();return true;
  }
  bool rescan(){group_=0;row_=0;reset_group();return true;}
  bool next(std::vector<Cell> &row,bool &end){
    while(group_<groups_.size()&&row_>=groups_[group_].rows){++group_;row_=0;reset_group();}
    if(group_>=groups_.size()){end=true;return true;}end=false;row.assign(schema_.size(),Cell());
    for(size_t i=0;i<schema_.size();++i){if(projection_set&&!projection.count(i))continue;Cursor &c=cursors_[i];if(c.at>=c.page.size()&&!page(i))return false;if(c.at>=c.page.size())return fail("Parquet page has too few rows");row[i]=c.page[c.at++];}
    ++row_;return true;
  }
private:
  bool fail(const char *message){error=message;return false;}
  bool bytes(uint64_t offset,size_t n,std::string &out){if(n>64*1024*1024||fseeko(file_,offset,SEEK_SET))return fail("Parquet read limit or seek error");out.resize(n);if(n&&fread(&out[0],1,n,file_)!=n)return fail("truncated Parquet file");return true;}
  void reset_group(){cursors_.clear();if(group_<groups_.size()){cursors_.resize(schema_.size());for(size_t i=0;i<cursors_.size();++i)cursors_[i].offset=groups_[group_].chunks[i].offset;}}
  static std::string real(double v){char buf[64];std::snprintf(buf,sizeof(buf),"%.17g",v);return buf;}
  bool plain(const std::string &data,size_t &p,size_t count,const Schema &schema,std::vector<Cell> &out){
    out.clear();for(size_t i=0;i<count;++i){Cell cell;cell.null_value=false;size_t n=0;uint64_t x=0;
      switch(schema.physical){
      case 0:if(p+i/8>=data.size())return fail("truncated Parquet boolean");cell.text=((data[p+i/8]>>(i%8))&1)?"true":"false";break;
      case 1:case 2:case 3:{n=schema.physical==1?4:schema.physical==2?8:12;if(p+n>data.size())return fail("truncated Parquet integer");x=little(data.data()+p,std::min(n,size_t(8)));int64_t value=n==4?static_cast<int32_t>(x):static_cast<int64_t>(x);p+=n;
        if(schema.type==Type::DATE||schema.type==Type::DATETIME){int64_t seconds=value;if(schema.type==Type::DATE)seconds=value*86400;else if(schema.physical==3){int64_t days=int64_t(little(data.data()+p-4,4))-2440588;seconds=days*86400+value/1000000000;}else seconds=value/(schema.converted==22?1000000000:schema.converted==10?1000000:1000);
          time_t stamp=seconds;struct tm t;if(!gmtime_r(&stamp,&t))return fail("Parquet timestamp out of range");char buf[64];strftime(buf,sizeof(buf),schema.type==Type::DATE?"%Y-%m-%d":"%Y-%m-%d %H:%M:%S",&t);cell.text=buf;
        }else cell.text=std::to_string(value);break;}
      case 4:{if(p+4>data.size())return fail("truncated Parquet float");uint32_t v=little(data.data()+p,4);float f;memcpy(&f,&v,4);p+=4;if(!std::isfinite(f))return fail("non-finite Parquet float");cell.text=real(f);break;}
      case 5:{if(p+8>data.size())return fail("truncated Parquet double");uint64_t v=little(data.data()+p,8);double d;memcpy(&d,&v,8);p+=8;if(!std::isfinite(d))return fail("non-finite Parquet double");cell.text=real(d);break;}
      case 6:case 7:if(schema.physical==6){if(p+4>data.size())return fail("truncated Parquet string");n=little(data.data()+p,4);p+=4;}else n=schema.length;if(n>16*1024*1024||p+n>data.size())return fail("Parquet field limit exceeded");cell.text=data.substr(p,n);p+=n;break;
      default:return fail("unsupported Parquet type");
      }
      out.push_back(std::move(cell));
    }if(schema.physical==0)p+=(count+7)/8;return true;
  }
  bool page(size_t column){
    Cursor &cursor=cursors_[column];const Schema &schema=schema_[column];const Chunk &chunk=groups_[group_].chunks[column];
    for(int pages=0;pages<100000;++pages){
      // Header size is not prefixed: a bounded read is decoded by compact protocol.
      if(fseeko(file_,0,SEEK_END))return false;off_t size=ftello(file_);if(cursor.offset>=uint64_t(size))return fail("Parquet page offset out of range");
      std::string header;if(!bytes(cursor.offset,std::min<uint64_t>(65536,uint64_t(size)-cursor.offset),header))return false;CompactValue h;CompactReader r(header);if(!r.structure(h))return fail("invalid Parquet page header");
      int type=h.field(1).integer;int64_t raw=h.field(2).integer,compressed=h.field(3).integer;if(raw<0||compressed<0||raw>16*1024*1024||compressed>16*1024*1024)return fail("Parquet page resource limit exceeded");
      std::string data;if(!bytes(cursor.offset+r.pos,compressed,data))return false;cursor.offset+=r.pos+compressed;
      if(type==1)continue; // index page
      const bool v2=type==3;const CompactValue &ph=h.field(v2?8:type==2?7:5);size_t count=ph.field(1).integer;
      uint64_t memory=raw+compressed+count*sizeof(Cell)*2;
      for(const Cursor &other:cursors_){
        memory+=(other.dictionary.capacity()+other.page.capacity())*sizeof(Cell);
        for(const Cell &cell:other.dictionary)memory+=cell.text.capacity();
        for(const Cell &cell:other.page)memory+=cell.text.capacity();
      }
      if(count>1000000||memory>128*1024*1024)return fail("Parquet page memory limit exceeded");size_t p=0;std::vector<uint32_t> definitions;
      if(v2){size_t deflen=ph.field(5).integer,replen=ph.field(6).integer;if(replen!=0||deflen>data.size())return fail("nested Parquet page is unsupported");if(schema.optional){std::string def=data.substr(0,deflen);size_t dp=0;if(!decode_levels(def,dp,1,count,definitions))return fail("invalid Parquet definition levels");}p=deflen;
        auto compressed_flag=ph.fields.find(7);bool is_compressed=compressed_flag==ph.fields.end()||compressed_flag->second.integer;
        if(chunk.codec==1&&is_compressed){std::string values;if(size_t(raw)<p||!snappy_decode(data.substr(p),values,raw-p))return fail("invalid Parquet Snappy page");data.resize(p);data+=values;}
      }else{
        if(chunk.codec==1){std::string expanded;if(!snappy_decode(data,expanded,raw))return fail("invalid Parquet Snappy page");data.swap(expanded);}else if(size_t(raw)!=data.size())return fail("Parquet page size mismatch");
        if(type!=2&&schema.optional){if(p+4>data.size())return fail("missing Parquet definition levels");size_t n=little(data.data()+p,4);p+=4;if(n>data.size()-p)return fail("invalid Parquet level length");std::string def=data.substr(p,n);size_t dp=0;if(!decode_levels(def,dp,1,count,definitions))return fail("invalid Parquet definition levels");p+=n;}
      }
      int encoding=ph.field(type==2?2:v2?4:2).integer;
      if(type==2){if(encoding!=0||!plain(data,p,count,schema,cursor.dictionary))return fail("unsupported Parquet dictionary encoding");continue;}
      if(type!=0&&type!=3)return fail("unsupported Parquet page type");
      size_t valid=count;if(schema.optional){valid=0;for(uint32_t d:definitions){if(d>1)return fail("invalid Parquet definition level");valid+=d;}}
      std::vector<Cell> values;
      if(encoding==0){if(!plain(data,p,valid,schema,values))return false;}
      else if(encoding==3&&schema.physical==0){
        if(p+4>data.size())return fail("missing Parquet boolean RLE length");
        size_t n=little(data.data()+p,4);p+=4;
        if(n>data.size()-p)return fail("invalid Parquet boolean RLE length");
        std::string encoded=data.substr(p,n);size_t bp=0;std::vector<uint32_t> bits;
        if(!decode_levels(encoded,bp,1,valid,bits))return fail("invalid Parquet boolean RLE");
        for(uint32_t bit:bits){if(bit>1)return fail("invalid Parquet boolean");values.push_back(Cell(bit?"true":"false"));}
      }
      else if(encoding==2||encoding==8){if(p>=data.size())return fail("missing Parquet dictionary indices");int bits=static_cast<unsigned char>(data[p++]);std::vector<uint32_t> indices;if(!decode_levels(data,p,bits,valid,indices))return fail("invalid Parquet dictionary indices");for(uint32_t i:indices){if(i>=cursor.dictionary.size())return fail("Parquet dictionary index out of range");values.push_back(cursor.dictionary[i]);}}
      else return fail("unsupported Parquet data encoding");
      cursor.page.clear();cursor.at=0;cursor.page.reserve(count);size_t used=0;for(size_t i=0;i<count;++i){if(schema.optional&&!definitions[i])cursor.page.push_back(Cell());else cursor.page.push_back(std::move(values[used++]));}if(!count)return fail("empty Parquet data page");return true;
    }return fail("Parquet page limit exceeded");
  }
  FILE *file_=nullptr;std::vector<Schema> schema_;std::vector<Group> groups_;std::vector<Cursor> cursors_;size_t group_=0;uint64_t row_=0;
};
