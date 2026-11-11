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

`ai_function_enhancement.test` 和 `ai_function_enhancement_extra.test` 使用 **8000** 端口，不能直接复用上例的 18000 端口配置。`file_sql_basic.test` 的部署 include 使用 SSH/SCP；无 SSH 的本机环境可使用上述 Python 协议测试。

## 原生单测

Linux 上可构建并运行对应模块。原生测试统一使用 `unittest/all_tests_main.cpp`，新增测试不能再定义独立入口。FTS 差分测试的独立运行模式由 `SEEKDB_FTS_STANDALONE_TEST` 显式启用。

```bash
make -C build_release -j2 sql_tests storage_tests share_tests
GTEST_FILTER='FileSql*:FileTableColumns.*:ObAIPromptTest.*:ObOpenAIUtilsTest.*:*AiColumn*:IkCharacterClassification.*:EnglishNormalization.*' \
  ./build_release/unittest/run_tests.sh \
  -R '^(sql|storage|share)_tests_shard_[0-9]+$' --output-on-failure
```

## 题目 3 的完成边界

目前只有 AI 列 Schema 字段、持久化任务存储、结果列赋值保护和严格读取检查的部分实现。AI COLUMN DDL、业务 DML 创建任务、后台执行、分组发布及 RETRY 语句尚未形成可运行的完整链路。

`ai_column/` 下的 mysqltest 文件是后续实现应通过的验收用例，不能把这些文件存在或任务存储单测通过视为题目 3 已完成。此前本地开发目录中的 DDL 补丁也是未完成的开发阶段，不能单独迁入后宣称功能可用。

## 题目 4 与 GitHub 验证

全文索引使用 `tools/benchmark/fts_large_bench.sh` 和固定基线评分，运行方式见仓库根目录 `seekdb题目4.md`。保持输入数据、SQL、计时与评分脚本不变。基准会重建 `fts_large_bench` 数据库，只能在专用实例运行。

`.github/workflows/vldb.yml` 在该分支推送时执行，也支持手动触发。它编译并执行相关单测、题目 1/2 的协议测试和完整 FTS 基准，上传日志及报告。它不代表题目 3 的端到端验收，也不能代替组委会的完整隐藏测试。GitHub 托管机器与正式基线硬件不同，性能分数仅供参考；局部分词函数的差分测试和微基准不能代替完整索引构建、TOKENIZE 和 MATCH 的验证。
