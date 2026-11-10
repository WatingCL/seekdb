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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "query/api/query/engine/expr/ob_expr_lob_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc,
                        T_FUN_SYS_AI_EMBED,
                        N_AI_EMBED,
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL,
                        NOT_ROW_DIMENSION)
{
}

ObExprAIEmbed::~ObExprAIEmbed()
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (ob_is_string_tc(types_stack[CONTENT_IDX].get_type())) {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
      types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    } else if (ob_is_text_tc(types_stack[CONTENT_IDX].get_type())) {
      // 输入支持 TEXT/LONGTEXT 等 LOB 类型, 保持原类型, 运行时读取真实数据
      types_stack[CONTENT_IDX].set_calc_type(types_stack[CONTENT_IDX].get_type());
    }
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("dimension parameter must be an integer, not a decimal or float", K(ret), K(types_stack[DIM_IDX].get_type()));
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
    LOG_WARN("evaluate parameters failed", K(ret));
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    // NULL 输入返回 NULL, 不视为错误
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);

    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content;
    if (model_id.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is empty");
      res.set_null();
    } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, temp_allocator,
                   *arg_content, expr.args_[CONTENT_IDX]->datum_meta_,
                   expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
    } else if (content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->is_null() ? 0 : arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("dimension parameter must be a positive integer", K(ret), K(dim));
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("AI endpoint resolver is unavailable", K(ret));
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                      const ObBitVector &skip, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObDatum *res_datum = expr.locate_batch_datums(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObDatumVector model_vec;
  ObDatumVector content_vec;
  if (OB_FAIL(expr.eval_batch_param_value(ctx, skip, batch_size, model_vec, content_vec))) {
  } else {
    ObDatumVector dim_vec;
    if (expr.arg_cnt_ == 3) {
      dim_vec = expr.locate_param_datumvector(ctx, DIM_IDX);
    }
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObArray<ObString> contents;
    ObArray<int64_t> row_indexes;
    ObArray<int64_t> group_indexes;
    ObArray<ObString> group_models;
    ObArray<int64_t> group_dimensions;
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
      if (skip.contain(i) || eval_flags.at(i)) {
        continue;
      } else if (model_vec.at(i)->is_null() || content_vec.at(i)->is_null()) {
        res_datum[i].set_null();
        eval_flags.set(i);
      } else {
        ObString model = model_vec.at(i)->get_string();
        ObString content;
        int64_t dimension = 0;
        if (expr.arg_cnt_ == 3) {
          if (dim_vec.at(i)->is_null() || (dimension = dim_vec.at(i)->get_int()) <= 0) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, temp_allocator,
                    *content_vec.at(i), expr.args_[CONTENT_IDX]->datum_meta_,
                    expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
        } else if (model.empty() || content.empty()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
        } else {
          int64_t group = 0;
          for (; group < group_models.count(); ++group) {
            if (model == group_models.at(group) && dimension == group_dimensions.at(group)) {
              break;
            }
          }
          if (group == group_models.count()) {
            if (OB_FAIL(group_models.push_back(model))) {
            } else if (OB_FAIL(group_dimensions.push_back(dimension))) {
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(contents.push_back(content))) {
          } else if (OB_FAIL(row_indexes.push_back(i))) {
          } else if (OB_FAIL(group_indexes.push_back(group))) {
          }
        }
      }
    }
    for (int64_t group = 0; OB_SUCC(ret) && group < group_models.count(); ++group) {
      ObArray<ObString> group_contents;
      ObArray<int64_t> group_rows;
      for (int64_t i = 0; OB_SUCC(ret) && i < contents.count(); ++i) {
        if (group_indexes.at(i) == group) {
          if (OB_FAIL(group_contents.push_back(contents.at(i)))) {
          } else if (OB_FAIL(group_rows.push_back(row_indexes.at(i)))) {
          }
        }
      }
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo resolved_endpoint;
      query::ObIAiEndpointResolver *endpoint_resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      ObJsonObject *config = nullptr;
      ObJsonInt *dim_json = nullptr;
      const int64_t dimension = group_dimensions.at(group);
      if (OB_SUCC(ret) && dimension > 0) {
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dimension, dim_json))) {
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, group_models.at(group), info))) {
      } else if (OB_ISNULL(endpoint_resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                     group_models.at(group), temp_allocator, resolved_endpoint))) {
      } else {
        ObAIFuncModel model(temp_allocator, *info, resolved_endpoint);
        ObArray<ObString> results;
        if (OB_FAIL(model.call_dense_embedding_vector_v2(group_contents, config, results))) {
        } else if (results.count() != group_contents.count()) {
          ret = OB_INVALID_DATA;
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < results.count(); ++i) {
            const int64_t row_idx = group_rows.at(i);
            const ObString &result = results.at(i);
            ObTextStringDatumResult output_result(expr.datum_meta_.type_, &expr, &ctx,
                                                   res_datum + row_idx);
            if (OB_FAIL(output_result.init_with_batch_idx(result.length(), row_idx))) {
            } else if (OB_FAIL(output_result.append(result))) {
            } else {
              output_result.set_result();
              eval_flags.set(row_idx);
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx,
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
