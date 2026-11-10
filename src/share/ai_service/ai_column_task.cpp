/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SHARE_SCHEMA
#include "share/ai_service/ai_column_task.h"
#include "share/ob_dml_sql_splicer.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/object/ob_object.h"
#include "lib/allocator/page_arena.h"
#include <limits>

using namespace oceanbase::common;

namespace oceanbase
{
namespace share
{
namespace
{
const char *const AI_COLUMN_TABLE = "oceanbase.__all_ai_column";
const char *const AI_COLUMN_TASK_TABLE = "oceanbase.__all_ai_column_task";

const char *state_name(AiColumnState state)
{
  switch (state) {
    case AiColumnState::PENDING: return "PENDING";
    case AiColumnState::RUNNING: return "RUNNING";
    case AiColumnState::READY: return "READY";
    case AiColumnState::FAILED: return "FAILED";
    default: return "INVALID";
  }
}

int append_hex_literal(ObSqlString &sql, const ObString &value)
{
  static const char HEX[] = "0123456789abcdef";
  int ret = sql.append("x'");
  char chunk[256];
  for (int64_t offset = 0; OB_SUCC(ret) && offset < value.length();) {
    int64_t count = min(static_cast<int64_t>(sizeof(chunk) / 2), value.length() - offset);
    for (int64_t i = 0; i < count; ++i) {
      const unsigned char c = static_cast<unsigned char>(value.ptr()[offset + i]);
      chunk[2 * i] = HEX[c >> 4];
      chunk[2 * i + 1] = HEX[c & 15];
    }
    ret = sql.append(chunk, count * 2);
    offset += count;
  }
  if (OB_SUCC(ret)) { ret = sql.append("'"); }
  return ret;
}

int write_one(ObISQLClient &client, const ObSqlString &sql)
{
  int64_t affected = 0;
  int ret = client.write(sql.ptr(), affected);
  if (OB_SUCC(ret) && affected != 1) { ret = OB_STATE_NOT_MATCH; }
  return ret;
}

int finish_transaction(ObMySQLTransaction &trans, int ret)
{
  if (trans.is_started()) {
    const int end_ret = trans.end(OB_SUCCESS == ret);
    if (OB_SUCCESS == ret) { ret = end_ret; }
  }
  return ret;
}

int copy_task_string(ObIAllocator &allocator, const ObString &source, ObString &target)
{
  return ob_write_string(allocator, source, target);
}
}

bool AiColumnDefinition::is_valid() const
{
  return table_id_ > 0 && column_id_ > 0 && definition_version_ > 0
      && !column_name_.empty() && !normalized_expr_.empty() && !result_type_.empty()
      && state_column_id_ > 0 && request_column_id_ > 0 && result_spec_column_id_ > 0
      && state_column_id_ != request_column_id_ && state_column_id_ != result_spec_column_id_
      && request_column_id_ != result_spec_column_id_ && column_id_ != state_column_id_
      && column_id_ != request_column_id_ && column_id_ != result_spec_column_id_
      && max_retries_ >= 0 && max_retries_ <= 100 && vector_dimension_ >= 0;
}

bool AiColumnTask::is_valid() const
{
  return table_id_ > 0 && column_id_ > 0 && definition_version_ > 0
      && !request_id_.empty() && request_id_.length() <= AiColumnTaskStore::MAX_REQUEST_ID_LENGTH
      && !rowkey_predicate_.empty() && model_input_.length() <= AiColumnTaskStore::MAX_MODEL_INPUT_LENGTH
      && group_size_ > 0 && group_size_ <= AiColumnTaskStore::MAX_GROUP_SIZE
      && attempts_ >= 0 && max_retries_ >= 0 && max_retries_ <= 100
      && lease_epoch_ >= 0 && eligible_ts_ > 0;
}

int AiColumnTask::deep_copy(ObIAllocator &allocator, const AiColumnTask &other)
{
  int ret = OB_SUCCESS;
  *this = other;
  if (OB_FAIL(copy_task_string(allocator, other.request_id_, request_id_))) {
  } else if (OB_FAIL(copy_task_string(allocator, other.rowkey_predicate_, rowkey_predicate_))) {
  } else if (OB_FAIL(copy_task_string(allocator, other.model_input_, model_input_))) {
  } else if (OB_FAIL(copy_task_string(allocator, other.result_, result_))) {
  } else if (OB_FAIL(copy_task_string(allocator, other.lease_owner_, lease_owner_))) {
  }
  return ret;
}

int AiColumnTaskStore::create_definition(ObMySQLTransaction &trans, const AiColumnDefinition &definition)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer dml;
  ObSqlString sql;
  if (!trans.is_started() || !definition.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(dml.add_pk_column("table_id", definition.table_id_))
      || OB_FAIL(dml.add_pk_column("column_id", definition.column_id_))
      || OB_FAIL(dml.add_column("definition_version", definition.definition_version_))
      || OB_FAIL(dml.add_column("column_name", ObHexEscapeSqlStr(definition.column_name_)))
      || OB_FAIL(dml.add_column("normalized_expr", ObHexEscapeSqlStr(definition.normalized_expr_)))
      || OB_FAIL(dml.add_column("dependency_column_ids", ObHexEscapeSqlStr(definition.dependency_column_ids_)))
      || OB_FAIL(dml.add_column("result_type", ObHexEscapeSqlStr(definition.result_type_)))
      || OB_FAIL(dml.add_column("state_column_id", definition.state_column_id_))
      || OB_FAIL(dml.add_column("request_column_id", definition.request_column_id_))
      || OB_FAIL(dml.add_column("result_spec_column_id", definition.result_spec_column_id_))
      || OB_FAIL(dml.add_column("max_retries", definition.max_retries_))
      || OB_FAIL(dml.add_column("vector_dimension", definition.vector_dimension_))) {
  } else if (OB_FAIL(dml.splice_insert_sql(AI_COLUMN_TABLE, sql))) {
  } else { ret = write_one(trans, sql); }
  return ret;
}

int AiColumnTaskStore::drop_table(ObMySQLTransaction &trans, uint64_t table_id)
{
  int ret = OB_SUCCESS;
  int64_t affected = 0;
  ObSqlString sql;
  if (!trans.is_started() || table_id == 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(sql.append_fmt("DELETE FROM %s WHERE table_id = %lu", AI_COLUMN_TASK_TABLE, table_id))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected))) {
  } else if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu", AI_COLUMN_TABLE, table_id))) {
  } else { ret = trans.write(sql.ptr(), affected); }
  return ret;
}

int AiColumnTaskStore::enqueue_group(ObMySQLTransaction &trans, const ObIArray<AiColumnTask> &group)
{
  int ret = OB_SUCCESS;
  if (!trans.is_started() || group.empty() || group.count() > MAX_GROUP_SIZE) {
    ret = OB_INVALID_ARGUMENT;
  }
  // Validate the whole group before performing any write. A caller must still
  // roll back its transaction on any SQL/storage error partway through enqueue.
  for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
    const AiColumnTask &task = group.at(i);
    if (!task.is_valid() || task.state_ != AiColumnState::PENDING || task.attempts_ != 0
        || task.lease_epoch_ != 0 || task.group_size_ != group.count()
        || task.table_id_ != group.at(0).table_id_ || task.request_id_ != group.at(0).request_id_
        || task.rowkey_predicate_ != group.at(0).rowkey_predicate_) {
      ret = OB_INVALID_ARGUMENT;
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < i; ++j) {
      if (group.at(j).column_id_ == task.column_id_) { ret = OB_INVALID_ARGUMENT; }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
    const AiColumnTask &task = group.at(i);
    ObDMLSqlSplicer dml;
    ObSqlString sql;
    if (OB_FAIL(dml.add_pk_column("request_id", ObHexEscapeSqlStr(task.request_id_)))
        || OB_FAIL(dml.add_pk_column("column_id", task.column_id_))
        || OB_FAIL(dml.add_column("table_id", task.table_id_))
        || OB_FAIL(dml.add_column("definition_version", task.definition_version_))
        || OB_FAIL(dml.add_column("rowkey_predicate", ObHexEscapeSqlStr(task.rowkey_predicate_)))
        || OB_FAIL(dml.add_column("model_input", ObHexEscapeSqlStr(task.model_input_)))
        || OB_FAIL(dml.add_column("group_size", task.group_size_))
        || OB_FAIL(dml.add_column("max_retries", task.max_retries_))
        || OB_FAIL(dml.add_column("status", "PENDING"))
        || OB_FAIL(dml.add_time_column("eligible_ts", task.eligible_ts_))) {
    } else if (OB_FAIL(dml.splice_insert_sql(AI_COLUMN_TASK_TABLE, sql))) {
    } else { ret = write_one(trans, sql); }
  }
  return ret;
}

int AiColumnTaskStore::cancel_row(ObMySQLTransaction &trans, uint64_t table_id,
                                const ObString &rowkey_predicate)
{
  int ret = OB_SUCCESS;
  int64_t affected = 0;
  ObSqlString sql;
  if (!trans.is_started() || table_id == 0 || rowkey_predicate.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(sql.append_fmt("DELETE FROM %s WHERE table_id = %lu AND rowkey_predicate = ",
                                  AI_COLUMN_TASK_TABLE, table_id))) {
  } else if (OB_FAIL(append_hex_literal(sql, rowkey_predicate))) {
  } else { ret = trans.write(sql.ptr(), affected); }
  return ret;
}

int AiColumnTaskStore::read_task(sqlclient::ObMySQLResult &result, ObIAllocator &allocator,
                               AiColumnTask &task)
{
  int ret = OB_SUCCESS;
  AiColumnTask tmp;
  ObString state;
  int64_t table_id = 0;
  int64_t column_id = 0;
  int64_t is_null = 0;
  if (OB_FAIL(result.get_int("table_id", table_id))
      || OB_FAIL(result.get_int("column_id", column_id))
      || OB_FAIL(result.get_int("definition_version", tmp.definition_version_))
      || OB_FAIL(result.get_varchar("request_id", tmp.request_id_))
      || OB_FAIL(result.get_varchar("rowkey_predicate", tmp.rowkey_predicate_))
      || OB_FAIL(result.get_varchar("model_input", tmp.model_input_))
      || OB_FAIL(result.get_varchar("result", tmp.result_))
      || OB_FAIL(result.get_varchar("lease_owner", tmp.lease_owner_))
      || OB_FAIL(result.get_varchar("status", state))
      || OB_FAIL(result.get_int("group_size", tmp.group_size_))
      || OB_FAIL(result.get_int("attempts", tmp.attempts_))
      || OB_FAIL(result.get_int("max_retries", tmp.max_retries_))
      || OB_FAIL(result.get_int("lease_epoch", tmp.lease_epoch_))
      || OB_FAIL(result.get_int("lease_expire_us", tmp.lease_expire_ts_))
      || OB_FAIL(result.get_int("eligible_us", tmp.eligible_ts_))
      || OB_FAIL(result.get_int("error_code", tmp.error_code_))
      || OB_FAIL(result.get_int("result_is_null", is_null))) {
  } else {
    tmp.table_id_ = table_id;
    tmp.column_id_ = column_id;
    tmp.result_is_null_ = is_null != 0;
    if (state == "PENDING") { tmp.state_ = AiColumnState::PENDING; }
    else if (state == "RUNNING") { tmp.state_ = AiColumnState::RUNNING; }
    else if (state == "READY") { tmp.state_ = AiColumnState::READY; }
    else if (state == "FAILED") { tmp.state_ = AiColumnState::FAILED; }
    else { ret = OB_INVALID_DATA; }
    if (OB_SUCC(ret) && !tmp.is_valid()) { ret = OB_INVALID_DATA; }
    if (OB_SUCC(ret)) { ret = task.deep_copy(allocator, tmp); }
  }
  return ret;
}

int AiColumnTaskStore::claim_one(ObISQLClient &proxy, const ObString &owner,
                               int64_t now, int64_t lease_us, ObIAllocator &allocator,
                               AiColumnTask &task, bool &found)
{
  int ret = OB_SUCCESS;
  found = false;
  ObMySQLTransaction trans;
  ObSqlString sql;
  ObISQLClient::ReadResult result;
  sqlclient::ObMySQLResult *rows = nullptr;
  if (owner.empty() || owner.length() > MAX_LEASE_OWNER_LENGTH || now <= 0 || lease_us <= 0
      || now > std::numeric_limits<int64_t>::max() - lease_us) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(trans.start(&proxy))) {
  } else if (OB_FAIL(sql.append_fmt(
      "SELECT *, time_to_usec(eligible_ts) AS eligible_us, lease_expire_ts AS lease_expire_us "
      "FROM %s WHERE (status = 'PENDING' AND eligible_ts <= usec_to_time(%ld)) "
      "OR (status = 'RUNNING' AND lease_expire_ts <= %ld) "
      "ORDER BY eligible_ts, request_id, column_id LIMIT 1 FOR UPDATE SKIP LOCKED",
      AI_COLUMN_TASK_TABLE, now, now))) {
  } else if (OB_FAIL(trans.read(result, sql.ptr()))) {
  } else if (OB_ISNULL(rows = result.get_result())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(rows->next())) {
    if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  } else if (OB_FAIL(read_task(*rows, allocator, task))) {
  } else {
    // The read result must be closed before another statement uses this connection.
    result.reset();
    const bool exhausted = task.attempts_ > task.max_retries_;
    ObDMLSqlSplicer dml;
    sql.reuse();
    if (OB_FAIL(dml.add_pk_column("request_id", ObHexEscapeSqlStr(task.request_id_)))
        || OB_FAIL(dml.add_pk_column("column_id", task.column_id_))
        || OB_FAIL(dml.add_pk_column("lease_epoch", task.lease_epoch_))
        || OB_FAIL(dml.add_column("status", exhausted ? "FAILED" : "RUNNING"))
        || OB_FAIL(dml.add_column("lease_epoch", task.lease_epoch_ + 1))
        || OB_FAIL(dml.add_column("lease_owner", ObHexEscapeSqlStr(owner)))
        || OB_FAIL(dml.add_column("lease_expire_ts", now + lease_us))
        || OB_FAIL(dml.add_column("attempts", task.attempts_ + (exhausted ? 0 : 1)))
        || OB_FAIL(dml.add_column("error_code", exhausted ? OB_TIMEOUT : 0))) {
    } else if (OB_FAIL(dml.splice_update_sql(AI_COLUMN_TASK_TABLE, sql))) {
    } else if (OB_FAIL(write_one(trans, sql))) {
    } else if (!exhausted) {
      task.state_ = AiColumnState::RUNNING;
      task.attempts_++;
      task.lease_epoch_++;
      task.lease_expire_ts_ = now + lease_us;
      ret = copy_task_string(allocator, owner, task.lease_owner_);
      found = OB_SUCCESS == ret;
    }
  }
  result.reset();
  ret = finish_transaction(trans, ret);
  if (OB_FAIL(ret)) { found = false; }
  return ret;
}

int AiColumnTaskStore::record_result(ObMySQLTransaction &trans, const AiColumnTask &claim,
                                   int64_t now, bool success, const ObString &result,
                                   bool result_is_null, int64_t error_code, int64_t retry_delay_us)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer dml;
  ObSqlString sql;
  const bool retry = !success && claim.attempts_ <= claim.max_retries_;
  if (!trans.is_started() || !claim.is_valid() || claim.state_ != AiColumnState::RUNNING
      || claim.lease_owner_.empty() || claim.lease_epoch_ <= 0 || now <= 0
      || now >= claim.lease_expire_ts_ || retry_delay_us < 0
      || now > std::numeric_limits<int64_t>::max() - retry_delay_us
      || (success && error_code != OB_SUCCESS) || (!success && error_code == OB_SUCCESS)
      || result.length() > MAX_MODEL_INPUT_LENGTH) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(dml.add_pk_column("request_id", ObHexEscapeSqlStr(claim.request_id_)))
      || OB_FAIL(dml.add_pk_column("table_id", claim.table_id_))
      || OB_FAIL(dml.add_pk_column("column_id", claim.column_id_))
      || OB_FAIL(dml.add_pk_column("definition_version", claim.definition_version_))
      || OB_FAIL(dml.add_pk_column("status", "RUNNING"))
      || OB_FAIL(dml.add_pk_column("lease_owner", ObHexEscapeSqlStr(claim.lease_owner_)))
      || OB_FAIL(dml.add_pk_column("lease_epoch", claim.lease_epoch_))
      || OB_FAIL(dml.add_column("status", state_name(success ? AiColumnState::READY
                                                          : retry ? AiColumnState::PENDING : AiColumnState::FAILED)))
      || OB_FAIL(dml.add_column("result", ObHexEscapeSqlStr(success ? result : ObString())))
      || OB_FAIL(dml.add_column("result_is_null", !success || result_is_null))
      || OB_FAIL(dml.add_column("error_code", error_code))
      || OB_FAIL(dml.add_time_column("eligible_ts", now + retry_delay_us))
      || OB_FAIL(dml.get_extra_condition().append_fmt("lease_expire_ts > %ld", now))) {
  } else if (OB_FAIL(dml.splice_update_sql(AI_COLUMN_TASK_TABLE, sql))) {
  } else { ret = write_one(trans, sql); }
  return ret;
}

int AiColumnTaskStore::publish_group(ObISQLClient &proxy, const ObString &request_id,
                                   IAiColumnPublisher &publisher, bool &published)
{
  int ret = OB_SUCCESS;
  published = false;
  ObMySQLTransaction trans;
  ObArenaAllocator allocator;
  ObArray<AiColumnTask> group;
  ObSqlString sql;
  ObISQLClient::ReadResult result;
  sqlclient::ObMySQLResult *rows = nullptr;
  bool terminal = true;
  bool current_definitions = true;
  bool applied = false;
  if (request_id.empty() || request_id.length() > MAX_REQUEST_ID_LENGTH) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(trans.start(&proxy))) {
  } else if (OB_FAIL(sql.append_fmt(
      "SELECT *, time_to_usec(eligible_ts) AS eligible_us, lease_expire_ts AS lease_expire_us FROM %s WHERE request_id = ",
      AI_COLUMN_TASK_TABLE))) {
  } else if (OB_FAIL(append_hex_literal(sql, request_id))) {
  } else if (OB_FAIL(sql.append(" ORDER BY column_id FOR UPDATE"))) {
  } else if (OB_FAIL(trans.read(result, sql.ptr()))) {
  } else if (OB_ISNULL(rows = result.get_result())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    while (OB_SUCC(ret) && OB_SUCC(ret = rows->next())) {
      AiColumnTask task;
      if (OB_FAIL(read_task(*rows, allocator, task))) {
      } else if (group.count() >= MAX_GROUP_SIZE) {
        ret = OB_SIZE_OVERFLOW;
      } else if (OB_FAIL(group.push_back(task))) {
      } else { terminal = terminal && task.is_terminal(); }
    }
    if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
  }
  result.reset();
  if (OB_SUCC(ret) && terminal && !group.empty()) {
    if (group.count() != group.at(0).group_size_) { ret = OB_INVALID_DATA; }
    for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
      const AiColumnTask &task = group.at(i);
      if (task.table_id_ != group.at(0).table_id_ || task.rowkey_predicate_ != group.at(0).rowkey_predicate_
          || task.group_size_ != group.count()) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(sql.assign_fmt("SELECT definition_version FROM %s WHERE table_id = %lu AND column_id = %lu FOR UPDATE",
                                        AI_COLUMN_TABLE, task.table_id_, task.column_id_))) {
      } else if (OB_FAIL(trans.read(result, sql.ptr()))) {
      } else if (OB_ISNULL(rows = result.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(rows->next())) {
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; current_definitions = false; }
      } else {
        int64_t version = 0;
        if (OB_FAIL(rows->get_int("definition_version", version))) {
        } else { current_definitions = current_definitions && version == task.definition_version_; }
      }
      result.reset();
    }
    if (OB_SUCC(ret) && current_definitions) {
      ret = publisher.publish(trans, group, applied);
    }
    // Superseded definitions/deleted rows are discarded; no late result can
    // resurrect them. Data writeback and task cleanup share this transaction.
    if (OB_SUCC(ret)) {
      int64_t affected = 0;
      if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE request_id = ", AI_COLUMN_TASK_TABLE))) {
      } else if (OB_FAIL(append_hex_literal(sql, request_id))) {
      } else if (OB_FAIL(trans.write(sql.ptr(), affected))) {
      } else if (affected != group.count()) { ret = OB_STATE_NOT_MATCH; }
    }
  }
  ret = finish_transaction(trans, ret);
  published = OB_SUCCESS == ret && applied;
  return ret;
}

} // namespace share
} // namespace oceanbase
