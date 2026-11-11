# 赛题 4 收尾：全文索引优化与同机验证

本次完成 IK 重复解码路径的补充优化、正确性核对、同机优化前后完整基准和 CI 验证入口。最终四组观测的三类等权综合耗时下降 **0.76%**，其中 IK TOKENIZE 平均耗时下降 **1.83%**。其他若干指标的差异小于运行波动；这不是稳定收益保证，也没有达到“同机综合耗时减半”的幅度。

## 修改与定位依据

从 `TokenizeContext::prepare_next_char` 和 `ObIKArbitrator::output_result` 的调用路径可以看到，每次读取字符先调用 `first_valid_char` 识别长度，再调用字符分类；原始分类中的多个判断还分别解码同一个字符。中文字符在长度检查和分类中会重复读取同一段 UTF-8 字节。该重复工作在索引构建、TOKENIZE 和查询词解析中都会发生。

- 保留此前的单次分类解码、中文数字码点匹配，以及 BENG 已小写 ASCII 词借用输入切片的实现。
- 新增共享的 `first_char_info`：有效 UTF-8 标量一次解码得到长度和码点，扫描和仲裁都使用它；ASCII 码点直接判断字母、数字和分隔符。
- 非法、截断、代理项输入走原来的 `first_valid_char` 校验路径，保留错误码；UTF-16 保留原有解码和分类语义。全角字母、数字优先于宽泛的 CJK 判断。
- 没有修改词典、分词规则、索引排序、查询排序或评分算法。BENG 分词在最终完整测量中没有可辨认的加速，因此不把减少分配推断为整条链路加速。

这里减少的是“一次获取字符长度和分类”中的重复解码，不代表整个分词过程只访问每个字符一次。

## 对照版本与环境

源代码基点为 `1e5b86437c9767bcd3ceafdfedd2dfea09b8e21c`。对照二进制只恢复以下六个文件在 `2845b8e06`（`930460d84` 的父提交）中的版本，其余代码保持相同，包含赛题 3 的实现：

```text
src/storage/fts/ik/ob_ik_char_util.h
src/storage/fts/utils/unicode_utils.cpp
src/storage/fts/utils/unicode_utils.h
src/share/text_analysis/ob_token_stream.cpp
src/storage/fts/ik/ob_ik_processor.cpp
src/storage/fts/ik/ob_ik_arbitrator.cpp
```

最后两个文件在最初对照构建时与基点相同；最终版增加了共享字符读取函数的调用。两个二进制都在同一源码路径、同一 Release Unity 构建目录中使用 AppleClang 21 和 `make -j3 seekdb` 构建。构建、原生测试和性能测试顺序执行，没有让本次编译与基准并行。

硬件为 Apple M3、8 GiB 内存，macOS arm64。每轮新建专用实例，固定 `cpu_count=2`、`memory_limit=2G`，使用同一个 28935 端口，结束后关闭进程；没有修改已有用户实例。宿主机仍有其他后台进程，最终各轮记录的一分钟 load average 在约 1.89～2.67 之间。原始清单保留了各轮负载、工具链、源码散列和二进制散列。

完整基准参数保持 `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。顺序为 **A B B A A B B A**，每个版本四次独立导入、构建和测量。全部轮次进入均值，单项退化保留为负值；先求各类内部平均，再对构建、分词、查询三类等权平均。

## 最终九项指标

构建单位为秒，分词和查询单位为毫秒。标准差为四轮之间的样本标准差，列内顺序是“对照 / 优化”。

| 指标 | 对照均值 | 优化均值 | 耗时下降 | 轮间标准差 |
| --- | ---: | ---: | ---: | ---: |
| `build_ik_all_sec` | 4.885250 | 4.853000 | +0.66% | 0.059640 / 0.059099 |
| `build_ik_content_sec` | 3.788750 | 3.684000 | +2.76% | 0.006449 / 0.085506 |
| `build_beng_en_sec` | 1.960750 | 1.959000 | +0.09% | 0.086284 / 0.005416 |
| `tokenize_ik_avg_ms` | 0.083525 | 0.082000 | +1.83% | 0.001056 / 0.000141 |
| `tokenize_beng_avg_ms` | 0.047950 | 0.047975 | -0.05% | 0.000342 / 0.000386 |
| `query_cn_avg_ms` | 0.970050 | 0.968300 | +0.18% | 0.011730 / 0.004364 |
| `query_beng_avg_ms` | 1.401075 | 1.399300 | +0.13% | 0.008127 / 0.003556 |
| `query_mixed_avg_ms` | 1.089250 | 1.082350 | +0.63% | 0.010116 / 0.002144 |
| `query_limit_avg_ms` | 1.252825 | 1.253925 | -0.09% | 0.008611 / 0.001556 |

三类耗时下降分别为：构建 **1.17%**、分词 **0.89%**、查询 **0.21%**；综合 **0.76%**。本地样本量和宿主机波动有限，不能据此宣称所有指标都有稳定改善。

## 正确性

- 原生差分检查覆盖全部可编码 UTF-8 码点、两种 UTF-16 字节序的全部 65,536 个双字节输入，以及中文数字集合的全部 1,114,112 个码点。
- 新字符读取函数同时比较类型、长度和错误码；增加全部 65,536 个双字节前缀，以及固定种子生成的 131,072 个随机前缀。NULL、空输入、截断和非法编码继续与旧路径比较。
- BENG 原生检查比较原始 trim/lowercase 算法的输出，覆盖大小写、非 ASCII、长词和借用/分配两种路径。
- 每轮运行原始 `ik_character_classification`、`beng_ascii_normalization` mysqltest，预期结果保持不变，包含重复词频检查。
- 每轮对全部 20,000 篇文档的四个文本字段计算 TOKENIZE 完整输出摘要；TEXT 字段显式转换为 CHAR 后传入，两边输入一致。还逐行核对四类 MATCH 的 ID 和分数，以及确定排序的 LIMIT 结果。最终八轮的六个 SHA-256 摘要完全相同，并非只比较命中数。
- A/B 汇总脚本的回归检查验证三类等权、退化不截断、全部样本计入，以及无效测量拒绝。

原生分类辅助微基准最终中位数为 155.278 ms → 141.646 ms，下降 8.78%。它只测字符分类，不包含完整索引构建或 SQL 查询，未拿它代替上述九项指标。

## 保留的开发测量

每一阶段都使用四组完整 A/B，保留原始报告和全部样本。最终提交由实现和正确性决定，没有挑选均值最高的中间版本作为最终结果。

| 阶段 | 同机综合耗时下降 | 全部数据 |
| --- | ---: | --- |
| 已有单次分类解码与 BENG 改动 | -0.62% | [`initial/comparison.json`](competition-validation-results/fts-task4-macos-20261111/initial/comparison.json) |
| ASCII 直接分类 | +0.32% | [`ascii/comparison.json`](competition-validation-results/fts-task4-macos-20261111/ascii/comparison.json) |
| ASCII 长度与分类合并 | +0.81% | [`ascii-width/comparison.json`](competition-validation-results/fts-task4-macos-20261111/ascii-width/comparison.json) |
| 完整 UTF-8 长度与分类合并，最终提交 | +0.76% | [`final/comparison.json`](competition-validation-results/fts-task4-macos-20261111/final/comparison.json) |

每个阶段目录包含 `manifest.json`、`comparison.json`，及每轮的原始报告、运行日志、正确性摘要和固定基线参考分。首次补充逐行检查时发现 TEXT 参数需要先转为 CHAR；那次未完成的探测没有进入任何 A/B 汇总。

## 固定 CI 基线与同机比较的区别

本次没有修改 `fts_large_bench.sh`、数据生成器、初始化 SQL、评分脚本或固定基线；五个文件的 SHA-256 记录在每个阶段的清单中。

历史成功的 [GitHub CI 42949287011](https://github.com/WatingCL/seekdb/actions/runs/42949287011) 对提交 `d0260b7d` 的完整报告按固定基线得到参考分 **100/100**，原始文件保存在 [`prior-ci/`](competition-validation-results/fts-task4-macos-20261111/prior-ci/)。这不是本次新提交的 CI 状态，也不能把旧基线与当前整仓库的差异归因于这几个 FTS 改动。正式分数以提交版本的统一评测结果为准。

## CI 兼容修复

上一轮 CI 已编译并通过原生、AI/文件 SQL 协议检查，但 MariaDB mysqltest 额外输出 `connection default;`，与 SQL-only 预期日志不一致，导致后续 FTS 基准被跳过。现在通过统一 prologue 关闭连接切换日志，并固定结果格式版本为 1；SQL、结果、警告和错误日志仍启用，原始测试和预期文件没有改动。参考 [MariaDB 10.6 mysqltest 源码](https://github.com/MariaDB/server/blob/10.6/client/mysqltest.cc) 中的连接日志开关。

兼容修复后本机五个 AI COLUMN 原始用例再次通过。CI 新增两项原始 FTS SQL 回归和 A/B 统计规则检查，随后运行原样完整基准并上传日志。

## 复现

先准备同一工具链和编译选项生成的两个二进制。对照可在本次提交的临时 worktree 中仅恢复上述六个生产文件到 `2845b8e06`，只构建 `seekdb`；最终源码和对照之间的完整差异另存为 [`reference-to-final.patch.gz`](competition-validation-results/fts-task4-macos-20261111/reference-to-final.patch.gz)。

macOS 需要 GNU Bash 与 GNU date：

```bash
PATH=/opt/homebrew/opt/coreutils/libexec/gnubin:$PATH \
python3 tools/benchmark/fts_compare_binaries.py \
  --before /path/to/seekdb-before --after /path/to/seekdb-after \
  --client "$PWD/deps/3rd/u01/obclient/bin/obclient" \
  --mysqltest "$PWD/deps/3rd/u01/obclient/bin/mysqltest" \
  --bash /opt/homebrew/bin/bash --port 28935 --pairs 4 \
  --output-dir /tmp/seekdb-fts-new-comparison
```

Linux 可直接使用系统 `bash` 和 `date`。输出目录必须为空，每轮使用新实例，脚本会删除和重建它自己的 `fts_large_bench` 测试库。准备二进制后应停止编译及其他测试再测量。

```bash
python3 unittest/storage/fts/run_ik_char_classification.py --build-dir build_release --benchmark
python3 unittest/storage/fts/run_ik_char_classification.py --build-dir build_release --english
python3 unittest/tools/test_fts_compare_binaries.py
```
