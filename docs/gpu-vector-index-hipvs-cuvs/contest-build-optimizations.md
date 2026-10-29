# CAGRA index build optimizations (AMD contest, V5 base)

Base: `amd-cagra-contest-baseline-v5` (`634888d9b`). The timed path is
`CREATE VECTOR INDEX ... WITH (type=hnsw, lib=cuvs, ...)` plus the first
`... APPROXIMATE LIMIT k` query. Index semantics, SQL, and the GPU-only
build/search path are unchanged; `lib=vsag` is untouched.

## Where the baseline spends its time

Offline DDL timeline of the baseline on SIFT250K (local RTX 5080 + CUDA cuVS
25.10 with the same C API as hipVS; `[OBVSAG][cuVS]` log lines):

| Phase | Time |
| --- | ---: |
| rowkey/vid auxiliary tables | 3.3 s |
| scan + `add_index` buffering (250K rows) | 0.7 s |
| GPU runtime start-up + CAGRA build + serialize | 23.1 s |
| snapshot rows written to LOB | 1.5 s |
| first query (served from the DDL-built index) | 0.17 s |

Graph construction dominates; GPU runtime start-up (context creation and
kernel loading) is paid inside the build because it is the first GPU call of the
process.

## Changes

1. **GPU warm-up overlapped with the DDL.** When a `lib=cuvs` index statement is
   resolved, `obvsag::prewarm_cuvs(dim)` starts a background thread that calls
   `seekdb_cuvs_warmup(dim)`: it initialises the device runtime and builds and
   searches a 4096-point synthetic set with the production parameters, which
   loads the build/search kernels. It runs once per process, while the
   auxiliary tables are built. Every real GPU call waits for an in-flight
   warm-up instead of racing it. No graph, index or data is reused across
   statements — the warm-up only touches synthetic data and is freed at once.
2. **Graph-only snapshot (`OBCUVS03`).** The baseline snapshot stores the
   vectors twice (once in the stream and once inside the cuVS index file) and
   round-trips through a temporary file. The new format stores
   `ids | vectors | CAGRA graph` and is loaded with `cuvsCagraIndexFromArgs`
   from host buffers. `OBCUVS02` snapshots are still readable.
3. **Search scratch reuse.** The bridge keeps the query/result device buffers and
   the search-parameter object per index handle (guarded by a mutex) instead of
   allocating and freeing them on every single-row query.
4. **NN-Descent kNN graph: degree 192, 60 iterations** (baseline 256, 100),
   still pruned to a degree-128 CAGRA graph with the same search parameters.
   NN-Descent runs every iteration it is given, so its cost scales with
   degree x iterations. The 192-wide graph converges faster and, on hard data,
   ends up better than the 256-wide one.

## Measurements (local RTX 5080, CUDA cuVS 25.10)

Bridge-level, CAGRA build only (graph build time, then Recall@10 / Recall@100
with the unchanged search parameters, single-row queries, exact ground truth):

| Data (250K) | Baseline 256/100 | 192/60 (this change) | 192/40 | 192/30 |
| --- | --- | --- | --- | --- |
| SIFT 128-d | 20.3 s, 1.000/1.000 | 12.1 s, 1.000/1.000 | 10.9 s, 1.000/1.000 | 9.8 s, 1.000/1.000 |
| NYTimes 256-d (zero/duplicate rows removed) | 22.8 s, 0.9973/0.9852 | 14.1 s, 0.9968/0.9834 | 12.0 s, 0.9949/0.9803 | 10.5 s, 0.9941/0.9766 |
| synthetic 256-d, flat spectrum (`spec250`) | 20.1 s, 0.9548/0.9153 | 12.7 s, 0.9798/0.9468 | 10.8 s, 0.9729/0.9392 | 9.7 s, 0.9703/0.9315 |

`spec250` (coordinate std `exp(-i/250)` in a random rotation) was chosen
because its baseline Recall@100 matches the contest's hidden 256-d set. 192/60
keeps the worst observed drop at 0.0018 (near-saturated NYTimes) while hard data
gains up to +0.03; 40 or fewer iterations come too close to the 0.005 margin.
Single-row QPS is unchanged within noise. An RMM pool resource gave no
measurable gain and is not used.

End to end in seekdb (offline DDL + first query, then 1024 single-row SQL queries):

| SIFT250K | Baseline | This change |
| --- | ---: | ---: |
| `CREATE VECTOR INDEX` + first query | 28.04 s | 17.88 s |
| Top-10 QPS / recall | 255 / 1.000 | 332 / 1.000 |
| Top-100 QPS / recall | 237 / 1.000 | 286 / 1.000 |

The warm-up (3.4 s) finishes during the auxiliary-table phase, so the vector
build does not wait for it. After an observer restart the `OBCUVS03` snapshot
loads in 0.58 s and serves the same recall. 256-d end to end: NYTimes 21.3 s,
0.9966/0.9837; `spec250` 18.6 s, 0.9794/0.9470. Filtered queries, inserted and
deleted rows still fall back correctly, and `lib=vsag` indexes are unchanged.

## Measurements on AMD (Radeon Cloud, Radeon PRO W7900-class gfx1100, ROCm 7.2.4, /opt/hipvs)

Bridge compiled with `hipcc` exactly as `tools/hipvs_bridge/build.sh` does.
This shared instance is slower than the official evaluation host, so only the
ratios are meaningful.

| Bridge-level, 250K | Baseline build | New build | Speed-up | Baseline R@10/R@100 | New R@10/R@100 |
| --- | ---: | ---: | ---: | --- | --- |
| SIFT 128-d | 42.6 s | 24.2 s | 1.76x | 1.000/1.000 | 1.000/1.000 |
| NYTimes 256-d | 44.1 s | 25.4 s | 1.74x | 0.9960/0.9834 | 0.9970/0.9848 |

End to end with this branch's seekdb and the hipcc bridge (SIFT250K, nq=4096):
`CREATE VECTOR INDEX` + first query 30.1 s (graph build 24.3 s; the 3.7 s
warm-up finished before the build needed the GPU), Top-10 144 QPS / recall
1.000, Top-100 137 QPS / recall 1.000, no bad rows. After an observer restart
the `OBCUVS03` snapshot loaded in 0.31 s with the same recall.

## Second round

5. **NN-Descent 128 wide, 30 iterations.** Cheaper per iteration than 192/60
   and, on hard 256-d data, still above the 256/100 baseline graph.
6. **CAGRA graph optimisation on the device** (`tools/hipvs_bridge/seekdb_cuvs_graph.cu`).
   The bridge runs NN-Descent through the C API, keeps the kNN graph on the
   device and applies CAGRA's optimisation in its own HIP kernels: exact fp32
   re-sort, 2-hop detour counts through a per-block hash of neighbour ranks
   (O(d^2) per node instead of cuVS's O(d^3) kernel), selection by
   (detours, rank), reverse graph and the reverse-edge splice. cuVS does the
   last three on the host with several host/device copies. The index is made
   with `cuvsCagraIndexFromArgs` over the device dataset and graph; search is
   CAGRA's. Any failure falls back to `cuvsCagraBuild`.
7. **GPU build overlapped with the auxiliary tables.** Before the DDL builds
   the rowkey/vid tables (CPU only, seconds), the resolver reads the indexed
   column through the inner SQL connection and starts the GPU build in the
   background (`ObVectorIndexUtil::check_vec_index_param` ->
   `obvsag::prebuild_cuvs`). The vector index build adopts that index only if
   it receives byte-identical vectors in the same order; otherwise it is
   discarded and the index is built as before. It is consumed once and never
   outlives the statement that started it.
8. **Search: 64 CTAs per single-row query** (`search_width=64`, multi-CTA,
   itopk unchanged), pinned host staging, async copies and one stream sync.
   More independent CTAs raise recall and lower latency on a W7900.

Local RTX 5080 (CUDA cuVS 25.10), cold observer, `CREATE VECTOR INDEX` + first
query, then 2048 single-row SQL queries per k:

| 250K | Build | Top-10 QPS / recall | Top-100 QPS / recall |
| --- | ---: | --- | --- |
| SIFT 128-d (baseline 28.0 s, 255 / 237 QPS) | 7.6 s | 518 / 0.9999 | 420 / 1.0000 |
| hard 256-d (`spec250`, baseline recall 0.955 / 0.916) | 7.7 s | 356 / 0.971 | 305 / 0.935 |
| NYTimes 256-d (baseline recall 0.997 / 0.985) | 6.9 s | 377 / 0.996 | 315 / 0.981 |

No out-of-range or duplicate ids; after an observer restart the snapshot
reloads with the same recall; `lib=vsag`, filtered queries, inserts and deletes
behave as before.

## Reproducing

```bash
# bridge-level timing (build / serialize / deserialize / single-query QPS / recall)
bridge_bench <data_dir> <dim> [nq]        # data_dir: base.f32 query.f32 gt100.i64
# end-to-end (seekdb, offline DDL + nq=4096 queries)
run_bench.py --port <port> --data <data_dir> --load --rounds 1
```

`tools/hipvs_bridge/test_persistence.cpp` covers the graph export/import path
(`graph` mode) in addition to the cuVS file format (`write`/`read`).
