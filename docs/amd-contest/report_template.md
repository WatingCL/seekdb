# CPU/GPU 向量索引构建性能对比报告

> 赛题：seekdb × AMD hipVS 数据库大赛 — 赛题二（CAGRA 向量索引构建链路优化）
> 被测基线：seekdb @ `634888d9b037153fa143d2c913ba4b25503e1d62`
> （分支 `amd-oceanbase-official-contest-base-v5`），hipVS pinned @ `f96a663d0a4441d6f5344bd3ef552b71ae7b370b`
> 报告日期：<YYYY-MM-DD>　　执行人：<名字>　　原始数据：<results.csv / result.json 路径>

---

## 1. 测试环境

| 项 | 值 |
|---|---|
| CPU | <型号 / 核数> |
| 内存 | <GB> |
| GPU | <型号 / 显存> |
| ROCm | <版本> |
| hipVS | pinned commit `f96a663d`（未做任何修改） |
| seekdb 构建 | <commit / 构建选项> |
| 数据集 | SIFT250K（250K×128，公开）/ Hidden250K（250K×256）或 hidden-proxy 代理 |
| 客户端 | mysql client，单连接顺序执行 |

## 2. 测试方法（与大赛计时口径一致）

计时窗口：**DDL 开始 → 第一条 `APPROXIMATE` 查询返回**。数据加载（`LOAD DATA`）在窗口外。
评测 SQL（与大赛逐字一致，仅库/索引名相同）：

```sql
CREATE VECTOR INDEX idx_items ON items(v)
  WITH (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64);
SELECT id FROM items ORDER BY l2_distance(v, '<query>') APPROXIMATE LIMIT <k>;
```

- GPU 组：`lib=cuvs`（CAGRA）；CPU 组：`lib=vsag`（同一 DDL、同参数）。
- 每组先跑 `base` 配置（不设任何 `OB_CUVS_*`，逐位复现冻结基线行为）留档，
  所有比值以 base 为分母。
- 查询吞吐：单连接顺序执行 4096 条评测 SQL 计 QPS；另测 8 个等分批次时延 p50/p95。
- Recall：与分块暴力精确计算的 ground truth 求 top-10/top-100 交集率。
- 显存：DDL 至查询全程 10Hz 采样峰值。
- 复现方式：`docs/amd-contest/`（`gen_dataset.py` → `bench.sh` → `run_matrix.sh`）。

## 3. 构建性能对比（核心表）

| 数据集 | 组 | 配置 | DDL→首查 (s) | 对基线加速比 | DDL (s) | 首查 (s) | build_us | graph_us | stream_us | 图快照 (MB) |
|---|---|---|---|---|---|---|---|---|---|---|
| SIFT250K | GPU | base（冻结基线行为） | <x.xx> | 1.00x | | | | | | |
| SIFT250K | GPU | <候选配置 + env> | <x.xx> | <x.xx>x | | | | | | |
| SIFT250K | CPU | vsag base | <x.xx> | — | | | — | — | — | — |
| Hidden250K | GPU | base | <x.xx> | 1.00x | | | | | | |
| Hidden250K | GPU | <候选配置 + env> | <x.xx> | <x.xx>x | | | | | | |
| Hidden250K | CPU | vsag base | <x.xx> | — | | | — | — | — | — |

> `build_us/graph_us/stream_us` 来自 observer.log 的 `serialized GPU CAGRA snapshot` 插桩
> （CAGRA 构建 / 图序列化落临时文件 / 快照流写）；`OB_CUVS_DEBUG=1` 时可另取 bridge 侧
> `build ... (x ms)` 交叉验证。加权提速 = 0.4×SIFT 加速比 + 0.6×Hidden 加速比。

## 4. 查询性能

| 数据集 | 组 | 配置 | QPS（单连接） | 对基线比值 | p50 (ms) | p95 (ms) | max (ms) |
|---|---|---|---|---|---|---|---|
| SIFT250K | GPU | base | | 1.00x | | | |
| SIFT250K | GPU | <候选> | | | | | |
| Hidden250K | GPU | base | | 1.00x | | | |
| Hidden250K | GPU | <候选> | | | | | |

## 5. 召回率（硬门槛）

| 数据集 | 配置 | Recall@10 | Recall@100 | 门槛 | 相对 base Δ@100 | 结论 |
|---|---|---|---|---|---|---|
| SIFT250K | base | | | ≥0.95 / ≥0.95 | — | |
| SIFT250K | <候选> | | | 同上 | <±0.00x> | 通过/不通过 |
| Hidden250K | base | | | ≥0.90 / ≥0.85 | — | |
| Hidden250K | <候选> | | | 同上 | <±0.00x> | 通过/不通过 |

## 6. 显存（硬门槛：≤ 基线 ×1.25）

| 数据集 | 配置 | 峰值 VRAM (GB) | 对 base 比值 | 结论 |
|---|---|---|---|---|
| SIFT250K | base | | 1.00x | |
| SIFT250K | <候选> | | | 通过/不通过 |
| Hidden250K | base | | 1.00x | |
| Hidden250K | <候选> | | | 通过/不通过 |

## 7. 优化手段与收益归因

1. **构建参数运行时化**（`OB_CUVS_*`，默认=基线）：NN-descent 迭代数、图度数、
   中间图度数、IVF-PQ 路径及搜索参数全部可环境变量 A/B，一次编译多配置。
   - 收益：<如 niter 100→20 构建 -xx%，recall 变化>
2. **主机侧缓冲预分配**（`ob_cuvs_add` 32MiB 块对齐 reserve）：减少 250K 级大向量
   缓冲反复 realloc/拷贝。
   - 收益：<扫描/缓冲阶段 -xx%>
3. **快照流式块 64KiB→1MiB**：>100MB 图 blob 的读写轮次大幅下降。
   - 收益：`stream_us` 从 <x> 降至 <y>
4. **查询路径**：search params 每 handle 缓存、设备 scratch grow-only 复用、
   结果缓冲 thread_local 化，去掉逐查询 RMM 分配。
   - 收益：QPS <x>→<y>（p95 <a>→<b>）

## 8. 结论与提交默认值

- 采用配置：<env 组合，如 `OB_CUVS_NN_ITERS=40`>
- 加权构建提速：<x.xx>x（SIFT 0.4 / Hidden 0.6）
- 查询 QPS 比值：<x.xx>x；Recall 全部达门槛；显存 <x.xx>x ≤ 1.25x。
- 该配置已设为默认值/或保留为文档化环境变量方案：<说明>

## 9. 复现实验记录

```bash
# 数据
python3 docs/amd-contest/gen_dataset.py --dataset sift         --out /data/sift250k
python3 docs/amd-contest/gen_dataset.py --dataset hidden-proxy --out /data/hidden250k
# 单配置
docs/amd-contest/bench.sh --observer <obs> --bridge <bridge> --dataset-dir /data/sift250k \
  --evidence /runs/base --label base --backend cuvs --results-csv /runs/results.csv
# 扫描
docs/amd-contest/run_matrix.sh --observer <obs> --bridge <bridge> \
  --dataset-dir /data/sift250k --evidence-root /runs/matrix
```

原始证据：`/runs/**/result.json`、`/runs/**/observer.log`、`/runs/**/queries*.out`、
`/runs/results.csv`。
