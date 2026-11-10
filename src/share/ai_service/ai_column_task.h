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

#pragma once

#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/container/ob_array.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace share
{

// Result states are independent of SQL NULL. A READY result may itself be NULL.
enum class AiColumnState : int64_t { PENDING = 0, RUNNING = 1, READY = 2, FAILED = 3 };

struct AiColumnDefinition
{
  uint64_t table_id_ = 0;
  uint64_t column_id_ = 0;
  int64_t definition_version_ = 0;
  common::ObString column_name_;
  common::ObString normalized_expr_;
  common::ObString dependency_column_ids_;
  common::ObString result_type_;
  uint64_t state_column_id_ = 0;
  uint64_t request_column_id_ = 0;
  uint64_t result_spec_column_id_ = 0;
  int64_t max_retries_ = 2;
  int64_t vector_dimension_ = 0;
  bool is_valid() const;
};

struct AiColumnTask
{
  uint64_t table_id_ = 0;
  uint64_t column_id_ = 0;
  int64_t definition_version_ = 0;
  common::ObString request_id_;
  common::ObString rowkey_predicate_;
  common::ObString model_input_;
  common::ObString result_;
  common::ObString lease_owner_;
  AiColumnState state_ = AiColumnState::PENDING;
  int64_t group_size_ = 0;
  int64_t attempts_ = 0;
  int64_t max_retries_ = 0;
  int64_t lease_epoch_ = 0;
  int64_t lease_expire_ts_ = 0;
  int64_t eligible_ts_ = 0;
  int64_t error_code_ = 0;
  bool result_is_null_ = true;

  bool is_valid() const;
  bool is_terminal() const { return state_ == AiColumnState::READY || state_ == AiColumnState::FAILED; }
  int deep_copy(common::ObIAllocator &allocator, const AiColumnTask &other);
};

// The DML adapter must use the supplied transaction and compare every row's
// hidden request id with request_id_. It must NOT open a second transaction.
// A deleted/replaced row or a superseded request returns applied=false.
class IAiColumnPublisher
{
public:
  virtual ~IAiColumnPublisher() = default;
  virtual int publish(common::ObMySQLTransaction &trans,
                      const common::ObIArray<AiColumnTask> &group,
                      bool &applied) = 0;
};

// All business-DML operations require an already-started transaction. This API
// deliberately cannot enqueue via an autocommit client: rollback of the base
// row must also roll back invalidation, task creation and cancellation.
class AiColumnTaskStore
{
public:
  static const int64_t MAX_REQUEST_ID_LENGTH = 64;
  static const int64_t MAX_LEASE_OWNER_LENGTH = 128;
  static const int64_t MAX_GROUP_SIZE = 1024;
  static const int64_t MAX_MODEL_INPUT_LENGTH = 16 * 1024 * 1024;

  int create_definition(common::ObMySQLTransaction &trans, const AiColumnDefinition &definition);
  int drop_table(common::ObMySQLTransaction &trans, uint64_t table_id);
  int enqueue_group(common::ObMySQLTransaction &trans, const common::ObIArray<AiColumnTask> &group);
  int cancel_row(common::ObMySQLTransaction &trans, uint64_t table_id,
                 const common::ObString &rowkey_predicate);

  // Claims only committed rows. SKIP LOCKED prevents duplicate claims; epochs
  // prevent a timed-out worker from completing a task re-leased to another one.
  int claim_one(common::ObISQLClient &proxy, const common::ObString &owner,
                int64_t now, int64_t lease_us, common::ObIAllocator &allocator,
                AiColumnTask &task, bool &found);
  int record_result(common::ObMySQLTransaction &trans, const AiColumnTask &claim,
                    int64_t now, bool success, const common::ObString &result,
                    bool result_is_null, int64_t error_code, int64_t retry_delay_us);
  int publish_group(common::ObISQLClient &proxy, const common::ObString &request_id,
                    IAiColumnPublisher &publisher, bool &published);

private:
  int read_task(common::sqlclient::ObMySQLResult &result, common::ObIAllocator &allocator,
                AiColumnTask &task);
};

} // namespace share
} // namespace oceanbase
