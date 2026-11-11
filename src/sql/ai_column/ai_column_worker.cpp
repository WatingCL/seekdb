/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "query/ai/ai_column_runtime.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "sql/ai_column/ai_column_runtime.h"
#include "sql/ai_column/ai_column_sql_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "share/ai_service/ai_column_task.h"
#include "share/ob_thread_pool.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <cctype>
#include <unistd.h>

namespace oceanbase { namespace sql {
using namespace common;
using namespace share;
using namespace share::schema;
namespace {
ObString ai_string(const std::string &s) { return ObString(s.size(),s.data()); }
int ai_json(ObIAllocator &a, const ObString &s, ObIJsonBase *&out)
{ return ObJsonBaseFactory::get_json_base(&a,s,ObJsonInType::JSON_TREE,ObJsonInType::JSON_TREE,out); }
int ai_dimension(const ObColumnSchemaV2 &column)
{
  if (column.get_extended_type_info().count()==0) return 0;
  const std::string type=ai_text(column.get_extended_type_info().at(0));
  const auto start=type.find('(');return start==std::string::npos ? 0 : std::atoi(type.c_str()+start+1);
}
class AiColumnWorker : public ObThreadPool, public IAiColumnPublisher {
public:
  explicit AiColumnWorker(ObMySQLProxy &proxy):proxy_(proxy) { set_thread_count(4); }
  int publish(ObMySQLTransaction &trans,const ObIArray<AiColumnTask> &group,bool &applied) override
  {
    int ret=OB_SUCCESS;applied=false;
    if(group.empty()) return OB_INVALID_ARGUMENT;
    ObSchemaGetterGuard guard;const ObTableSchema *table=nullptr;const ObDatabaseSchema *db=nullptr;
    if(OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) return ret;
    if(OB_FAIL(guard.get_table_schema(group.at(0).table_id_,table))) return ret;
    if(table==nullptr) return ret;
    if(OB_FAIL(guard.get_database_schema(table->get_database_id(),db))) return ret;
    if(db==nullptr) return OB_SCHEMA_ERROR;
    const std::string target=ai_identifier(db->get_database_name_str())+"."+ai_identifier(table->get_table_name_str());
    AiColumnInternalGuard internal;
    // Each native update belongs to the supplied publication transaction. The
    // group becomes visible at its single commit, including vector index edits.
    for(int64_t i=0;i<group.count();++i) {
      const auto &task=group.at(i);const auto *column=table->get_column_schema(task.column_id_);
      if(column==nullptr||!column->is_ai_column()||column->get_schema_version()!=task.definition_version_) return OB_SUCCESS;
      const auto *request=table->get_column_schema(column->get_ai_column_request_column_id());
      const auto *state=table->get_column_schema(column->get_ai_column_state_column_id());
      if(request==nullptr||state==nullptr) return OB_SCHEMA_ERROR;
      const std::string value=task.result_is_null_ ? "NULL" : ai_literal(ai_text(task.result_));
      const std::string update="UPDATE "+target+" SET "+ai_identifier(column->get_column_name_str())+"="+value+","+
        ai_identifier(state->get_column_name_str())+"="+std::to_string(int64_t(task.state_))+
        " WHERE ("+ai_text(task.rowkey_predicate_)+") AND "+ai_identifier(request->get_column_name_str())+"="+ai_literal(ai_text(task.request_id_));
      int64_t affected=0;
      if(OB_FAIL(trans.write(update.c_str(),affected))) return ret;
      applied=applied||affected>0;
    }
    return ret;
  }

  int execute(const AiColumnTask &task,ObIAllocator &allocator,ObString &output,bool &is_null)
  {
    int ret=OB_SUCCESS;is_null=true;
    ObSchemaGetterGuard guard;const ObTableSchema *table=nullptr;
    if(OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) return ret;
    if(OB_FAIL(guard.get_table_schema(task.table_id_,table))) return ret;
    const auto *column=table==nullptr ? nullptr : table->get_column_schema(task.column_id_);
    if(column==nullptr||column->get_schema_version()!=task.definition_version_) return OB_SCHEMA_ERROR;
    ObIJsonBase *input=nullptr,*model_node=nullptr,*prompt_node=nullptr,*config_node=nullptr;
    if(OB_FAIL(ai_json(allocator,task.model_input_,input))) return ret;
    if(input->json_type()!=ObJsonNodeType::J_ARRAY||input->element_count()<2||input->element_count()>3) return OB_INVALID_DATA;
    if(OB_FAIL(input->get_array_element(0,model_node))||OB_FAIL(input->get_array_element(1,prompt_node))) return ret;
    if(prompt_node->json_type()==ObJsonNodeType::J_NULL) return OB_SUCCESS;
    if(model_node->json_type()!=ObJsonNodeType::J_STRING) return OB_INVALID_DATA;
    ObString model_id(model_node->get_data_length(),model_node->get_data()),prompt;
    if(prompt_node->json_type()==ObJsonNodeType::J_STRING) prompt.assign_ptr(prompt_node->get_data(),prompt_node->get_data_length());
    else if(prompt_node->json_type()==ObJsonNodeType::J_OBJECT) {
      auto *object=static_cast<ObJsonObject *>(prompt_node);
      if(!ObAIFuncPromptObjectUtils::is_valid_prompt_object(object)) return OB_INVALID_DATA;
      if(OB_FAIL(ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator,object,prompt))) return ret;
    } else return OB_INVALID_DATA;
    ObJsonObject *config=nullptr;
    if(input->element_count()==3) {
      if(OB_FAIL(input->get_array_element(2,config_node))) return ret;
      if(config_node->json_type()==ObJsonNodeType::J_STRING) {
        ObString text(config_node->get_data_length(),config_node->get_data());
        if(OB_FAIL(ai_json(allocator,text,config_node))) return ret;
      }
      if(config_node->json_type()!=ObJsonNodeType::J_NULL && config_node->json_type()!=ObJsonNodeType::J_INT) {
        if(config_node->json_type()!=ObJsonNodeType::J_OBJECT) return OB_INVALID_DATA;
        config=static_cast<ObJsonObject *>(config_node);
      }
    }
    const bool embed=ai_text(column->get_ai_column_expr()).find("AI_EMBED")!=std::string::npos || column->is_collection();
    if(embed) {
      if(config==nullptr) config=OB_NEWx(ObJsonObject,&allocator,&allocator);
      if(config==nullptr) return OB_ALLOCATE_MEMORY_FAILED;
      auto *dimension=OB_NEWx(ObJsonInt,&allocator,ai_dimension(*column));
      if(dimension==nullptr) return OB_ALLOCATE_MEMORY_FAILED;
      if(OB_FAIL(config->add("dimensions",dimension))) return ret;
    }
    ObAIFuncExprInfo *info=nullptr;ObAiModelEndpointInfo endpoint;
    auto *resolver=server_service<query::ObIAiEndpointResolver>();
    if(resolver==nullptr) return OB_NOT_INIT;
    if(OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator,model_id,info))) return ret;
    if(OB_FAIL(resolver->resolve_by_model_name(model_id,allocator,endpoint,false))) return ret;
    ObAIFuncModel model(allocator,*info,endpoint);ObString response;
    if(embed) ret=model.call_dense_embedding(prompt,config,response);
    else ret=model.call_completion(prompt,config,response,ObString(ai_column_result_hint(*column)));
    if(OB_FAIL(ret)) return ret;
    ObIJsonBase *value=nullptr;
    if(column->get_data_type()==ObJsonType||embed) {
      if(OB_FAIL(ai_json(allocator,response,value))) return ret;
      if(embed) {
        if(value->json_type()!=ObJsonNodeType::J_ARRAY||value->element_count()!=ai_dimension(*column)) return OB_INVALID_DATA;
        for(uint64_t i=0;i<value->element_count();++i) {
          ObIJsonBase *number=nullptr;if(OB_FAIL(value->get_array_element(i,number))) return ret;
          ObString text;if(OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(allocator,number,text))) return ret;
          const std::string s=ai_text(text);char *end=nullptr;errno=0;double d=std::strtod(s.c_str(),&end);
          if(errno||end!=s.c_str()+s.size()||!std::isfinite(d)) return OB_INVALID_DATA;
        }
      }
    } else if(ob_is_numeric_type(column->get_data_type())) {
      const std::string text=ai_text(response);char *end=nullptr;errno=0;
      const auto type=column->get_data_type();
      if(ob_is_integer_type(type)) {
        size_t begin=(!text.empty() && (text[0]=='-' || text[0]=='+')) ? 1 : 0;
        if(begin==text.size()) return OB_INVALID_DATA;
        for(size_t i=begin;i<text.size();++i) if(!std::isdigit(static_cast<unsigned char>(text[i]))) return OB_INVALID_DATA;
        ObObjType signed_type=type;convert_unsigned_type_to_signed(signed_type);
        int bits=signed_type==ObTinyIntType ? 8 : signed_type==ObSmallIntType ? 16 : signed_type==ObMediumIntType ? 24 : signed_type==ObInt32Type ? 32 : 64;
        if(ob_is_unsigned_type(type)) {
          if(text[0]=='-') return OB_INVALID_DATA;
          const auto value=std::strtoull(text.c_str(),&end,10);
          if(bits<64 && value>((uint64_t(1)<<bits)-1)) return OB_INVALID_DATA;
        } else {
          const auto value=std::strtoll(text.c_str(),&end,10);
          if(bits<64 && (value<-(int64_t(1)<<(bits-1)) || value>((int64_t(1)<<(bits-1))-1))) return OB_INVALID_DATA;
        }
      } else {
        double d=std::strtod(text.c_str(),&end);
        if(!std::isfinite(d) || (ob_is_unsigned_type(type) && d<0) ||
            ((type==ObFloatType || type==ObUFloatType) && std::abs(d)>std::numeric_limits<float>::max())) return OB_INVALID_DATA;
      }
      if(errno||end==text.c_str()||end!=text.c_str()+text.size()) return OB_INVALID_DATA;
    } else if(column->get_data_type()==ObDateType || column->get_data_type()==ObDateTimeType) {
      const std::string text=ai_text(response);
      const bool date=column->get_data_type()==ObDateType;
      if(text.size()!=(date ? 10u : 19u) || text[4]!='-' || text[7]!='-') return OB_INVALID_DATA;
      for(size_t i=0;i<text.size();++i) {
        if(i==4 || i==7 || (!date && (i==10 || i==13 || i==16))) continue;
        if(!std::isdigit(static_cast<unsigned char>(text[i]))) return OB_INVALID_DATA;
      }
      const int year=std::atoi(text.c_str()),month=std::atoi(text.c_str()+5),day=std::atoi(text.c_str()+8);
      const int days[]={0,31,28+int(year%4==0 && (year%100!=0 || year%400==0)),31,30,31,30,31,31,30,31,30,31};
      if(year<1 || month<1 || month>12 || day<1 || day>days[month]) return OB_INVALID_DATA;
      if(!date && (text[10]!=' ' || text[13]!=':' || text[16]!=':' || std::atoi(text.c_str()+11)>23 ||
          std::atoi(text.c_str()+14)>59 || std::atoi(text.c_str()+17)>59)) return OB_INVALID_DATA;
    } else if(ob_is_string_type(column->get_data_type()) &&
        ObCharset::strlen_char(column->get_collation_type(),response.ptr(),response.length())>column->get_data_length()) {
      return OB_ERR_DATA_TOO_LONG;
    }
    output=response;is_null=false;return ret;
  }
  void run1() override
  {
    lib::set_thread_name("AiColumn");
    while (!has_set_stop() && !share::g_server_modules_ready) usleep(10000);
    SERVER_MODULE_SCOPE {
      AiColumnTaskStore store;
      while(!has_set_stop()) {
        ObArenaAllocator allocator("AiColumnWorker");AiColumnTask task;bool found=false;
        char owner[96];snprintf(owner,sizeof(owner),"%d-%p-%ld",getpid(),this,static_cast<long>(get_thread_idx()));
        int ret=store.claim_one(proxy_,ObString(owner),ObTimeUtility::current_time(),90000000,allocator,task,found);
        if(OB_SUCC(ret)&&found) {
          ObString output;bool is_null=true;
          const int64_t saved_timeout=THIS_WORKER.get_timeout_ts();
          THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time()+30000000);
          const int call_ret=execute(task,allocator,output,is_null);
          THIS_WORKER.set_timeout_ts(saved_timeout);
          if (call_ret != OB_SUCCESS) LOG_WARN("AI column model attempt failed",K(call_ret),K(task));
          ObMySQLTransaction trans;
          if(OB_SUCC(ret=trans.start(&proxy_))) {
            ret=store.record_result(trans,task,ObTimeUtility::current_time(),call_ret==OB_SUCCESS,output,is_null,call_ret,1000000);
            const int end_ret=trans.end(ret==OB_SUCCESS);if(OB_SUCC(ret)) ret=end_ret;
          }
          bool published=false;if(OB_SUCC(ret)) ret=store.publish_group(proxy_,task.request_id_,*this,published);
          if (ret != OB_SUCCESS) LOG_WARN("AI column record or publication failed",K(ret),K(task));
        }
        // Recover groups whose last worker died between recording and publishing,
        // including exhausted leases converted to FAILED during claim.
        if(!found) {
          ObMySQLProxy::MySQLResult result;
          std::string request;
          if(OB_SUCC(proxy_.read(result,"SELECT request_id FROM oceanbase.__all_ai_column_task GROUP BY request_id HAVING SUM(status IN ('PENDING','RUNNING'))=0 LIMIT 1"))) {
            auto *rows=result.get_result();ObString id;
            if(rows!=nullptr&&OB_SUCC(rows->next())&&OB_SUCC(rows->get_varchar("request_id",id))) request=ai_text(id);
          }
          result.reset();bool published=false;
          if(!request.empty()) store.publish_group(proxy_,ai_string(request),*this,published);
          usleep(100000);
        }
      }
    }
  }
private:
  ObMySQLProxy &proxy_;
};
AiColumnWorker *ai_worker=nullptr;
}
} namespace query {
int start_ai_column_worker(common::ObMySQLProxy &proxy)
{
  if(sql::ai_worker!=nullptr) return OB_INIT_TWICE;
  sql::ai_worker=new sql::AiColumnWorker(proxy);
  return sql::ai_worker==nullptr ? OB_ALLOCATE_MEMORY_FAILED : sql::ai_worker->start();
}
void stop_ai_column_worker() {if(sql::ai_worker!=nullptr) sql::ai_worker->stop();}
void wait_ai_column_worker() {if(sql::ai_worker!=nullptr) {sql::ai_worker->wait();delete sql::ai_worker;sql::ai_worker=nullptr;}}
} }
