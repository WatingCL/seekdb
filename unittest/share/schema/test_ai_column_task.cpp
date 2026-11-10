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

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "share/ai_service/ai_column_task.h"

using namespace oceanbase::common;
using namespace oceanbase::share;

namespace
{
class CapturingTransaction : public ObMySQLTransaction
{
public:
  bool is_started() const override { return started_; }
  int write(const char *sql, const int32_t, int64_t &affected) override
  {
    statements_.emplace_back(sql);
    affected = affected_;
    return error_;
  }
  bool started_ = true;
  int error_ = OB_SUCCESS;
  int64_t affected_ = 1;
  std::vector<std::string> statements_;
};

AiColumnTask make_task(uint64_t column_id = 17)
{
  AiColumnTask task;
  task.table_id_ = 1001;
  task.column_id_ = column_id;
  task.definition_version_ = 123;
  task.request_id_ = ObString("request-1");
  task.rowkey_predicate_ = ObString("`id` = 7");
  task.model_input_ = ObString("content with 'quotes' and \n newline");
  task.group_size_ = 1;
  task.max_retries_ = 2;
  task.eligible_ts_ = 1000000;
  return task;
}
}

TEST(AiColumnTasks, RequiresBusinessTransaction)
{
  AiColumnTaskStore store;
  CapturingTransaction trans;
  trans.started_ = false;
  ObArray<AiColumnTask> group;
  ASSERT_EQ(OB_SUCCESS, group.push_back(make_task()));
  EXPECT_EQ(OB_INVALID_ARGUMENT, store.enqueue_group(trans, group));
  EXPECT_EQ(OB_INVALID_ARGUMENT, store.cancel_row(trans, 1001, ObString("`id` = 7")));
  EXPECT_TRUE(trans.statements_.empty());
}

TEST(AiColumnTasks, ValidatesWholeGroupBeforeWriting)
{
  AiColumnTaskStore store;
  CapturingTransaction trans;
  ObArray<AiColumnTask> group;
  AiColumnTask first = make_task();
  first.group_size_ = 2;
  ASSERT_EQ(OB_SUCCESS, group.push_back(first));
  AiColumnTask second = first;
  ASSERT_EQ(OB_SUCCESS, group.push_back(second));
  EXPECT_EQ(OB_INVALID_ARGUMENT, store.enqueue_group(trans, group));
  EXPECT_TRUE(trans.statements_.empty());
  group.at(1).column_id_ = 18;
  group.at(1).request_id_ = ObString("another-row-change");
  EXPECT_EQ(OB_INVALID_ARGUMENT, store.enqueue_group(trans, group));
  EXPECT_TRUE(trans.statements_.empty());
  group.at(1).request_id_ = first.request_id_;
  ASSERT_EQ(OB_SUCCESS, store.enqueue_group(trans, group));
  ASSERT_EQ(2, trans.statements_.size());
  EXPECT_NE(std::string::npos, trans.statements_[0].find("__all_ai_column_task"));
  EXPECT_EQ(std::string::npos, trans.statements_[0].find("content with 'quotes'"));
}

TEST(AiColumnTasks, WriteFailureIsPropagated)
{
  AiColumnTaskStore store;
  CapturingTransaction trans;
  trans.error_ = OB_TIMEOUT;
  ObArray<AiColumnTask> group;
  ASSERT_EQ(OB_SUCCESS, group.push_back(make_task()));
  EXPECT_EQ(OB_TIMEOUT, store.enqueue_group(trans, group));
  trans.error_ = OB_SUCCESS;
  trans.affected_ = 0;
  EXPECT_EQ(OB_STATE_NOT_MATCH, store.enqueue_group(trans, group));
}

TEST(AiColumnTasks, CompletionIsFencedAndRetriesAreFinite)
{
  AiColumnTaskStore store;
  CapturingTransaction trans;
  AiColumnTask claim = make_task();
  claim.state_ = AiColumnState::RUNNING;
  claim.lease_owner_ = ObString("worker-1");
  claim.lease_epoch_ = 9;
  claim.lease_expire_ts_ = 2000000;
  claim.attempts_ = 1;
  ASSERT_EQ(OB_SUCCESS, store.record_result(trans, claim, 1000000, false, ObString(),
                                          true, OB_TIMEOUT, 100));
  ASSERT_EQ(1, trans.statements_.size());
  const std::string &retry = trans.statements_[0];
  EXPECT_NE(std::string::npos, retry.find("PENDING"));
  EXPECT_NE(std::string::npos, retry.find("lease_epoch"));
  EXPECT_NE(std::string::npos, retry.find("lease_owner"));
  EXPECT_NE(std::string::npos, retry.find("definition_version"));
  EXPECT_NE(std::string::npos, retry.find("lease_expire_ts > 1000000"));
  EXPECT_EQ(std::string::npos, retry.find("AND  AND"));
  claim.attempts_ = 3;
  ASSERT_EQ(OB_SUCCESS, store.record_result(trans, claim, 1000000, false, ObString(),
                                          true, OB_TIMEOUT, 100));
  EXPECT_NE(std::string::npos, trans.statements_.back().find("FAILED"));
  EXPECT_EQ(OB_INVALID_ARGUMENT, store.record_result(trans, claim, 2000000, true,
                                                   ObString("result"), false, OB_SUCCESS, 0));
  EXPECT_EQ(2, trans.statements_.size());
}

TEST(AiColumnTasks, NullReadyIsDifferentFromPending)
{
  AiColumnTaskStore store;
  CapturingTransaction trans;
  AiColumnTask claim = make_task();
  claim.state_ = AiColumnState::RUNNING;
  claim.lease_owner_ = ObString("worker-1");
  claim.lease_epoch_ = 1;
  claim.lease_expire_ts_ = 2000000;
  claim.attempts_ = 1;
  ASSERT_EQ(OB_SUCCESS, store.record_result(trans, claim, 1000000, true, ObString(),
                                          true, OB_SUCCESS, 0));
  EXPECT_NE(std::string::npos, trans.statements_.back().find("READY"));
  EXPECT_NE(std::string::npos, trans.statements_.back().find("result_is_null"));
  claim.state_ = AiColumnState::READY;
  claim.result_is_null_ = true;
  EXPECT_TRUE(claim.is_terminal());
  claim.state_ = AiColumnState::PENDING;
  EXPECT_FALSE(claim.is_terminal());
}

TEST(AiColumnTasks, DeepCopyOwnsRequestAndInput)
{
  ObArenaAllocator allocator;
  char request[] = "request-1";
  char input[] = "original";
  AiColumnTask task = make_task();
  task.request_id_ = ObString(request);
  task.model_input_ = ObString(input);
  AiColumnTask copy;
  ASSERT_EQ(OB_SUCCESS, copy.deep_copy(allocator, task));
  request[0] = 'X';
  input[0] = 'Y';
  EXPECT_EQ(ObString("request-1"), copy.request_id_);
  EXPECT_EQ(ObString("original"), copy.model_input_);
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
