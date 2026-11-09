# 提交指引（seekdb × AMD hipVS 大赛 — 赛题二）

本文件说明如何把本地已完成的改动发布为符合大赛要求的提交仓库。

## 1. 本地状态（已完成）

- 仓库：`seekdb-amd/seekdb`（克隆自 `https://github.com/zihaimu/seekdb`）
- 分支：`amd-cagra-submit`
- 提交：`3a40e20 perf(vector): runtime-tunable CAGRA build + faster cuVS snapshot path`
- 父提交：冻结基线 `634888d9b037153fa143d2c913ba4b25503e1d62`（V5）
- 改动 7 个文件：bridge `.c`、adaptor `.cpp`、`docs/amd-contest/**`（测试方法学）

自检（已通过）：

```bash
git merge-base --is-ancestor 634888d9b037153fa143d2c913ba4b25503e1d62 HEAD && echo "是冻结基线后代"
git log --oneline -2        # 应看到 3a40e20 直接位于 634888d9 之上
```

## 2. 发布到自己的 fork

```bash
cd seekdb-amd/seekdb
git remote add fork https://github.com/WatingCL/seekdb.git   # 已 fork 过则用 set-url
git push -u fork amd-cagra-submit
```

## 3. 大赛提交的仓库地址格式

按官方 FAQ，446 个执行错误中 24% 是仓库地址格式错误（failMsg=4）。地址必须是：

```
https://<你的GitHub用户名>:<访问令牌>@github.com/<所有者>/<仓库>
```

示例（用户名 `WatingCL`，fork 后的仓库所有者仍是 `WatingCL`）：

```
https://WatingCL:<PERSONAL_ACCESS_TOKEN>@github.com/WatingCL/seekdb
```

注意：

- 令牌需要 `repo` 权限的 classic PAT（或 fine-grained 的 Contents 读权限）。
- 不要在提交表单以外的任何地方泄露令牌；令牌泄露后到 GitHub 设置里 revoke。
- 若使用 fork 的默认分支，请确保提交的分支（`amd-cagra-submit`）在 fork 中可见，
  或在表单的备注里写明分支名。

## 4. 提交前三项硬性自检

1. **基线后代**：`git merge-base --is-ancestor 634888d9 HEAD` 必须成功
   （failMsg=8「非基线后代」占执行错误 35.9%，是最常见的硬伤）。
2. **未动 hipVS/cuVS**：本仓库不含 hipVS 源码；桥接 `.so` 仍链接 pinned
   commit `f96a663d` 的 `libcuvs_c.so`（`tools/hipvs_bridge/build.sh` 未改）。
3. **默认行为 = 基线**：不设任何 `OB_CUVS_*` 环境变量时，bridge 逐位复现冻结基线
   （NN_DESCENT / degree=128 / intermediate=256 / niter=100，
   itopk=max(4·topk,512) / search_width=4）。

## 5. 提交物清单（大赛要求）

| 要求 | 位置 |
|---|---|
| 实现代码 | `tools/hipvs_bridge/seekdb_cuvs_bridge.c`、`src/oblib/lib/vector/ob_vsag_adaptor.cpp` |
| 可复现测试方法 | `docs/amd-contest/`（README + gen_dataset.py + bench.sh + run_matrix.sh） |
| CPU/GPU 构建性能对比报告 | `docs/amd-contest/report_template.md`（在 GPU 机器上按模板填数） |

报告填写步骤（在 GPU 机器上）：

```bash
# 1) 生成两个数据集（SIFT 公开数据 + Hidden 代理；正式结论以官方 Hidden250K 复核）
python3 docs/amd-contest/gen_dataset.py --dataset sift         --out /data/sift250k
python3 docs/amd-contest/gen_dataset.py --dataset hidden-proxy --out /data/hidden250k

# 2) 每个数据集先跑 base 配置（基线分母），留档
docs/amd-contest/bench.sh --observer <observer> --bridge <libseekdb_cuvs_bridge.so> \
  --dataset-dir /data/sift250k --evidence /runs/base-sift --label base \
  --backend cuvs --results-csv /runs/results.csv

# 3) 扫候选配置（无需重编译），CPU 对照组用 --backend vsag
docs/amd-contest/run_matrix.sh --observer <observer> --bridge <libseekdb_cuvs_bridge.so> \
  --dataset-dir /data/sift250k --evidence-root /runs/matrix-sift

# 4) 把 /runs/**/result.json 的数字填进 report_template.md
```

## 6. 失败模式备忘（来自官方 FAQ）

- 72.1% 零分提交死于 Recall——尤其 Hidden@100 低于同轮基线 0.005 以上。
  对策：任何提速配置必须两个数据集都过 Recall 门禁；只把达标配置写成默认值。
- 23.5% 显存超限（>1.25×基线）。对策：VRAM 采样纳入每次 bench，超限即淘汰。
- 13% 查询回归（QPS<0.9×基线）。对策：查询侧改动默认关闭、逐项 A/B。
- 执行错误前二名：非基线后代、仓库地址格式错——见上文第 3、4 节。
