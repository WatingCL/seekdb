/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  /* Search scratch reused across calls: per-query device allocations and
   * parameter objects otherwise dominate single-row SQL latency. */
  pthread_mutex_t mu;
  float*    q_dev;  size_t q_cap;   /* elements */
  unsigned* n_dev;  float* d_dev;  size_t r_cap;
  cuvsCagraSearchParams_t sp;
  long sp_topk;
} sk_cuvs_index;

static void sk_tensor(DLManagedTensor* t, void* data, int on_device, uint8_t code,
                      int64_t* shape)
{
  memset(t, 0, sizeof(*t));
  t->dl_tensor.data = data;
  t->dl_tensor.device.device_type = on_device ? kDLCUDA : kDLCPU;
  t->dl_tensor.device.device_id = 0;
  t->dl_tensor.ndim = 2;
  t->dl_tensor.dtype.code = code;
  t->dl_tensor.dtype.bits = 32;
  t->dl_tensor.dtype.lanes = 1;
  t->dl_tensor.shape = shape;
}

static sk_cuvs_index* sk_alloc(void)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  pthread_mutex_init(&h->mu, NULL);
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { pthread_mutex_destroy(&h->mu); free(h); return NULL; }
  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) {
    cuvsResourcesDestroy(h->res); pthread_mutex_destroy(&h->mu); free(h); return NULL;
  }
  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) {
    cuvsCagraIndexParamsDestroy(h->ip); cuvsResourcesDestroy(h->res);
    pthread_mutex_destroy(&h->mu); free(h); return NULL;
  }
  return h;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = sk_alloc();
  if (!h) return NULL;
  DLManagedTensor dset; int64_t ds[2] = {n, dim};
  sk_tensor(&dset, (void*)base, 0, kDLFloat, ds);

  /* Degree-128 graph pruned from a degree-192 NN-Descent kNN graph. Measured
   * against the 256/100 baseline: the 192-wide kNN graph converges in 60
   * iterations to a graph that is at least as good on hard 256-d data (recall
   * up to +0.03) and within 0.002 on near-saturated data, at ~60% of the build
   * time (docs/gpu-vector-index-hipvs-cuvs/contest-build-optimizations.md). */
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 128;
  h->ip->intermediate_graph_degree = 192;
  h->ip->nn_descent_niter = 60;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
    seekdb_cuvs_free(h);
    return NULL;
  }
  h->n = n; h->dim = dim;
  return h;
}

static int sk_reserve(sk_cuvs_index* h, long nq, long topk)
{
  const size_t q = (size_t)nq * h->dim, r = (size_t)nq * topk;
  if (q > h->q_cap) {
    if (h->q_dev) cuvsRMMFree(h->res, h->q_dev, sizeof(float) * h->q_cap);
    h->q_dev = NULL; h->q_cap = 0;
    if (cuvsRMMAlloc(h->res, (void**)&h->q_dev, sizeof(float) * q) != CUVS_SUCCESS) return 10;
    h->q_cap = q;
  }
  if (r > h->r_cap) {
    if (h->n_dev) cuvsRMMFree(h->res, h->n_dev, sizeof(unsigned) * h->r_cap);
    if (h->d_dev) cuvsRMMFree(h->res, h->d_dev, sizeof(float) * h->r_cap);
    h->n_dev = NULL; h->d_dev = NULL; h->r_cap = 0;
    if (cuvsRMMAlloc(h->res, (void**)&h->n_dev, sizeof(unsigned) * r) != CUVS_SUCCESS) return 11;
    if (cuvsRMMAlloc(h->res, (void**)&h->d_dev, sizeof(float) * r) != CUVS_SUCCESS) {
      cuvsRMMFree(h->res, h->n_dev, sizeof(unsigned) * r); h->n_dev = NULL; return 12;
    }
    h->r_cap = r;
  }
  if (!h->sp || h->sp_topk != topk) {
    if (!h->sp && cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) { h->sp = 0; return 14; }
    size_t required_itopk = (size_t)topk * 4;
    if (required_itopk < 512) required_itopk = 512;
    h->sp->itopk_size = required_itopk;
    h->sp->search_width = 4;
    h->sp_topk = topk;
  }
  return 0;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  pthread_mutex_lock(&h->mu);
  int rc = sk_reserve(h, nq, topk);
  if (rc == 0 && cudaMemcpy(h->q_dev, query, sizeof(float) * nq * h->dim, cudaMemcpyDefault) != cudaSuccess) rc = 13;
  if (rc == 0) {
    DLManagedTensor qt, nt, dt;
    int64_t qs[2] = {nq, h->dim}, rs[2] = {nq, topk};
    sk_tensor(&qt, h->q_dev, 1, kDLFloat, qs);
    sk_tensor(&nt, h->n_dev, 1, kDLUInt, rs);
    sk_tensor(&dt, h->d_dev, 1, kDLFloat, rs);
    cuvsFilter f; f.type = NO_FILTER; f.addr = (uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) rc = 15;
    else if (cudaMemcpy(out_ids, h->n_dev, sizeof(unsigned) * nq * topk, cudaMemcpyDefault) != cudaSuccess) rc = 16;
    else if (out_dist && cudaMemcpy(out_dist, h->d_dev, sizeof(float) * nq * topk, cudaMemcpyDefault) != cudaSuccess) rc = 17;
  }
  pthread_mutex_unlock(&h->mu);
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  if (h->q_dev) cuvsRMMFree(h->res, h->q_dev, sizeof(float) * h->q_cap);
  if (h->n_dev) cuvsRMMFree(h->res, h->n_dev, sizeof(unsigned) * h->r_cap);
  if (h->d_dev) cuvsRMMFree(h->res, h->d_dev, sizeof(float) * h->r_cap);
  if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  pthread_mutex_destroy(&h->mu);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

static int sk_check_shape(sk_cuvs_index* h, long n, long dim)
{
  int64_t actual_n = 0, actual_dim = 0;
  if (cuvsCagraIndexGetSize(h->idx, &actual_n) != CUVS_SUCCESS ||
      cuvsCagraIndexGetDims(h->idx, &actual_dim) != CUVS_SUCCESS ||
      actual_n != n || actual_dim != dim) {
    fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
            (long)actual_n, (long)actual_dim, n, dim);
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* h = sk_alloc();
  if (!h) return NULL;
  if (cuvsCagraDeserialize(h->res, filename, h->idx) != CUVS_SUCCESS || sk_check_shape(h, n, dim)) {
    fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
    seekdb_cuvs_free(h);
    return NULL;
  }
  h->n = n;
  h->dim = dim;
  return h;
}

long seekdb_cuvs_graph_degree(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  int64_t degree = 0;
  if (!h || cuvsCagraIndexGetGraphDegree(h->idx, &degree) != CUVS_SUCCESS) return -1;
  return (long)degree;
}

int seekdb_cuvs_copy_graph(void* handle, unsigned int* out_graph)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !out_graph) return 1;
  const long degree = seekdb_cuvs_graph_degree(h);
  if (degree <= 0) return 2;
  DLManagedTensor graph;
  memset(&graph, 0, sizeof(graph));
  if (cuvsCagraIndexGetGraph(h->idx, &graph) != CUVS_SUCCESS) return 3;
  int rc = 0;
  if (graph.dl_tensor.ndim != 2 || graph.dl_tensor.shape[0] != h->n || graph.dl_tensor.shape[1] != degree) {
    rc = 4;
  } else if (cudaMemcpy(out_graph, graph.dl_tensor.data, sizeof(unsigned) * h->n * degree,
                        cudaMemcpyDefault) != cudaSuccess) {
    rc = 5;
  }
  if (graph.deleter) graph.deleter(&graph);
  if (rc) fprintf(stderr, "[seekdb_cuvs] copy graph rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void* seekdb_cuvs_from_graph(const float* base, long n, long dim,
                             const unsigned int* graph, long degree)
{
  if (!base || !graph || n <= 0 || dim <= 0 || degree <= 0) return NULL;
  sk_cuvs_index* h = sk_alloc();
  if (!h) return NULL;
  DLManagedTensor dset, gt;
  int64_t ds[2] = {n, dim}, gs[2] = {n, degree};
  sk_tensor(&dset, (void*)base, 0, kDLFloat, ds);
  sk_tensor(&gt, (void*)graph, 0, kDLUInt, gs);
  /* Host inputs are copied into index-owned device memory. */
  if (cuvsCagraIndexFromArgs(h->res, L2Expanded, &gt, &dset, h->idx) != CUVS_SUCCESS ||
      cuvsStreamSync(h->res) != CUVS_SUCCESS || sk_check_shape(h, n, dim)) {
    fprintf(stderr, "[seekdb_cuvs] load graph failed: %s\n", cuvsGetLastErrorText());
    seekdb_cuvs_free(h);
    return NULL;
  }
  h->n = n;
  h->dim = dim;
  return h;
}

int seekdb_cuvs_warmup(long dim)
{
  /* Initialise the device runtime and load the build/search kernels on a tiny
   * synthetic set, so the first real CREATE VECTOR INDEX does not pay for it. */
  if (dim <= 0 || dim > 4096) return 1;
  const long n = 4096;
  float* base = (float*)malloc(sizeof(float) * n * dim);
  if (!base) return 2;
  uint32_t s = 2463534242u;
  for (long i = 0; i < n * dim; ++i) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    base[i] = (float)(s >> 8) * (1.0f / 16777216.0f);
  }
  int rc = 3;
  void* h = seekdb_cuvs_build(base, n, dim);
  if (h) {
    unsigned ids[100]; float dist[100];
    rc = seekdb_cuvs_search(h, base, 1, 10, ids, dist) || seekdb_cuvs_search(h, base, 1, 100, ids, dist);
    seekdb_cuvs_free(h);
  }
  free(base);
  return rc;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
