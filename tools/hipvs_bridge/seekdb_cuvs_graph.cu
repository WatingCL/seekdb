/* Device-side CAGRA graph optimisation for the seekdb bridge.
 *
 * Turns an all-neighbours kNN graph into a CAGRA search graph with the same
 * steps as cuvs::neighbors::cagra::detail::graph::optimize (no MST pass, as
 * cuvsCagraBuild uses by default):
 *   1. re-sort every neighbour list by exact fp32 L2 distance,
 *   2. count 2-hop detours for every edge and keep, per node, the
 *      `out_degree` edges with the fewest detours (ties by rank),
 *   3. build the reverse graph and splice reverse edges into the second half
 *      of every list.
 * cuVS does steps 2-3 with host round trips, OpenMP loops and an O(d^3)
 * detour kernel; here everything stays on the device and the detour count is
 * O(d^2) per node through a per-block hash of the neighbour ranks. */
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#define SK_SHFL_XOR(v, o) __shfl_xor((v), (o))
#else
#include <cuda_runtime.h>
#define SK_SHFL_XOR(v, o) __shfl_xor_sync(0xffffffffu, (v), (o))
#endif
#include <float.h>
#include <stdint.h>
#include <stdio.h>

namespace {

constexpr unsigned SK_EMPTY = 0xffffffffu;
constexpr int SK_THREADS = 256;
constexpr int SK_TEAM = 32;  // lanes per distance; xor shuffles stay inside 32 lanes on wave32 and wave64

template <typename K>
__device__ void sk_bitonic_sort(K* key, unsigned* val, int p)
{
  for (int size = 2; size <= p; size <<= 1) {
    for (int stride = size >> 1; stride > 0; stride >>= 1) {
      for (int i = threadIdx.x; i < p; i += blockDim.x) {
        const int j = i ^ stride;
        if (j > i) {
          const bool up = (i & size) == 0;
          if ((key[i] > key[j]) == up) {
            K tk = key[i]; key[i] = key[j]; key[j] = tk;
            if (val) { unsigned tv = val[i]; val[i] = val[j]; val[j] = tv; }
          }
        }
      }
      __syncthreads();
    }
  }
}

/* One block per node: exact distances to its K neighbours, then sort. */
template <int P>
__global__ void sk_sort_kernel(const float* __restrict__ data, unsigned n, unsigned dim,
                               unsigned* __restrict__ knn, unsigned k_deg)
{
  extern __shared__ float s_src[];
  __shared__ float s_key[P];
  __shared__ unsigned s_val[P];
  const uint64_t a = blockIdx.x;
  for (unsigned d = threadIdx.x; d < dim; d += blockDim.x) s_src[d] = data[a * dim + d];
  for (int i = threadIdx.x; i < P; i += blockDim.x) { s_key[i] = FLT_MAX; s_val[i] = SK_EMPTY; }
  __syncthreads();
  const unsigned lane = threadIdx.x % SK_TEAM, team = threadIdx.x / SK_TEAM;
  const unsigned teams = blockDim.x / SK_TEAM;
  for (unsigned k = team; k < k_deg; k += teams) {
    const unsigned b = knn[a * k_deg + k];
    float dist = 0.f;
    if (b < n) {
      const float* row = data + (uint64_t)b * dim;
      for (unsigned d = lane; d < dim; d += SK_TEAM) { const float t = s_src[d] - row[d]; dist += t * t; }
    }
    for (int o = SK_TEAM / 2; o > 0; o >>= 1) dist += SK_SHFL_XOR(dist, o);
    if (lane == 0) { s_key[k] = (b < n) ? dist : FLT_MAX; s_val[k] = b; }
  }
  __syncthreads();
  sk_bitonic_sort<float>(s_key, s_val, P);
  for (unsigned k = threadIdx.x; k < k_deg; k += blockDim.x) knn[a * k_deg + k] = s_val[k];
}

__device__ __forceinline__ unsigned sk_hash(unsigned x) { x *= 0x9e3779b1u; return x ^ (x >> 15); }

/* One block per node A: rank of every neighbour in a shared hash, detour
 * counts for A->B as |{D : rank(D) < rank(B), D->B}|, then the out_degree
 * edges ordered by (detours, rank). Duplicates and invalid ids are dropped. */
template <int P, int HS>
__global__ void sk_prune_kernel(const unsigned* __restrict__ knn, unsigned n, unsigned k_deg,
                                unsigned* __restrict__ out, unsigned out_deg, unsigned* bad)
{
  __shared__ unsigned s_nbr[P];
  __shared__ unsigned s_cnt[P];
  __shared__ unsigned s_key[P];
  __shared__ unsigned s_hk[HS];
  __shared__ unsigned s_hv[HS];
  const uint64_t a = blockIdx.x;
  for (int i = threadIdx.x; i < HS; i += blockDim.x) { s_hk[i] = SK_EMPTY; s_hv[i] = SK_EMPTY; }
  for (int i = threadIdx.x; i < P; i += blockDim.x) {
    s_nbr[i] = (unsigned)i < k_deg ? knn[a * k_deg + i] : SK_EMPTY;
    s_cnt[i] = 0;
  }
  __syncthreads();
  for (unsigned j = threadIdx.x; j < k_deg; j += blockDim.x) {
    const unsigned id = s_nbr[j];
    if (id >= n) continue;
    unsigned h = sk_hash(id) & (HS - 1);
    while (true) {
      const unsigned prev = atomicCAS(&s_hk[h], SK_EMPTY, id);
      if (prev == SK_EMPTY || prev == id) { atomicMin(&s_hv[h], j); break; }
      h = (h + 1) & (HS - 1);
    }
  }
  __syncthreads();
  const unsigned pairs = (k_deg - 1) * k_deg;
  for (unsigned p = threadIdx.x; p < pairs; p += blockDim.x) {
    const unsigned kad = p / k_deg, kdb = p - kad * k_deg;
    const unsigned d = s_nbr[kad];
    if (d >= n) continue;
    const unsigned b = knn[(uint64_t)d * k_deg + kdb];
    if (b >= n) continue;
    unsigned h = sk_hash(b) & (HS - 1);
    while (true) {
      const unsigned key = s_hk[h];
      if (key == b) {
        const unsigned r = s_hv[h];
        if (r > kad) atomicAdd(&s_cnt[r], 1u);
        break;
      }
      if (key == SK_EMPTY) break;
      h = (h + 1) & (HS - 1);
    }
  }
  __syncthreads();
  for (unsigned j = threadIdx.x; j < (unsigned)P; j += blockDim.x) {
    unsigned key = SK_EMPTY;
    const unsigned id = s_nbr[j];
    if (j < k_deg && id < n) {
      unsigned h = sk_hash(id) & (HS - 1);
      while (s_hk[h] != id) h = (h + 1) & (HS - 1);
      if (s_hv[h] == j) key = (min(s_cnt[j], 255u) << 16) | j;  // first occurrence only
    }
    s_key[j] = key;
  }
  __syncthreads();
  sk_bitonic_sort<unsigned>(s_key, nullptr, P);
  for (unsigned t = threadIdx.x; t < out_deg; t += blockDim.x) {
    unsigned key = s_key[t];
    if (key == SK_EMPTY) { key = s_key[0]; if (threadIdx.x == t) atomicAdd(bad, 1u); }
    out[a * out_deg + t] = key == SK_EMPTY ? 0u : s_nbr[key & 0xffffu];
  }
}

__global__ void sk_rev_kernel(const unsigned* __restrict__ out, unsigned n, unsigned deg, unsigned k,
                              unsigned* __restrict__ rev, unsigned* __restrict__ rev_cnt)
{
  const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const unsigned dst = out[(uint64_t)i * deg + k];
  if (dst >= n) return;
  const unsigned pos = atomicAdd(&rev_cnt[dst], 1u);
  if (pos < deg) rev[(uint64_t)dst * deg + pos] = i;
}

/* One block per node: replay cuVS's reverse-edge splice exactly (newest
 * reverse edge first, inserted right after the protected first half). */
template <int P>
__global__ void sk_combine_kernel(unsigned* __restrict__ out, const unsigned* __restrict__ rev,
                                  const unsigned* __restrict__ rev_cnt, unsigned n, unsigned deg)
{
  __shared__ unsigned s_out[P];
  __shared__ unsigned s_rev[P];
  __shared__ unsigned s_pos;
  const uint64_t a = blockIdx.x;
  for (unsigned t = threadIdx.x; t < deg; t += blockDim.x) {
    s_out[t] = out[a * deg + t];
    s_rev[t] = rev[a * deg + t];
  }
  const unsigned prot = deg / 2;
  const unsigned kr0 = min(rev_cnt[a], deg);
  __syncthreads();
  for (unsigned kr = kr0; kr > 0;) {
    kr -= 1;
    const unsigned v = s_rev[kr];
    if (v >= n) continue;
    if (threadIdx.x == 0) s_pos = deg;
    __syncthreads();
    for (unsigned t = threadIdx.x; t < deg; t += blockDim.x)
      if (s_out[t] == v) atomicMin(&s_pos, t);
    __syncthreads();
    const unsigned pos = s_pos;
    __syncthreads();
    if (pos < prot) continue;
    const unsigned shift = pos >= deg ? deg - prot - 1 : pos - prot;
    unsigned moved[(P + SK_THREADS - 1) / SK_THREADS];
    int m = 0;
    for (unsigned t = prot + threadIdx.x; t < prot + shift; t += blockDim.x) moved[m++] = s_out[t];
    __syncthreads();
    m = 0;
    for (unsigned t = prot + threadIdx.x; t < prot + shift; t += blockDim.x) s_out[t + 1] = moved[m++];
    if (threadIdx.x == 0) s_out[prot] = v;
    __syncthreads();
  }
  for (unsigned t = threadIdx.x; t < deg; t += blockDim.x) out[a * deg + t] = s_out[t];
}

template <int P>
int sk_run(cudaStream_t st, const float* data, unsigned n, unsigned dim, unsigned* knn, unsigned k_deg,
           unsigned* out, unsigned out_deg, unsigned* rev, unsigned* rev_cnt, unsigned* bad)
{
  sk_sort_kernel<P><<<n, SK_THREADS, sizeof(float) * dim, st>>>(data, n, dim, knn, k_deg);
  sk_prune_kernel<P, 4 * P><<<n, SK_THREADS, 0, st>>>(knn, n, k_deg, out, out_deg, bad);
  for (unsigned k = 0; k < out_deg; ++k)
    sk_rev_kernel<<<(n + 255) / 256, 256, 0, st>>>(out, n, out_deg, k, rev, rev_cnt);
  sk_combine_kernel<P><<<n, SK_THREADS, 0, st>>>(out, rev, rev_cnt, n, out_deg);
  return cudaGetLastError() == cudaSuccess ? 0 : 1;
}

}  // namespace

extern "C" int seekdb_cuvs_optimize_graph(void* stream, const float* d_data, long n, long dim,
                                          unsigned int* d_knn, long k_deg,
                                          unsigned int* d_out, long out_deg)
{
  if (n <= 0 || dim <= 0 || dim > 8192 || k_deg < 2 || k_deg > 256 || out_deg < 2 || out_deg > k_deg ||
      n >= (long)SK_EMPTY) {
    return 1;
  }
  cudaStream_t st = (cudaStream_t)stream;
  unsigned *rev = nullptr, *rev_cnt = nullptr;
  const size_t rev_bytes = sizeof(unsigned) * (size_t)n * out_deg;
  if (cudaMalloc((void**)&rev, rev_bytes + sizeof(unsigned) * ((size_t)n + 1)) != cudaSuccess) return 2;
  rev_cnt = rev + (size_t)n * out_deg;
  unsigned* bad = rev_cnt + n;
  int rc = 3;
  if (cudaMemsetAsync(rev, 0xff, rev_bytes, st) == cudaSuccess &&
      cudaMemsetAsync(rev_cnt, 0, sizeof(unsigned) * ((size_t)n + 1), st) == cudaSuccess) {
    rc = k_deg <= 128 ? sk_run<128>(st, d_data, n, dim, d_knn, k_deg, d_out, out_deg, rev, rev_cnt, bad)
                      : sk_run<256>(st, d_data, n, dim, d_knn, k_deg, d_out, out_deg, rev, rev_cnt, bad);
  }
  unsigned h_bad = 0;
  if (rc == 0 && (cudaMemcpyAsync(&h_bad, bad, sizeof(h_bad), cudaMemcpyDefault, st) != cudaSuccess ||
                  cudaStreamSynchronize(st) != cudaSuccess)) {
    rc = 4;
  }
  if (rc == 0 && h_bad) {
    fprintf(stderr, "[seekdb_cuvs] %u nodes lack %ld distinct neighbours\n", h_bad, out_deg);
  }
  (void)cudaStreamSynchronize(st);
  (void)cudaFree(rev);
  return rc;
}
