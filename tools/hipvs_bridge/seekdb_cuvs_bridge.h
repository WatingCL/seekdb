#ifndef SEEKDB_CUVS_BRIDGE_H
#define SEEKDB_CUVS_BRIDGE_H
#ifdef __cplusplus
extern "C" {
#endif
/* One-shot: build a CAGRA index over base (n x dim, row-major f32) on the GPU and
 * search queries (nq x dim), writing top-k uint32 neighbor ids to out_ids
 * (length nq*topk). Returns 0 on success, non-zero on error. */
int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids);

/* Build-once / search-many (used by seekdb's ob_vsag_adaptor GPU path).
 *  - seekdb_cuvs_build: builds a CAGRA index on the GPU from base (n x dim,
 *    row-major f32) and returns an opaque handle (NULL on error). cuVS copies
 *    the dataset to the device, so `base` may be freed after this returns.
 *  - seekdb_cuvs_search: runs top-k search for nq queries (row-major f32, dim
 *    cols), writing uint32 row offsets to out_ids[nq*topk] and (if non-NULL) L2
 *    distances to out_dist[nq*topk]. Returns 0 on success.
 *  - seekdb_cuvs_free: releases the handle.
 * All ROCm/cuVS state lives inside the .so; seekdb only ever sees these C symbols. */
void* seekdb_cuvs_build(const float* base, long n, long dim);
int   seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                         unsigned int* out_ids, float* out_dist);
void  seekdb_cuvs_free(void* handle);
int seekdb_cuvs_serialize(void* handle, const char* filename);
void* seekdb_cuvs_deserialize(const char* filename, long n, long dim);

/* Graph-only persistence: the caller already stores the vectors, so a snapshot
 * only needs the CAGRA graph (n x degree uint32 row offsets).
 *  - seekdb_cuvs_graph_degree: graph degree of a built handle, or -1.
 *  - seekdb_cuvs_copy_graph: copies the graph to host memory out_graph[n*degree].
 *  - seekdb_cuvs_from_graph: rebuilds a searchable handle from host vectors and
 *    a graph produced by seekdb_cuvs_copy_graph; both inputs may be freed after
 *    it returns. */
long  seekdb_cuvs_graph_degree(void* handle);
int   seekdb_cuvs_copy_graph(void* handle, unsigned int* out_graph);
void* seekdb_cuvs_from_graph(const float* base, long n, long dim,
                             const unsigned int* graph, long degree);

/* Initialises the GPU runtime and loads the CAGRA build/search kernels for
 * `dim`-wide data on a tiny synthetic set. Returns 0 on success. */
int   seekdb_cuvs_warmup(long dim);
#ifdef __cplusplus
}
#endif
#endif
