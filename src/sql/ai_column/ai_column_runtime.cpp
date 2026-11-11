/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/ai_column/ai_column_runtime.h"
#include "sql/ai_column/ai_column_sql_utils.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "share/ai_service/ai_column_task.h"
#include "share/schema/ob_table_schema.h"
#include "share/ob_dml_sql_splicer.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "lib/random/ob_random.h"
#include <atomic>
#include <vector>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace oceanbase { namespace sql {
using namespace common;
using namespace share::schema;
namespace {
// Authorization follows the SQL session across coroutine scheduling. A thread
// local flag would grant unrelated requests access while nested SQL yields.
std::mutex ai_session_mutex;
std::unordered_map<ObSQLSessionInfo *, int> ai_internal_sessions;
}
AiColumnInternalGuard::AiColumnInternalGuard(ObSQLSessionInfo *session)
  : session_(session != nullptr ? session : THIS_WORKER.get_session())
{
  if (session_ != nullptr) { std::lock_guard<std::mutex> lock(ai_session_mutex);++ai_internal_sessions[session_]; }
}
AiColumnInternalGuard::~AiColumnInternalGuard()
{
  if (session_ != nullptr) {
    std::lock_guard<std::mutex> lock(ai_session_mutex);
    const auto it=ai_internal_sessions.find(session_);
    if(it!=ai_internal_sessions.end() && --it->second==0) ai_internal_sessions.erase(it);
  }
}
bool ai_column_is_internal()
{
  auto *session=THIS_WORKER.get_session();
  if(session==nullptr) return false;
  std::lock_guard<std::mutex> lock(ai_session_mutex);
  return ai_internal_sessions.find(session)!=ai_internal_sessions.end();
}
namespace {
struct AiPendingRow {
  std::string predicate, cursor;
  std::vector<std::string> snapshots, inputs;
  std::vector<bool> changed;
};
int ai_read_text(sqlclient::ObMySQLResult &result, const std::string &name, std::string &out)
{
  ObString value;
  int ret=result.get_varchar(name.c_str(),value);
  if(ret==OB_ERR_NULL_VALUE) { out.clear();ret=OB_SUCCESS; }
  else if(ret==OB_SUCCESS) out=ai_text(value);
  return ret;
}
// Produce a SQL literal for the value of a key, without depending on SQL_MODE
// string escaping. Numeric keys retain their natural diagnostic spelling.
std::string ai_key_literal(const ObColumnSchemaV2 &column)
{
  const std::string name=ai_identifier(column.get_column_name_str());
  if(ob_is_numeric_type(column.get_data_type())) return "CAST("+name+" AS CHAR)";
  const bool binary=column.get_collation_type()==CS_TYPE_BINARY;
  return "CONCAT("+ai_literal(binary ? "X'" : "CONVERT(X'")+",HEX("+name+"),"+
      ai_literal(binary ? "'" : "' USING utf8mb4)")+")";
}
}

int reconcile_ai_column_table(ObIInnerSQLConnection &connection,
    const ObTableSchema &table, const ObString &database)
{
  int ret=OB_SUCCESS;
  std::vector<const ObColumnSchemaV2 *> columns;
  for(auto it=table.column_begin();it!=table.column_end();++it)
    if((*it)->is_ai_column()) columns.push_back(*it);
  if(columns.empty()) return ret;
  AiColumnInternalGuard internal;
  const std::string target=ai_identifier(database)+"."+ai_identifier(table.get_table_name_str());
  std::string key_names="(", key_values="CONCAT("+ai_literal("(");
  std::string predicate="CONCAT(";
  std::vector<uint64_t> keys;
  for(int64_t i=0;OB_SUCC(ret)&&i<table.get_rowkey_info().get_size();++i) {
    uint64_t id=0;
    if(OB_FAIL(table.get_rowkey_info().get_column_id(i,id))) break;
    const auto *column=table.get_column_schema(id);
    if(column==nullptr) {ret=OB_SCHEMA_ERROR;break;}
    keys.push_back(id);
    const std::string name=ai_identifier(column->get_column_name_str());
    if(i) {key_names+=",";key_values+=","+ai_literal(",");predicate+=",";}
    key_names+=name;key_values+=","+ai_key_literal(*column);
    const bool timestamp=column->get_data_type()==ObTimestampType;
    predicate+=ai_literal((i ? " AND " : "")+(timestamp ? "UNIX_TIMESTAMP("+name+")" : name)+" = ")+","+
        (timestamp ? "CAST(UNIX_TIMESTAMP("+name+") AS CHAR)" : ai_key_literal(*column));
  }
  if(OB_FAIL(ret)) return ret;
  if(keys.empty()) return OB_NOT_SUPPORTED;
  key_names+=")";key_values+=","+ai_literal(")")+")";predicate+=")";
  std::string select="SELECT "+predicate+" AS p,"+key_values+" AS k";
  for(size_t i=0;i<columns.size();++i) {
    const auto &column=*columns[i];
    std::string deps=ai_text(column.get_ai_column_dependency_ids());
    std::string snapshot="JSON_ARRAY("+std::to_string(column.get_schema_version());
    auto snapshot_value=[](const ObColumnSchemaV2 &col) {
      const std::string name=ai_identifier(col.get_column_name_str());
      return col.get_data_type()==ObTimestampType ? "UNIX_TIMESTAMP("+name+")" :
        col.get_collation_type()==CS_TYPE_BINARY && !ob_is_numeric_type(col.get_data_type()) ? "HEX("+name+")" : name;
    };
    for(uint64_t id:keys) snapshot+=","+snapshot_value(*table.get_column_schema(id));
    size_t start=0;
    while(start<deps.size()) {
      size_t end=deps.find(',',start);
      uint64_t id=std::strtoull(deps.c_str()+start,nullptr,10);
      const auto *dep=table.get_column_schema(id);
      if(dep==nullptr) return OB_SCHEMA_ERROR;
      snapshot+=","+snapshot_value(*dep);
      if(end==std::string::npos) break;
      start=end+1;
    }
    snapshot+=")";
    const std::string expression=ai_text(column.get_ai_column_expr());
    const size_t first=expression.find('('),last=expression.rfind(')');
    if(first==std::string::npos||last<=first) return OB_INVALID_ARGUMENT;
    const auto *request=table.get_column_schema(column.get_ai_column_request_column_id());
    const auto *spec=table.get_column_schema(column.get_ai_column_result_spec_column_id());
    if(request==nullptr||spec==nullptr) return OB_SCHEMA_ERROR;
    const std::string suffix=std::to_string(i);
    select+=",CAST("+snapshot+" AS CHAR CHARACTER SET utf8mb4) AS d"+suffix+
        ",CAST(JSON_ARRAY("+expression.substr(first+1,last-first-1)+") AS CHAR CHARACTER SET utf8mb4) AS i"+suffix+
        ","+ai_identifier(request->get_column_name_str())+" AS r"+suffix+
        ",CAST("+ai_identifier(spec->get_column_name_str())+" AS CHAR CHARACTER SET utf8mb4) AS s"+suffix;
  }
  std::string cursor;
  static std::atomic<uint64_t> sequence{0};
  for(bool done=false;OB_SUCC(ret)&&!done;) {
    const std::string query=select+" FROM "+target+(cursor.empty() ? "" : " WHERE "+key_names+" > "+cursor)+
        " ORDER BY "+key_names.substr(1,key_names.size()-2)+" LIMIT 32 FOR UPDATE";
    ObMySQLProxy::MySQLResult result;
    std::vector<AiPendingRow> rows;
    size_t bytes=0;
    if(OB_FAIL(connection.execute_read(ObString(query.size(),query.data()),result))) break;
    auto *data=result.get_result();
    if(data==nullptr) {ret=OB_ERR_UNEXPECTED;break;}
    while(OB_SUCC(ret)&&OB_SUCC(ret=data->next())) {
      AiPendingRow row;
      if(OB_FAIL(ai_read_text(*data,"p",row.predicate))||OB_FAIL(ai_read_text(*data,"k",row.cursor))) break;
      for(size_t i=0;OB_SUCC(ret)&&i<columns.size();++i) {
        std::string snapshot,input,old,request;
        const std::string suffix=std::to_string(i);
        if(OB_FAIL(ai_read_text(*data,"d"+suffix,snapshot))||OB_FAIL(ai_read_text(*data,"i"+suffix,input))||
           OB_FAIL(ai_read_text(*data,"s"+suffix,old))||OB_FAIL(ai_read_text(*data,"r"+suffix,request))) break;
        bytes+=snapshot.size()+input.size();
        if(input.size()>share::AiColumnTaskStore::MAX_MODEL_INPUT_LENGTH||bytes>64*1024*1024) {ret=OB_SIZE_OVERFLOW;break;}
        row.changed.push_back(request.empty()||snapshot!=old);
        row.snapshots.push_back(std::move(snapshot));row.inputs.push_back(std::move(input));
      }
      if(OB_SUCC(ret)) rows.push_back(std::move(row));
    }
    if(ret==OB_ITER_END) ret=OB_SUCCESS;
    const int close_ret=data->close();
    result.reset();
    if(OB_SUCC(ret)) ret=close_ret;
    if(OB_FAIL(ret)) break;
    done=rows.size()<32;
    for(const AiPendingRow &row:rows) {
      cursor=row.cursor;
      int64_t group_size=0;
      for(bool changed:row.changed) group_size+=changed;
      if(!group_size) continue;
      const int64_t now=ObTimeUtility::current_time();
      char request_id[65];
      snprintf(request_id,sizeof(request_id),"%016lx%016lx%016lx", static_cast<unsigned long>(now),
          static_cast<unsigned long>(sequence.fetch_add(1)),static_cast<unsigned long>(ObRandom::rand(0,INT64_MAX)));
      std::string update="UPDATE "+target+" SET ";bool first=true;
      for(size_t i=0;OB_SUCC(ret)&&i<columns.size();++i) {
        if(!row.changed[i]) continue;
        const auto &column=*columns[i];
        const auto *state=table.get_column_schema(column.get_ai_column_state_column_id());
        const auto *request=table.get_column_schema(column.get_ai_column_request_column_id());
        const auto *spec=table.get_column_schema(column.get_ai_column_result_spec_column_id());
        if(state==nullptr||request==nullptr||spec==nullptr) {ret=OB_SCHEMA_ERROR;break;}
        if(!first) update+=",";first=false;
        update+=ai_identifier(column.get_column_name_str())+"=NULL,"+ai_identifier(state->get_column_name_str())+
          "=0,"+ai_identifier(request->get_column_name_str())+"="+ai_literal(request_id)+","+
          ai_identifier(spec->get_column_name_str())+"="+ai_literal(row.snapshots[i]);
        share::ObDMLSqlSplicer dml;ObSqlString sql;
        if(OB_FAIL(dml.add_pk_column("request_id",ObHexEscapeSqlStr(ObString(request_id))))||
           OB_FAIL(dml.add_pk_column("column_id",column.get_column_id()))||
           OB_FAIL(dml.add_column("table_id",table.get_table_id()))||
           OB_FAIL(dml.add_column("definition_version",column.get_schema_version()))||
           OB_FAIL(dml.add_column("rowkey_predicate",ObHexEscapeSqlStr(ObString(row.predicate.size(),row.predicate.data()))))||
           OB_FAIL(dml.add_column("model_input",ObHexEscapeSqlStr(ObString(row.inputs[i].size(),row.inputs[i].data()))))||
           OB_FAIL(dml.add_column("group_size",group_size))||OB_FAIL(dml.add_column("max_retries",column.get_ai_column_max_retries()))||
           OB_FAIL(dml.add_column("status","PENDING"))||OB_FAIL(dml.add_time_column("eligible_ts",now))) {
        } else if(OB_FAIL(dml.splice_insert_sql("oceanbase.__all_ai_column_task",sql))) {
        } else {int64_t affected=0;ret=connection.execute_write(sql.string(),affected);}
      }
      if(OB_SUCC(ret)) {update+=" WHERE "+row.predicate;int64_t affected=0;ret=connection.execute_write(ObString(update.size(),update.data()),affected);}
      if(OB_FAIL(ret)) break;
    }
  }
  if(OB_SUCC(ret)) {
    std::string cleanup="DELETE FROM oceanbase.__all_ai_column_task WHERE table_id="+std::to_string(table.get_table_id())+
      " AND NOT EXISTS (SELECT 1 FROM "+target+" WHERE ";
    for(size_t i=0;i<columns.size();++i) {
      if(i) cleanup+=" OR ";
      cleanup+=ai_identifier(table.get_column_schema(columns[i]->get_ai_column_request_column_id())->get_column_name_str())+
          "=oceanbase.__all_ai_column_task.request_id";
    }
    cleanup+=")";int64_t affected=0;ret=connection.execute_write(ObString(cleanup.size(),cleanup.data()),affected);
  }
  return ret;
}
} }
