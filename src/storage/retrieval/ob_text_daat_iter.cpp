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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (param.base_param_->need_project_relevance()
             && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
  }
  return ret;
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  // Small score-free unions avoid the heap and virtual collector round for
  // every posting. Larger queries retain the O(log n) heap merge.
  if (IS_NOT_INIT || function_lookup_mode_
      || sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE != mode_flag_
      || iter_param_->need_project_relevance() || iter_param_->need_filter()
      || dim_iters_->count() > 8) {
    return ObSRDaaTIterImpl::get_next_rows(capacity, count);
  }
  return union_next_rows(capacity, count);
}

int ObTextDaaTIter::union_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  count = 0;
  if (capacity <= 0) {
  } else if (dim_iters_->count() == 0
             || (iter_param_->limit_param_->is_valid()
                 && output_row_cnt_ >= iter_param_->limit_param_->limit_)) {
    ret = OB_ITER_END;
  } else {
    const auto &meta = iter_param_->id_proj_expr_->datum_meta_;
    const ObDatumBasicFuncs *funcs = ObDatumFuncs::get_basic_func(meta.type_, meta.cs_type_);
    if (OB_ISNULL(funcs) || OB_ISNULL(funcs->null_first_cmp_)) {
      return OB_ERR_UNEXPECTED;
    }
    const auto compare = [&](const ObDatum &left, const ObDatum &right, int &result) {
      int cmp_ret = OB_SUCCESS;
      if (left.is_null() || right.is_null()) {
        cmp_ret = funcs->null_first_cmp_(left, right, result, nullptr);
      } else if (ObIntType == meta.type_) {
        const int64_t l = left.get_int(), r = right.get_int();
        result = l < r ? -1 : (l > r ? 1 : 0);
      } else if (ObUInt64Type == meta.type_) {
        const uint64_t l = left.get_uint64(), r = right.get_uint64();
        result = l < r ? -1 : (l > r ? 1 : 0);
      } else if (ObVarcharType == meta.type_ && CS_TYPE_BINARY == meta.cs_type_) {
        const int c = MEMCMP(left.ptr_, right.ptr_, MIN(left.len_, right.len_));
        result = c != 0 ? (c < 0 ? -1 : 1) : (left.len_ < right.len_ ? -1 : (left.len_ > right.len_ ? 1 : 0));
      } else {
        cmp_ret = funcs->null_first_cmp_(left, right, result, nullptr);
      }
      return cmp_ret;
    };
    const int64_t should_match = static_cast<ObSRDaaTInnerProductRelevanceCollector *>(
        relevance_collector_)->minimum_should_match();
    const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
    while (OB_SUCC(ret) && count < real_capacity) {
      for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
        const int64_t index = next_round_iter_idxes_[i];
        auto *iter = static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(index));
        if (iter->advance_cached_id(iter_domain_ids_[index])) {
        } else if (OB_FAIL(iter->get_next_row())) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
            iter_domain_ids_[index] = nullptr;
          }
        } else if (OB_FAIL(iter->get_curr_id(iter_domain_ids_[index]))) {
        }
      }
      next_round_cnt_ = 0;
      const ObDatum *minimum = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
        const ObDatum *current = iter_domain_ids_[i];
        int result = 0;
        if (nullptr == current) {
          continue;
        } else if (nullptr == minimum) {
          result = -1;
        } else if (OB_FAIL(compare(*current, *minimum, result))) {
        }
        if (OB_FAIL(ret)) {
        } else if (result < 0) {
          minimum = current;
          next_round_cnt_ = 0;
          next_round_iter_idxes_[next_round_cnt_++] = i;
        } else if (result == 0) {
          next_round_iter_idxes_[next_round_cnt_++] = i;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (nullptr == minimum) {
        ret = OB_ITER_END;
      } else if (next_round_cnt_ >= should_match
                 && OB_FAIL(ObSRDaaTIterImpl::cache_result(count, *minimum, 0.0))) {
      }
    }
    if ((OB_SUCC(ret) || OB_ITER_END == ret) && count > 0 && !direct_project_ && !iter_param_->count_only_) {
      const int scan_ret = ret;
      if (OB_FAIL(ObSRDaaTIterImpl::project_results(count))) {
      } else {
        ret = scan_ret;
      }
    }
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())
      || (OB_NOT_NULL(iter_param_) && !iter_param_->need_project_relevance())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance()) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
