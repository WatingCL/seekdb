# seekdb × AMD hipVS 大赛 — 可复现测试方法

本目录包含赛题二（CAGRA 向量索引构建优化）的完整可复现测试方法：数据集生成、
单配置基准、配置扫描、指标定义与硬门槛核对。所有脚本只依赖比赛基线环境
（observer + `libseekdb_cuvs_bridge.so` + ROCm/hipVS + mysql client + python3/numpy），
不修改 hipVS/cuVS 库本身。

## 0. 优化实现概览（被测对象）

改动仅在两处，均保持默认行为与冻结基线 V5 完全一致（未设任何环境变量时逐位复现基线）：

| 文件 | 改动 | 效果 |
|---|---|---|
| `tools/hipvs_bridge/seekdb_cuvs_bridge.c` | 全部构建/搜索参数改为环境变量运行时可选（`OB_CUVS_*`，默认值=基线值）；search params 每 handle 缓存一次；设备 scratch 缓冲 grow-only 复用；`OB_CUVS_DEBUG=1` 输出分阶段计时 | 一次编译、多配置 A/B；查询路径去掉每次 RMM 分配 |
| `src/oblib/lib/vector/ob_vsag_adaptor.cpp` | `ob_cuvs_add` 32MiB 块对齐 reserve（减少大缓冲反复拷贝）；快照图 blob 流式块 64KiB→1MiB；序列化日志分解 `build_us/graph_us/stream_us`；查询结果缓冲 thread_local 复用 | 扫描缓冲与快照落盘更快；计时插桩定位瓶颈 |

桥接 `.so` 的 C ABI 未变（`seekdb_cuvs_build/search/free/serialize/deserialize/cagra_knn`），
`libcuvs_c.so` 仍按 pinned commit `f96a663d` 链接。

## 1. 环境要求

- 已按比赛基线构建的 seekdb（分支 `amd-oceanbase-official-contest-base-v5` @ `634888d9b`），
  在此基础上叠加本目录所述改动（或直接使用本仓库 `amd-cagra-submit` 分支）。
- hipVS 安装在 `/opt/hipvs`（含 `include/cuvs/**` 与 `lib/libcuvs_c.so`），ROCm 在 `/opt/rocm`。
- 一张可用 AMD GPU（`/dev/kfd` + `/dev/dri/render*`）。
- `mysql` client、`python3` + `numpy`、`bash`、`awk`。
- 磁盘：数据集 TSV 约 0.4GB（128 维）/ 0.8GB（256 维），建议放本地 NVMe。

## 2. 生成数据集

```bash
python3 docs/amd-contest/gen_dataset.py --dataset sift         --out /data/sift250k
python3 docs/amd-contest/gen_dataset.py --dataset hidden-proxy --out /data/hidden250k
```

- `sift`：公开 SIFT1M 语料（texmex, ftp）取前 250K 行作为库、前 4096 条作为查询；
  精确 ground truth 在 250K 子集上用分块暴力重算（不依赖厂商 GT 文件，保证 recall 口径自洽）。
- `hidden-proxy`：256 维聚类高斯混合（2048 个簇、重尾簇大小、簇内 σ=1），
  查询为随机库点加微扰——比纯高斯更接近真实 embedding 分布，用于在拿不到
  Hidden250K 时做代理评估。**正式提交前应以官方 Hidden250K 复核结论。**

输出（每个数据集目录）：

| 文件 | 用途 |
|---|---|
| `base.tsv` | `LOAD DATA` 输入（不计时阶段） |
| `queries.sql` / `queries100.sql` | 4096 条评测 SQL（第 1 条即计时窗口的首条查询） |
| `gt10.i32` / `gt100.i32` | 精确 top-k ground truth（int32, [nq][k]） |
| `base.f32` / `queries.f32` / `probe0.txt` / `meta.json` | 原始数组与元数据 |

## 3. 单配置基准

```bash
docs/amd-contest/bench.sh \
  --observer  /path/to/observer \
  --bridge    /path/to/libseekdb_cuvs_bridge.so \
  --dataset-dir /data/sift250k \
  --evidence  /runs/base --label base \
  --backend   cuvs --port 2881 \
  --base-dir  /runs/base/run --results-csv /runs/results.csv
```

流程（每个配置都是全新 observer 进程，环境变量在进程启动时生效）：

1. **预检**：observer/bridge/数据集/工具链。
2. **建表 + LOAD DATA**（不计时）：`create table items(id bigint primary key, v vector(DIM))`，
   `load data local infile base.tsv`。校验行数 = 250K。
3. **计时窗口**（核心指标）：`t0` → `CREATE VECTOR INDEX idx_items ON items(v) WITH (distance=l2,
   type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64)` → 第一条
   `SELECT id FROM items ORDER BY l2_distance(v,'<probe0>') APPROXIMATE LIMIT 10` 返回 → `t2`。
   记录 `ddl_to_first_query_s = t2-t0`（大赛口径），并分解 `ddl_s`、`first_query_s`。
   与大赛 SQL 逐字一致（库/索引名除外）。
4. **VRAM 采样**：DDL 前启动 10Hz 采样循环（`/sys/class/drm/card*/device/mem_info_vram_used`，
   多卡取 max），首查后停止，取峰值 `vram_peak_bytes`。
5. **查询吞吐**：单连接顺序执行 4096 条评测 SQL，`qps = nq / wall`；
   再按 8 个等分批次各测一次，得到批级时延 p50/p95/max。
6. **Recall**：对全部 4096 个查询取 `APPROXIMATE LIMIT 10/100` 的返回 id，
   与 `gt10.i32`/`gt100.i32` 求交集率（`|∩|/k` 的均值）。
7. **日志分解**：从 observer.log 提取 `serialized GPU CAGRA snapshot ... build_us= graph_us=
   stream_us= graph_bytes= built=true`（本次新增的计时插桩）；若启动环境带
   `OB_CUVS_DEBUG=1`，还能拿到 bridge 侧 `build ... (x ms)` 的 CAGRA 构建耗时。
8. **GPU 门禁证据**：`gpu_build_log=1`（快照日志 built=true）；若 observer 编译了
   `OB_BUILD_CUVS_TRACE`，另核对 trace 中 `cuvs_build`/`cuvs_serve` 标记数。
9. 输出 `result.json`，可选追加一行 CSV，随后关停 observer 并清理运行目录（`--keep` 保留）。

## 4. 配置扫描（一次编译，多配置 A/B）

```bash
docs/amd-contest/run_matrix.sh \
  --observer /path/to/observer --bridge /path/to/libseekdb_cuvs_bridge.so \
  --dataset-dir /data/sift250k --evidence-root /runs/matrix --backend cuvs
```

内置配置（全部通过环境变量生效，**不需要重编译**）：

| 配置 | 环境变量 | 意图 |
|---|---|---|
| `base` | （无） | 冻结基线行为，所有对比的基准 |
| `niter40` / `niter20` | `OB_CUVS_NN_ITERS=40/20` | NN-descent 迭代数 100→40/20，主要构建提速候选 |
| `niter20-deg96` | `NN_ITERS=20 GRAPH_DEGREE=96 INTER_DEGREE=192` | 迭代+度数双降，提速最大、召回风险最高 |
| `inter192` | `OB_CUVS_INTER_DEGREE=192` | 只降中间图度数 |
| `ivfpq` | `OB_CUVS_BUILD_ALGO=1 IVF_N_PROBES=64 IVF_REFINE_RATE=2.0` | IVF-PQ 建图路径 |
| `itopk1024` | `OB_CUVS_ITOPK=1024` | 搜索精度候选（查召回/时延） |
| `width8` | `OB_CUVS_SEARCH_WIDTH=8` | 搜索宽度候选 |

扫描结束打印汇总表：各配置 `build_s`（对 base 的加速比）、`qps`（对 base 的比值）、
`recall@10/@100`、峰值显存、GPU 门禁标记。**只有 recall 达标的配置才考虑翻默认值**，
保 4 分硬门槛优先于提速。

## 5. 指标定义与硬门槛

| 指标 | 定义 | 门槛 |
|---|---|---|
| 构建性能 | `ddl_to_first_query_s`（DDL 开始→首条查询返回） | 3.0x 满分，对数计分 |
| 查询性能 | 单连接 QPS（4096 条评测 SQL） | ≥ 基线 0.9x（1.5x 满分） |
| Recall@10 | top-10 交集率均值 | SIFT ≥ 0.95；Hidden ≥ 0.90 |
| Recall@100 | top-100 交集率均值 | SIFT ≥ 0.95；Hidden ≥ 0.85，且不低于同轮基线 −0.005 |
| 显存 | 全程峰值 VRAM | ≤ 基线 ×1.25 |
| GPU 真实性 | 快照日志 built=true + trace `cuvs_build`/`cuvs_serve` | 必须为真 |

评分权重提示：SIFT250K 40% + Hidden250K 60%；官方 FAQ 显示零分提交 72.1% 死于
Recall（尤其 Hidden@100 相对基线下降），因此每个候选配置必须两个数据集都过
Recall 门禁再谈提速。

## 6. 已知测量误差与注意事项

- `ddl_to_first_query_s` 含两次 mysql 客户端进程启动开销（约 30–80ms），对 10s 级
  构建窗口可忽略；如需剔除，可用 `--keep` 复用同一连接手工复测。
- `qps_single_conn` 是单连接顺序时延口径；多连接并发 QPS 会更高，但与大赛
  SQL 形状（逐条 SELECT）最同构，作为主口径。
- `base` 配置必须每个数据集先跑一遍并留档（`/runs/base-<dataset>/result.json`），
  它同时是加速比、QPS 比值、显存比值的分母。
- 数据集 TSV 用 `%.7f` 格式化（与基线 smoke 一致）；ground truth 由本工具用
  float32 精确重算，避免供应商 GT 与入库精度不一致导致 recall 口径漂移。
- 若 observer 未编译 `OB_BUILD_CUVS_TRACE`，trace 文件为空、`trace_*` 列为 0，
  不影响其它指标（GPU 真实性以快照日志为准）。

## 7. 文件清单

```
docs/amd-contest/
├── README.md           本文档
├── gen_dataset.py      数据集 + 精确 ground truth 生成
├── bench.sh            单配置基准（构建/查询/召回/显存/门禁）
├── run_matrix.sh       配置扫描 + 汇总表
└── report_template.md  CPU/GPU 构建性能对比报告模板
```
