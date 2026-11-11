# 题目 1～4 的构建与验证

仓库分支 `competition/tasks-1-4` 保存数据库内核实现、题目说明、Mock 服务和回归用例。测试文件必须随源代码一起提交；仅在本地存在的未跟踪文件不会进入 GitHub 提测环境。

## 构建

首次构建需要初始化依赖，之后才可编译。`build_release` 目录存在不代表已经生成数据库程序。

```bash
./build.sh release --init
make -C build_release -j2 seekdb
test -x build_release/src/observer/seekdb
```

macOS 默认通过 `xcrun` 选择与当前 SDK 配套的编译器。升级 Xcode 或切换编译器后，应使用新的构建目录或清除旧的 CMake 配置，避免复用旧编译器缓存。新版 AppleClang 的兼容处理仅限构建警告及 Boost 上游的数值类型特征修复，不修改数据库算法或下载的依赖包。

## 题目 1、2 的 SQL 与 HTTP 验证

使用专用实例。下列协议测试会创建和删除自己的表、模型、Endpoint，并暂时设置 `secure_file_priv`；不要连接业务实例。

```bash
mkdir -p /tmp/seekdb-competition-local
build_release/src/observer/seekdb --nodaemon --port=2885 \
  --base-dir=/tmp/seekdb-competition-local \
  --parameter cpu_count=2 --parameter memory_limit=2G
```

如果端口已被其他实例占用，应更换端口并同步调整客户端参数。连接后检查 `SELECT @@version_comment` 中的提交号，确认查询实际到达本次构建的实例；仅检查 `SELECT 1` 成功可能误连旧实例。

另开终端启动 Mock 服务，再运行测试。`CLIENT` 可以是 `mysql` 或 `obclient` 的绝对路径。

```bash
python3 tools/deploy/mysql_test/test_suite/ai_function/mock_ai_service.py \
  --host 127.0.0.1 --port 18000
```

```bash
CLIENT="$PWD/deps/3rd/u01/obclient/bin/obclient"
"$CLIENT" --no-defaults -h127.0.0.1 -P2885 -uroot \
  -e 'CREATE DATABASE IF NOT EXISTS test'
python3 unittest/sql/engine/expr/test_ai_function_protocol.py \
  --client "$CLIENT" --port 2885 --mock-port 18000
python3 unittest/sql/engine/basic/test_file_sql_protocol.py \
  --client "$CLIENT" --port 2885
```

`ai_function_enhancement.test` 和 `ai_function_enhancement_extra.test` 使用 **8000** 端口，不能直接复用上例的 18000 端口配置。`file_sql_basic.test` 的部署 include 使用 SSH/SCP；无 SSH 的本机环境可使用上述 Python 协议测试。文件 SQL 协议测试通过 TCP 查询数据，通过实例的 `@@socket` 修改及恢复 `secure_file_priv`，遵守只允许 Unix socket 管理该变量的限制；也可用 `--admin-socket` 显式指定管理 socket。

## 原生单测

初始化依赖后，可单独验证解析器首次生成与缓存行为。测试在临时目录运行真实 Bison/Flex，不修改当前构建目录：

```bash
python3 unittest/sql/parser/test_parser_generation_cache.py
```

Linux 上可构建并运行对应模块。原生测试统一使用 `unittest/all_tests_main.cpp`，新增测试不能再定义独立入口。FTS 差分测试的独立运行模式由 `SEEKDB_FTS_STANDALONE_TEST` 显式启用。

```bash
make -C build_release -j2 sql_tests storage_tests share_tests
GTEST_FILTER='FileSql*:FileDescribeParser.*:FileTableColumns.*:ObAIPromptTest.*:ObOpenAIUtilsTest.*:*AiColumn*:IkCharacterClassification.*:EnglishNormalization.*' \
  ./build_release/unittest/run_tests.sh \
  -R '^(sql|storage|share)_tests_shard_[0-9]+$' --output-on-failure
```

## 题目 3 的完成边界

目前只有 AI 列 Schema 字段、持久化任务存储、结果列赋值保护和严格读取检查的部分实现。AI COLUMN DDL、业务 DML 创建任务、后台执行、分组发布及 RETRY 语句尚未形成可运行的完整链路。

`ai_column/` 下的 mysqltest 文件是后续实现应通过的验收用例，不能把这些文件存在或任务存储单测通过视为题目 3 已完成。此前本地开发目录中的 DDL 补丁也是未完成的开发阶段，不能单独迁入后宣称功能可用。

当前 `ai_model_endpoint_ddl.test` 还包含开发中的 `max_concurrency` 配置及视图列断言；实现尚不支持该参数，该用例在创建 Endpoint 时会失败。不要将其报为已通过，也不能据此认为并发限流已经实现。

## 题目 4 与 GitHub 验证

全文索引使用 `tools/benchmark/fts_large_bench.sh` 和固定基线评分，运行方式见仓库根目录 `seekdb题目4.md`。保持输入数据、SQL、计时与评分脚本不变。基准会重建 `fts_large_bench` 数据库，只能在专用实例运行。

`.github/workflows/vldb.yml` 在该分支推送时执行，也支持手动触发。它编译并执行相关单测、题目 1/2 的协议测试和完整 FTS 基准，上传日志及报告。它不代表题目 3 的端到端验收，也不能代替组委会的完整隐藏测试。GitHub 托管机器与正式基线硬件不同，性能分数仅供参考；局部分词函数的差分测试和微基准不能代替完整索引构建、TOKENIZE 和 MATCH 的验证。

## 2026-11-11 本机验证记录

macOS arm64，8 GiB 内存；隔离实例配置 `cpu_count=2`、`memory_limit=2G`。编译器为 AppleClang 21，Release Unity 构建完成，数据库成功启动。

- 使用生产链接库运行的 22 项原生回归测试全部通过，包括解析器、文件读取/导出、列名解析、任务存储、AI 请求工具及分词差分测试。
- AI 协议测试：75 项 SQL 检查及 HTTP 批次、乱序恢复、NULL、LIMIT、故障响应断言通过。
- 文件 SQL 协议测试：28 项检查通过，包括三种格式、JOIN、聚合、DESCRIBE、glob、COPY 和路径安全。
- mysqltest：`ai_prompt`、`ai_function_enhancement`、`ai_function_enhancement_extra`、`ik_character_classification`、`beng_ascii_normalization` 通过。AI 用例在临时副本中将 Mock 地址统一改为本机隔离端口，期望输出做相同地址替换，其余断言保持原样。
- `ai_model_endpoint_ddl` 仍在 `max_concurrency` 处失败；AI COLUMN 端到端功能仍未完成。
- 完整 FTS 基准完成，原始报告见 [fts-macos-20261111.txt](competition-validation-results/fts-macos-20261111.txt)，参考评分见 [fts-macos-20261111-score.json](competition-validation-results/fts-macos-20261111-score.json)。报告保留了测试时的 `git_head` 和 `git_dirty=1`，对应本次提交前工作区。该分数比较的是本机与不同硬件上的固定基线，不是同机优化前后对比或正式成绩。

GitHub 首次运行在依赖初始化阶段失败：清单引用的 `obshell-4.5.0.1-52026082614.el8.x86_64.rpm` 返回 HTTP 404。EL8/EL9 x86_64 清单已改为仓库中原有注释列出的、经下载地址确认仍可用的 `4.5.0.0-12026050816` 包；其他社区工具下载地址也已核对。
