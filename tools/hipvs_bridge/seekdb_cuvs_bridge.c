/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/nn_descent.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <pthread.h>
#include <stdbool.h>
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
  /* Pinned host staging, so a search is async copies plus one stream sync. */
  float*    q_host; unsigned* n_host; float* d_host;
  cuvsCagraSearchParams_t sp;
  long sp_topk;
  /* Device dataset and graph a device-built index refers to (not owned by cuVS). */
  float*    data_dev;
  unsigned* graph_dev;
} sk_cuvs_index;

/* seekdb_cuvs_graph.cu */
int seekdb_cuvs_optimize_graph(void* stream, const float* d_data, long n, long dim,
                               unsigned int* d_knn, long k_deg, unsigned int* d_out, long out_deg);

enum { SK_GRAPH_DEGREE = 128, SK_KNN_DEGREE = 128, SK_NND_ITERS = 30, SK_SEARCH_WIDTH = 64 };

#ifdef __HIP_PLATFORM_AMD__
#define SK_HOST_ALLOC(p, bytes) hipHostMalloc((void**)(p), (bytes), 0)
#define SK_HOST_FREE(p) hipHostFree(p)
#else
#define SK_HOST_ALLOC(p, bytes) cudaMallocHost((void**)(p), (bytes))
#define SK_HOST_FREE(p) cudaFreeHost(p)
#endif

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

/* NN-Descent kNN graph, then the CAGRA graph optimisation on the device
 * (seekdb_cuvs_graph.cu), then an index over the device dataset and graph.
 * Same graph construction as cuvsCagraBuild(NN_DESCENT) without its host round
 * trips and O(d^3) detour counting. Returns 0 on success. */
static int sk_build_device(sk_cuvs_index* h, const float* base, long n, long dim)
{
  cudaStream_t st;
  if (cuvsStreamGet(h->res, &st) != CUVS_SUCCESS) return 1;
  cuvsNNDescentIndexParams_t np = NULL;
  cuvsNNDescentIndex_t ni = NULL;
  unsigned* knn_dev = NULL;
  int rc = 0;
  DLManagedTensor dset; int64_t ds[2] = {n, dim};
  sk_tensor(&dset, (void*)base, 0, kDLFloat, ds);
  if (cuvsNNDescentIndexParamsCreate(&np) != CUVS_SUCCESS) { np = NULL; rc = 2; goto out; }
  np->metric = L2Expanded;
  np->graph_degree = SK_KNN_DEGREE;
  np->intermediate_graph_degree = SK_KNN_DEGREE * 3 / 2;  /* what cuvsCagraBuild passes */
  np->max_iterations = SK_NND_ITERS;
  np->return_distances = false;
  if (cuvsNNDescentIndexCreate(&ni) != CUVS_SUCCESS) { ni = NULL; rc = 3; goto out; }
  if (cuvsNNDescentBuild(h->res, np, &dset, NULL, ni) != CUVS_SUCCESS) { rc = 4; goto out; }
  if (cudaMalloc((void**)&h->data_dev, sizeof(float) * n * dim) != cudaSuccess) { h->data_dev = NULL; rc = 5; goto out; }
  if (cudaMalloc((void**)&knn_dev, sizeof(unsigned) * n * SK_KNN_DEGREE) != cudaSuccess) { knn_dev = NULL; rc = 6; goto out; }
  if (cudaMalloc((void**)&h->graph_dev, sizeof(unsigned) * n * SK_GRAPH_DEGREE) != cudaSuccess) { h->graph_dev = NULL; rc = 7; goto out; }
  if (cudaMemcpyAsync(h->data_dev, base, sizeof(float) * n * dim, cudaMemcpyDefault, st) != cudaSuccess) { rc = 8; goto out; }
  {
    DLManagedTensor kt; int64_t ks[2] = {n, SK_KNN_DEGREE};
    sk_tensor(&kt, knn_dev, 1, kDLUInt, ks);
    if (cuvsNNDescentIndexGetGraph(h->res, ni, &kt) != CUVS_SUCCESS) { rc = 9; goto out; }
  }
  cuvsNNDescentIndexDestroy(ni); ni = NULL;
  if ((rc = seekdb_cuvs_optimize_graph((void*)st, h->data_dev, n, dim, knn_dev, SK_KNN_DEGREE,
                                       h->graph_dev, SK_GRAPH_DEGREE)) != 0) { rc += 10; goto out; }
  cudaFree(knn_dev); knn_dev = NULL;
  {
    DLManagedTensor dt, gt; int64_t gs[2] = {n, SK_GRAPH_DEGREE};
    sk_tensor(&dt, h->data_dev, 1, kDLFloat, ds);
    sk_tensor(&gt, h->graph_dev, 1, kDLUInt, gs);
    if (cuvsCagraIndexFromArgs(h->res, L2Expanded, &gt, &dt, h->idx) != CUVS_SUCCESS ||
        cuvsStreamSync(h->res) != CUVS_SUCCESS) { rc = 30; goto out; }
  }
out:
  if (rc) fprintf(stderr, "[seekdb_cuvs] device graph build rc=%d: %s\n", rc, cuvsGetLastErrorText());
  if (ni) cuvsNNDescentIndexDestroy(ni);
  if (np) cuvsNNDescentIndexParamsDestroy(np);
  if (knn_dev) cudaFree(knn_dev);
  return rc;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = sk_alloc();
  if (!h) return NULL;
  if (n >= 4 * SK_KNN_DEGREE && !getenv("SEEKDB_CUVS_LIBRARY_BUILD")) {
    if (sk_build_device(h, base, n, dim) == 0) { h->n = n; h->dim = dim; return h; }
    seekdb_cuvs_free(h);  /* fall back to the library build on a fresh handle */
    if (!(h = sk_alloc())) return NULL;
  }
  DLManagedTensor dset; int64_t ds[2] = {n, dim};
  sk_tensor(&dset, (void*)base, 0, kDLFloat, ds);

  /* Degree-128 graph from a degree-128 NN-Descent kNN graph after 30
   * iterations. NN-Descent cost scales with degree x iterations and the 2-hop
   * pruning with degree^3; a 128-wide kNN graph converges faster than the
   * 256-wide baseline one and, on hard 256-d data, ends up better
   * (docs/gpu-vector-index-hipvs-cuvs/contest-build-optimizations.md). */
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = SK_GRAPH_DEGREE;
  h->ip->intermediate_graph_degree = SK_KNN_DEGREE;
  h->ip->nn_descent_niter = SK_NND_ITERS;
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
    if (h->q_host) SK_HOST_FREE(h->q_host);
    h->q_dev = NULL; h->q_host = NULL; h->q_cap = 0;
    if (cuvsRMMAlloc(h->res, (void**)&h->q_dev, sizeof(float) * q) != CUVS_SUCCESS) { h->q_dev = NULL; return 10; }
    if (SK_HOST_ALLOC(&h->q_host, sizeof(float) * q) != cudaSuccess) { h->q_host = NULL; return 10; }
    h->q_cap = q;
  }
  if (r > h->r_cap) {
    if (h->n_dev) cuvsRMMFree(h->res, h->n_dev, sizeof(unsigned) * h->r_cap);
    if (h->d_dev) cuvsRMMFree(h->res, h->d_dev, sizeof(float) * h->r_cap);
    if (h->n_host) SK_HOST_FREE(h->n_host);
    if (h->d_host) SK_HOST_FREE(h->d_host);
    h->n_dev = NULL; h->d_dev = NULL; h->n_host = NULL; h->d_host = NULL; h->r_cap = 0;
    if (cuvsRMMAlloc(h->res, (void**)&h->n_dev, sizeof(unsigned) * r) != CUVS_SUCCESS) { h->n_dev = NULL; return 11; }
    if (cuvsRMMAlloc(h->res, (void**)&h->d_dev, sizeof(float) * r) != CUVS_SUCCESS) { h->d_dev = NULL; return 12; }
    if (SK_HOST_ALLOC(&h->n_host, sizeof(unsigned) * r) != cudaSuccess) { h->n_host = NULL; return 12; }
    if (SK_HOST_ALLOC(&h->d_host, sizeof(float) * r) != cudaSuccess) { h->d_host = NULL; return 12; }
    h->r_cap = r;
  }
  if (!h->sp || h->sp_topk != topk) {
    if (!h->sp && cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) { h->sp = 0; return 14; }
    size_t required_itopk = (size_t)topk * 4;
    if (required_itopk < 512) required_itopk = 512;
    h->sp->itopk_size = required_itopk;
    /* Single-row searches run as multi-CTA, with max(search_width,
     * itopk/32) CTAs each searching from its own seeds; 64 CTAs keep a W7900
     * busy, finish sooner than 16 and return a higher recall. */
    h->sp_topk = topk;
  }
  /* Name the algorithm: left at its default, a single row runs the single-CTA
   * kernel, whose shared memory at width 64 exceeds the 64 KB a W7900 block
   * gets, so every search failed there. Large batches keep cuVS's own choice
   * and the baseline width. */
  h->sp->algo = nq <= 16 ? MULTI_CTA : AUTO;
  h->sp->search_width = nq <= 16 ? SK_SEARCH_WIDTH : 4;
  return 0;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  pthread_mutex_lock(&h->mu);
  cudaStream_t st;
  int rc = cuvsStreamGet(h->res, &st) == CUVS_SUCCESS ? sk_reserve(h, nq, topk) : 18;
  const size_t qb = sizeof(float) * nq * h->dim, rb = sizeof(unsigned) * nq * topk;
  if (rc == 0) {
    memcpy(h->q_host, query, qb);
    if (cudaMemcpyAsync(h->q_dev, h->q_host, qb, cudaMemcpyDefault, st) != cudaSuccess) rc = 13;
  }
  if (rc == 0) {
    DLManagedTensor qt, nt, dt;
    int64_t qs[2] = {nq, h->dim}, rs[2] = {nq, topk};
    sk_tensor(&qt, h->q_dev, 1, kDLFloat, qs);
    sk_tensor(&nt, h->n_dev, 1, kDLUInt, rs);
    sk_tensor(&dt, h->d_dev, 1, kDLFloat, rs);
    cuvsFilter f; f.type = NO_FILTER; f.addr = (uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) rc = 15;
    else if (cudaMemcpyAsync(h->n_host, h->n_dev, rb, cudaMemcpyDefault, st) != cudaSuccess) rc = 16;
    else if (out_dist && cudaMemcpyAsync(h->d_host, h->d_dev, rb, cudaMemcpyDefault, st) != cudaSuccess) rc = 17;
    else if (cudaStreamSynchronize(st) != cudaSuccess) rc = 19;
    else {
      memcpy(out_ids, h->n_host, rb);
      if (out_dist) memcpy(out_dist, h->d_host, rb);
    }
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
  if (h->q_host) SK_HOST_FREE(h->q_host);
  if (h->n_host) SK_HOST_FREE(h->n_host);
  if (h->d_host) SK_HOST_FREE(h->d_host);
  if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
  cuvsCagraIndexDestroy(h->idx);
  if (h->graph_dev) cudaFree(h->graph_dev);
  if (h->data_dev) cudaFree(h->data_dev);
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
