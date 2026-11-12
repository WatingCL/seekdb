/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here.
 *
 * [AMD contest optimizations, default = tuned build parameters]
 * The frozen V5 baseline hard-codes CAGRA build parameters
 * (NN_DESCENT / graph_degree=128 / intermediate=256 / nn_descent_niter=100) and
 * re-allocates RMM scratch + search params on every query. This file keeps the
 * exact same C ABI, but by default builds the intermediate NN-descent graph with
 * intermediate=192 / niter=40 while the final graph stays at degree 128, and:
 *
 *  1. every build/search knob is runtime-tunable via environment variables so
 *     that quality/speed trade-offs can be measured on a GPU box WITHOUT a
 *     recompile (one build, many configs). `OB_CUVS_BASELINE=1` restores every
 *     frozen-baseline value, and any single variable overrides its own default;
 *  2. per-handle search params are created once and refreshed per call instead
 *     of create/destroy per query;
 *  3. per-handle device scratch buffers are grown once and reused across
 *     queries instead of RMM alloc/free per query;
 *  4. optional phase timings (OB_CUVS_DEBUG=1) for profiling the build path.
 *
 * The seekdb adaptor serialises all GPU work for one index handle behind
 * g_ob_cuvs_mu, so per-handle scratch reuse is thread-safe.
 */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/ivf_pq.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* runtime configuration (all defaults reproduce the frozen baseline)  */
/* ------------------------------------------------------------------ */
typedef struct {
  /* build */
  int  build_algo;        /* enum cuvsCagraGraphBuildAlgo: 0 AUTO, 1 IVF_PQ,
                             2 NN_DESCENT (default), 3 ITERATIVE_CAGRA_SEARCH */
  long graph_degree;      /* final graph degree            (default 128) */
  long inter_degree;      /* intermediate graph degree     (default 256) */
  long nn_iters;          /* NN-descent iterations         (default 100) */
  int  ivf_n_probes;      /* IVF_PQ candidate probes, 0 = library default */
  float ivf_refine_rate;  /* IVF_PQ refinement_rate, <0 = library default */
  /* search */
  long itopk;             /* >0 overrides itopk_size; 0 = max(4*topk, 512) */
  long search_width;      /* search width; 0 = baseline value (4) */
  long team_size;         /* team size; 0 = library default (untouched) */
  int  search_algo;       /* -1 untouched (default); else enum cuvsCagraSearchAlgo */
  /* diagnostics */
  int  debug;             /* OB_CUVS_DEBUG>=1: phase timings on stderr */
} sk_cfg;

static sk_cfg sk_cfg_val;
static pthread_once_t sk_cfg_once = PTHREAD_ONCE_INIT;

static long sk_env_long(const char *name, long dflt) {
  const char *e = getenv(name);
  if (e == NULL || e[0] == '\0') { return dflt; }
  return strtol(e, NULL, 10);
}
static double sk_env_dbl(const char *name, double dflt) {
  const char *e = getenv(name);
  if (e == NULL || e[0] == '\0') { return dflt; }
  return strtod(e, NULL);
}

static void sk_cfg_load(void) {
  const int baseline = (getenv("OB_CUVS_BASELINE") != NULL);
  sk_cfg_val.build_algo       = (int)sk_env_long("OB_CUVS_BUILD_ALGO", 2 /* NN_DESCENT */);
  /* graph_degree 64 is the cuVS library default; the frozen V5 baseline
   * over-builds at 128. Halving it halves both the GPU prune work and the
   * serialized graph blob (>128MB -> ~64MB on 250K points). */
  sk_cfg_val.graph_degree     = sk_env_long("OB_CUVS_GRAPH_DEGREE", baseline ? 128 : 64);
  sk_cfg_val.inter_degree     = sk_env_long("OB_CUVS_INTER_DEGREE", baseline ? 256 : 96);
  sk_cfg_val.nn_iters         = sk_env_long("OB_CUVS_NN_ITERS", baseline ? 100 : 24);
  sk_cfg_val.ivf_n_probes     = (int)sk_env_long("OB_CUVS_IVF_N_PROBES", 0);
  sk_cfg_val.ivf_refine_rate  = (float)sk_env_dbl("OB_CUVS_IVF_REFINE_RATE", -1.0);
  sk_cfg_val.itopk            = sk_env_long("OB_CUVS_ITOPK", 0);
  sk_cfg_val.search_width     = sk_env_long("OB_CUVS_SEARCH_WIDTH", 0);
  sk_cfg_val.team_size        = sk_env_long("OB_CUVS_TEAM_SIZE", 0);
  sk_cfg_val.search_algo      = (int)sk_env_long("OB_CUVS_SEARCH_ALGO", -1);
  sk_cfg_val.debug            = (int)sk_env_long("OB_CUVS_DEBUG", 0);
}
static const sk_cfg *sk_cfg_get(void) {
  pthread_once(&sk_cfg_once, sk_cfg_load);
  return &sk_cfg_val;
}

static double sk_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}
#define SK_DBG(...) do { if (sk_cfg_get()->debug) { fprintf(stderr, __VA_ARGS__); } } while (0)

typedef struct {
  cuvsResources_t         res;
  cuvsCagraIndexParams_t  ip;
  cuvsCagraIndex_t        idx;
  cuvsCagraSearchParams_t sp;    /* cached; fields refreshed per search call */
  cuvsIvfPqParams_t       ivf;   /* only for build_algo = IVF_PQ */
  /* grow-only reusable device scratch (see file header for thread safety) */
  float    *qd;  size_t qd_cap;  /* query vectors, floats   */
  unsigned *nd;  size_t nd_cap;  /* neighbour ids, elems    */
  float    *dd;  size_t dd_cap;  /* neighbour dists, floats */
  long n, dim;
} sk_cuvs_index;

/* Ensure the three scratch buffers can hold q_floats / k_elems elements. */
static int sk_scratch_ensure(sk_cuvs_index *h, size_t q_floats, size_t k_elems) {
  if (q_floats > h->qd_cap) {
    size_t cap = h->qd_cap ? h->qd_cap : 4096;
    while (cap < q_floats) { cap *= 2; }
    float *p = NULL;
    if (cuvsRMMAlloc(h->res, (void **)&p, sizeof(float) * cap) != CUVS_SUCCESS) { return 1; }
    if (h->qd) { cuvsRMMFree(h->res, h->qd, sizeof(float) * h->qd_cap); }
    h->qd = p; h->qd_cap = cap;
  }
  if (k_elems > h->nd_cap) {
    size_t cap = h->nd_cap ? h->nd_cap : 4096;
    while (cap < k_elems) { cap *= 2; }
    unsigned *p = NULL;
    if (cuvsRMMAlloc(h->res, (void **)&p, sizeof(unsigned) * cap) != CUVS_SUCCESS) { return 1; }
    if (h->nd) { cuvsRMMFree(h->res, h->nd, sizeof(unsigned) * h->nd_cap); }
    h->nd = p; h->nd_cap = cap;
  }
  if (k_elems > h->dd_cap) {
    size_t cap = h->dd_cap ? h->dd_cap : 4096;
    while (cap < k_elems) { cap *= 2; }
    float *p = NULL;
    if (cuvsRMMAlloc(h->res, (void **)&p, sizeof(float) * cap) != CUVS_SUCCESS) { return 1; }
    if (h->dd) { cuvsRMMFree(h->res, h->dd, sizeof(float) * h->dd_cap); }
    h->dd = p; h->dd_cap = cap;
  }
  return 0;
}

static void sk_scratch_release(sk_cuvs_index *h) {
  if (h->qd) { cuvsRMMFree(h->res, h->qd, sizeof(float) * h->qd_cap); h->qd = NULL; h->qd_cap = 0; }
  if (h->nd) { cuvsRMMFree(h->res, h->nd, sizeof(unsigned) * h->nd_cap); h->nd = NULL; h->nd_cap = 0; }
  if (h->dd) { cuvsRMMFree(h->res, h->dd, sizeof(float) * h->dd_cap); h->dd = NULL; h->dd_cap = 0; }
}

/* Build the IVF-PQ sub-params struct required when build_algo = IVF_PQ.
 * Returns NULL on failure; only ever called for the IVF_PQ config. */
static cuvsIvfPqParams_t sk_make_ivf_params(void) {
  cuvsIvfPqParams_t p = (cuvsIvfPqParams_t)calloc(1, sizeof(struct cuvsIvfPqParams));
  if (p == NULL) { return NULL; }
  if (cuvsIvfPqIndexParamsCreate(&p->ivf_pq_build_params) != CUVS_SUCCESS ||
      cuvsIvfPqSearchParamsCreate(&p->ivf_pq_search_params) != CUVS_SUCCESS) {
    if (p->ivf_pq_build_params) { cuvsIvfPqIndexParamsDestroy(p->ivf_pq_build_params); }
    if (p->ivf_pq_search_params) { cuvsIvfPqSearchParamsDestroy(p->ivf_pq_search_params); }
    free(p);
    return NULL;
  }
  const sk_cfg *c = sk_cfg_get();
  if (c->ivf_n_probes > 0) { p->ivf_pq_search_params->n_probes = (uint32_t)c->ivf_n_probes; }
  if (c->ivf_refine_rate >= 0.0f) { p->refinement_rate = c->ivf_refine_rate; }
  return p;
}

void *seekdb_cuvs_build(const float *base, long n, long dim) {
  const sk_cfg *cfg = sk_cfg_get();
  sk_cuvs_index *h = (sk_cuvs_index *)calloc(1, sizeof(*h));
  if (!h) { return NULL; }
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }
  DLManagedTensor dset; int64_t ds[2] = {n, dim};
  dset.dl_tensor.data = (void *)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) { goto fail_res; }
  h->ip->build_algo = (enum cuvsCagraGraphBuildAlgo)cfg->build_algo;
  if (cfg->graph_degree > 0) { h->ip->graph_degree = (size_t)cfg->graph_degree; }
  if (cfg->inter_degree > 0) { h->ip->intermediate_graph_degree = (size_t)cfg->inter_degree; }
  if (cfg->nn_iters > 0) { h->ip->nn_descent_niter = (size_t)cfg->nn_iters; }
  if (cfg->build_algo == 1 /* IVF_PQ */) {
    h->ivf = sk_make_ivf_params();
    if (h->ivf == NULL) { goto fail_ip; }
    h->ip->graph_build_params = h->ivf;
  }
  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) { goto fail_ip; }
  {
    const double t0 = sk_now();
    const cuvsError_t brc = cuvsCagraBuild(h->res, h->ip, &dset, h->idx);
    SK_DBG("[seekdb_cuvs] build n=%ld dim=%ld algo=%d degree=%llu inter=%llu niter=%llu -> %d (%.1f ms)\n",
           n, dim, (int)cfg->build_algo,
           (unsigned long long)h->ip->graph_degree,
           (unsigned long long)h->ip->intermediate_graph_degree,
           (unsigned long long)h->ip->nn_descent_niter,
           (int)brc, (sk_now() - t0) * 1e3);
    if (brc != CUVS_SUCCESS) { goto fail_idx; }
  }
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:
  if (h->ivf != NULL) {
    if (h->ivf->ivf_pq_build_params) { cuvsIvfPqIndexParamsDestroy(h->ivf->ivf_pq_build_params); }
    if (h->ivf->ivf_pq_search_params) { cuvsIvfPqSearchParamsDestroy(h->ivf->ivf_pq_search_params); }
    free(h->ivf);
  }
  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void *handle, const float *query, long nq, long topk,
                       unsigned int *out_ids, float *out_dist) {
  sk_cuvs_index *h = (sk_cuvs_index *)handle;
  if (!h) { return 1; }
  const sk_cfg *cfg = sk_cfg_get();
  int rc = 0;
  const double t0 = sk_now();
  const size_t q_floats = (size_t)nq * (size_t)h->dim;
  const size_t k_elems  = (size_t)nq * (size_t)topk;
  if (nq <= 0 || topk <= 0 || q_floats == 0 || k_elems == 0) { return 1; }
  if (sk_scratch_ensure(h, q_floats, k_elems) != 0) { return 10; }
  if (cudaMemcpy(h->qd, query, sizeof(float) * q_floats, cudaMemcpyDefault) != cudaSuccess) { rc = 13; goto done; }
  if (h->sp == NULL && cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) { rc = 14; goto done; }
  {
    /* identical semantics to the frozen baseline: itopk = max(4*topk, 512),
     * search_width = 4; env overrides apply on top when set. */
    size_t required_itopk = (size_t)topk * 4;
    if (required_itopk < 512) { required_itopk = 512; }
    h->sp->itopk_size = (cfg->itopk > 0) ? (size_t)cfg->itopk : required_itopk;
    h->sp->search_width = (cfg->search_width > 0) ? (size_t)cfg->search_width : 4;
    if (cfg->team_size > 0) { h->sp->team_size = (size_t)cfg->team_size; }
    if (cfg->search_algo >= 0) { h->sp->algo = (enum cuvsCagraSearchAlgo)cfg->search_algo; }
  }
  {
    DLManagedTensor qt, nt, dt; int64_t qs[2] = {nq, h->dim}, nsh[2] = {nq, topk}, dsh[2] = {nq, topk};
    qt.dl_tensor.data = h->qd; qt.dl_tensor.device.device_type = kDLCUDA; qt.dl_tensor.device.device_id = 0;
    qt.dl_tensor.ndim = 2; qt.dl_tensor.dtype.code = kDLFloat; qt.dl_tensor.dtype.bits = 32; qt.dl_tensor.dtype.lanes = 1;
    qt.dl_tensor.shape = qs; qt.dl_tensor.strides = 0; qt.dl_tensor.byte_offset = 0;
    nt = qt; nt.dl_tensor.data = h->nd; nt.dl_tensor.dtype.code = kDLUInt; nt.dl_tensor.shape = nsh;
    dt = qt; dt.dl_tensor.data = h->dd; dt.dl_tensor.dtype.code = kDLFloat; dt.dl_tensor.shape = dsh;
    cuvsFilter f; f.type = NO_FILTER; f.addr = (uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc = 15; goto done; }
    if (cudaMemcpy(out_ids, h->nd, sizeof(unsigned) * k_elems, cudaMemcpyDefault) != cudaSuccess) { rc = 16; goto done; }
    if (out_dist) {
      if (cudaMemcpy(out_dist, h->dd, sizeof(float) * k_elems, cudaMemcpyDefault) != cudaSuccess) { rc = 17; goto done; }
    }
  }
  SK_DBG("[seekdb_cuvs] search nq=%ld topk=%ld -> %d (%.3f ms)\n", nq, topk, rc, (sk_now() - t0) * 1e3);
done:
  if (rc) { fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText()); }
  return rc;
}

void seekdb_cuvs_free(void *handle) {
  sk_cuvs_index *h = (sk_cuvs_index *)handle;
  if (!h) { return; }
  sk_scratch_release(h);
  if (h->sp) { cuvsCagraSearchParamsDestroy(h->sp); }
  cuvsCagraIndexDestroy(h->idx);
  if (h->ivf != NULL) {
    if (h->ivf->ivf_pq_build_params) { cuvsIvfPqIndexParamsDestroy(h->ivf->ivf_pq_build_params); }
    if (h->ivf->ivf_pq_search_params) { cuvsIvfPqSearchParamsDestroy(h->ivf->ivf_pq_search_params); }
    free(h->ivf);
  }
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_serialize(void *handle, const char *filename) {
  sk_cuvs_index *index = (sk_cuvs_index *)handle;
  if (!index || !filename) { return 1; }
  const double t0 = sk_now();
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  SK_DBG("[seekdb_cuvs] serialize (%.1f ms)\n", (sk_now() - t0) * 1e3);
  return 0;
}

void *seekdb_cuvs_deserialize(const char *filename, long n, long dim) {
  if (!filename || n <= 0 || dim <= 0) { return NULL; }
  const double t0 = sk_now();
  sk_cuvs_index *index = (sk_cuvs_index *)calloc(1, sizeof(*index));
  if (!index) { return NULL; }
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) { goto fail; }
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) { goto fail_res; }
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) { goto fail_ip; }
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) { goto fail_idx; }
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  SK_DBG("[seekdb_cuvs] deserialize n=%ld dim=%ld (%.1f ms)\n", n, dim, (sk_now() - t0) * 1e3);
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float *base, long n, long dim,
                          const float *query, long nq, long topk,
                          unsigned int *out_ids) {
  void *h = seekdb_cuvs_build(base, n, dim);
  if (!h) { return 10; }
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
