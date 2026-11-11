/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/ai_column/ai_column_runtime.h"
#include "sql/ai_column/ai_column_sql_utils.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/resolver/ob_resolver_define.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/parser/ob_parser.h"

namespace oceanbase { namespace sql {
using namespace common;
namespace {
// Replace only an unqualified, unquoted status reference in this RETRY's
// predicate. Quoted identifiers retain normal SQL business-column semantics.
int replace_ai_status(ParseNode *&node,ParseNode *replacement,int depth=0)
{
  if(node==nullptr) return OB_SUCCESS;
  if(depth>256) return OB_SIZE_OVERFLOW;
  if(node->type_==T_COLUMN_REF && node->num_child_==3 && node->children_[0]==nullptr && node->children_[1]==nullptr) {
    const auto *name=node->children_[2];
    if(name!=nullptr && !name->is_input_quoted_ && ObString(name->str_len_,name->str_value_).case_compare("status")==0) {
      node=replacement;return OB_SUCCESS;
    }
  }
  for(int64_t i=0;i<node->num_child_;++i) {
    int ret=replace_ai_status(node->children_[i],replacement,depth+1);if(ret!=OB_SUCCESS) return ret;
  }
  return OB_SUCCESS;
}
}
int resolve_ai_column_retry(ObResolverParams &params,const ParseNode &tree,ObStmt *&stmt)
{
  int ret=OB_SUCCESS;const auto *actions=tree.children_[1];
  if(actions->num_child_!=1) {LOG_USER_ERROR(OB_NOT_SUPPORTED,"combining AI COLUMN RETRY with another ALTER action");return OB_NOT_SUPPORTED;}
  const auto *retry=actions->children_[0];const auto *verb=retry->children_[0];
  if(ObString(verb->str_len_,verb->str_value_).case_compare("retry")!=0) return OB_NOT_SUPPORTED;
  const auto *relation=tree.children_[0];
  if(relation==nullptr||relation->num_child_<2||relation->children_[1]==nullptr) return OB_ERR_UNEXPECTED;
  const auto *dbnode=relation->children_[0];const auto *tablenode=relation->children_[1];
  ObString db=dbnode==nullptr ? params.session_info_->get_database_name() : ObString(dbnode->str_len_,dbnode->str_value_);
  ObString name(tablenode->str_len_,tablenode->str_value_);
  const share::schema::ObTableSchema *table=nullptr;
  if(OB_FAIL(params.schema_checker_->get_table_schema(db,name,false,table))) return ret;
  if(table==nullptr) return OB_TABLE_NOT_EXIST;
  const auto *colnode=retry->children_[1];
  const auto *column=table->get_column_schema(ObString(colnode->str_len_,colnode->str_value_));
  if(column==nullptr||!column->is_ai_column()) {LOG_USER_ERROR(OB_NOT_SUPPORTED,"retrying a column that is not an AI column");return OB_NOT_SUPPORTED;}
  const auto *state=table->get_column_schema(column->get_ai_column_state_column_id());
  const auto *request=table->get_column_schema(column->get_ai_column_request_column_id());
  if(state==nullptr||request==nullptr) return OB_SCHEMA_ERROR;
  const std::string update="UPDATE "+ai_identifier(db)+"."+ai_identifier(name)+" SET "+ai_identifier(column->get_column_name_str())+
      "=NULL,"+ai_identifier(state->get_column_name_str())+"=0,"+ai_identifier(request->get_column_name_str())+"=NULL";
  const std::string status="SELECT CASE "+ai_identifier(state->get_column_name_str())+
    " WHEN 2 THEN 'READY' WHEN 3 THEN 'FAILED' ELSE 'PENDING' END FROM "+ai_identifier(db)+"."+ai_identifier(name)+" WHERE 1";
  ObParser parser(*params.allocator_,params.session_info_->get_sql_mode());ParseResult parsed,expression;
  if(OB_FAIL(parser.parse(ObString(update.size(),update.data()),parsed))) return ret;
  auto *update_tree=parsed.result_tree_->children_[0];
  if(retry->children_[2]!=nullptr) {
    if(OB_FAIL(parser.parse(ObString(status.size(),status.data()),expression))) return ret;
    // Parse an expression separately to avoid relying on SELECT's projection layout.
    const std::string dummy="UPDATE "+ai_identifier(db)+"."+ai_identifier(name)+" SET "+ai_identifier(state->get_column_name_str())+
      "=0 WHERE (CASE "+ai_identifier(state->get_column_name_str())+" WHEN 2 THEN 'READY' WHEN 3 THEN 'FAILED' ELSE 'PENDING' END)";
    if(OB_FAIL(parser.parse(ObString(dummy.size(),dummy.data()),expression))) return ret;
    auto *replacement=expression.result_tree_->children_[0]->children_[3]->children_[0];
    update_tree->children_[3]=retry->children_[2];
    if(OB_FAIL(replace_ai_status(update_tree->children_[3],replacement))) return ret;
  }
  AiColumnInternalGuard internal(params.session_info_);ObResolver resolver(params);
  ret=resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,*update_tree,stmt);
  return ret;
}
} }
