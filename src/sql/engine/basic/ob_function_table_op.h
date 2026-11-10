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

#ifndef OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_
#define OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_chunk_datum_store.h"
#include "lib/charset/ob_charset.h"
#include "sql/engine/basic/file_sql/file_reader.h"

namespace oceanbase
{
namespace sql
{

class ObExpr;
class ObFunctionTableSpec : public ObOpSpec
{
OB_UNIS_VERSION_V(1);
public:
  ObFunctionTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type), value_expr_(nullptr), column_exprs_(alloc), has_correlated_expr_(false), file_scan_(false), file_column_ids_(alloc)
  {}
  ObExpr *value_expr_;
  common::ObFixedArray<ObExpr*, common::ObIAllocator> column_exprs_;
  bool has_correlated_expr_;
  bool file_scan_;
  common::ObFixedArray<int64_t, common::ObIAllocator> file_column_ids_;
};

class ObFunctionTableOp : public ObOperator
{
public:
  ObFunctionTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
    file_reader_(nullptr),
    file_row_allocator_("FileSqlRow"),
    node_idx_(0),
    already_calc_(false),
    row_count_(0),
    col_count_(0),
    value_table_(NULL)
  {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  //virtual int inner_get_next_batch(int64_t max_row_cnt) override;
  virtual int inner_close() override;
  virtual void destroy() override;
protected:
  int inner_get_next_row_file();
  file_sql::Reader *file_reader_;
  common::ObArenaAllocator file_row_allocator_;
  int inner_get_next_row_udf();
  int inner_get_next_row_sys_func();
  int get_current_result(common::ObObj &result);
  int64_t node_idx_;
  bool already_calc_;
  int64_t row_count_;
  int64_t col_count_;
  common::ObObj value_;
  pl::ObPLCollection *value_table_;
  int (ObFunctionTableOp::*next_row_func_)();
};

// Keep file scans vectorized without changing PL/UDF collection execution.
class FileScanSpec : public ObFunctionTableSpec
{
  OB_UNIS_VERSION_V(1);
public:
  FileScanSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObFunctionTableSpec(alloc, type) {}
};
class FileScanOp : public ObFunctionTableOp
{
public:
  FileScanOp(ObExecContext &ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObFunctionTableOp(ctx, spec, input) {}
  virtual int inner_get_next_batch(int64_t maximum) override;
};

} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_ */
