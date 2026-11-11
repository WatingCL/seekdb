# MiniVectorDB 运行说明

赛题一「完成一个轻量向量数据库」的实现。本文件是赛题 4.2 节要求的运行说明；
设计与验证记录见 [DESIGN.md](DESIGN.md)。

## 1. 语言与版本

| 项目 | 取值 |
| --- | --- |
| 实现语言 | C++17，仅使用标准库 |
| 编译器 | g++（Ubuntu 24.04 自带 g++ 13）；本地开发用 MinGW g++ 8.1.0 验证过同一份源码 |
| 第三方依赖 | 无。JSON 解析/序列化、SQL 词法与语法分析、存储与向量检索全部自研 |
| 构建工具 | GNU make（仓库内 `Makefile`） |
| 运行库 | 只需 `libstdc++`/`libc`，均随基础镜像提供；不链接任何数据库或索引库 |
| 目标平台 | Linux x86_64。源码同时可在 Windows 上编译，便于本地自测 |

## 2. 目录结构

```
minivectordb/
├── Makefile              # make 即在当前目录产出可执行文件 minivectordb
├── README.md             # 本文件（4.2 运行说明）
├── DESIGN.md             # 设计与验证记录（4.3）
├── src/
│   ├── main.cpp          # JSONL 协议循环、握手、shutdown
│   ├── json.{hpp,cpp}    # 单头风格 JSON 解析与序列化
│   ├── parser.{hpp,cpp}  # SQL 词法 + 递归下降语法分析
│   ├── value.{hpp,cpp}   # 值类型、UTF-8 字符数、码点比较、向量字面量
│   ├── storage.{hpp,cpp} # 追加日志、CRC、目录（catalog）、恢复扫描
│   └── engine.{hpp,cpp}  # 执行器、事务、向量检索
└── tools/
    ├── local_check.py    # 在裸进程上跑 mysqltest/ 公开用例（复用官方 _assertions.py）
    ├── crash_check.py    # 崩溃/恢复套件：回滚、SIGKILL、恢复中再杀、COMMIT 竞争
    └── scale_check.py    # 20 万行 × 128 维规模与内存占用测量
```

## 3. 构建

### 3.1 从源码直接构建

```bash
make -C minivectordb            # 产出 minivectordb/minivectordb
```

等价的单行命令（无 make 时）：

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pthread minivectordb/src/*.cpp -o minivectordb/minivectordb
```

### 3.2 构建镜像

仓库根目录的 `Dockerfile` 使用多阶段之外的单阶段源码构建：基于 `ubuntu:24.04`，
安装 `g++ make`，`COPY minivectordb/ /src/minivectordb/` 后 `make`，不复制任何预编译二进制。

```bash
docker build -t minivectordb:latest .
```

根目录的 `.dockerignore` 只把 `minivectordb/` 放进构建上下文：提交树本身只有 0.4 MiB，
但仓库克隆里有 `.git`，不排除会让 `docker build .` 先把整个克隆上传成上下文。

构建与公开用例已在 `ubuntu-24.04` + g++ 13 上用真实判题参数（`--read-only`、`--cap-drop=ALL`、
`--network=none`、`--cpus=2`、`--memory=2g`、`--pids-limit=128`）复现通过，记录见 `DESIGN.md` 7.4。

### 3.3 启动

前台启动并保持标准输入打开（评测方式，`-i`，不用 `-d`/`-t`，无端口映射）：

```bash
docker run --rm -i \
  -v minivectordb-data:/data \
  --cpus=2 \
  --memory=2g \
  minivectordb:latest
```

- **启动入口**：`ENTRYPOINT ["/src/minivectordb/minivectordb"]`，`CMD ["--data-dir", "/data"]`。
- **参数**：`--data-dir <目录>`，唯一支持的命令行参数；缺省为 `/data`。目录不存在时自动创建。
- **数据目录**：所有持久化文件都写在 `--data-dir` 下，共三类：`meta.json`（表结构目录）、
  `<表名>.log`（每表一个追加日志，存行数据）、`txn.log`（全局提交点日志）。
- **通道**：请求走 stdin，响应走 stdout，日志走 stderr。stdout 除协议 JSON 外不输出任何内容，
  每行写完立即 flush。
- 本地不起容器时同样可用：`./minivectordb/minivectordb --data-dir /some/dir`。

## 4. 协议

进程启动后先完成崩溃恢复，然后主动输出且仅输出一行握手：

```json
{"type":"ready"}
```

之后按行读取请求，每行一个 JSON 对象：

```json
{"id": 1, "op": "execute", "sql": "SELECT 1;", "search_mode": "exact"}
{"id": 2, "op": "shutdown"}
```

- `id` 原样回传，类型保持不变（数字回数字，字符串回字符串）。
- `op` 只有 `execute` 和 `shutdown`；其他取值返回 `unsupported_op` 错误。
- `search_mode` 接受 `exact`（缺省）与 `ann`。当前实现下 `ann` 走与 `exact` 相同的精确扫描路径，
  因此召回率为 1.0（见 DESIGN.md「已知限制」）。
- `shutdown` 先持久化、回 `ok`、flush，再以退出码 0 结束进程。

响应形态：

```json
{"id":1,"ok":true}                                                  // DDL / BEGIN / COMMIT / ROLLBACK
{"id":2,"ok":true,"affected_rows":3}                                // INSERT / UPDATE / DELETE
{"id":3,"ok":true,"columns":["id","title"],"rows":[[1,"alpha"]]}    // SELECT
{"id":4,"ok":false,"error":{"code":"unknown_table","message":"docs"}}
```

BIGINT 以 JSON 整数直出（不走浮点），VARCHAR 以 JSON 字符串直出，距离值为 JSON 浮点数。

## 5. SQL 支持范围

支持的语句（大小写不敏感，必须以 `;` 结尾，一条请求一条语句）：

```sql
CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(32), tag VARCHAR(16), v VECTOR(128));
CREATE VECTOR INDEX idx ON t (v);
INSERT INTO t VALUES (1, 'a', 'x', '[1,0,0]'), (2, 'b', 'y', '[0,1,0]');
INSERT INTO t (id, name) VALUES (3, 'c');
SELECT * FROM t;
SELECT id, name FROM t WHERE tag = 'x' AND id > 1 ORDER BY name DESC LIMIT 10;
SELECT id, L2_DISTANCE(v, '[1,0,0]') AS d FROM t ORDER BY d ASC LIMIT 5;
SELECT id, L2_DISTANCE(v, '[1,0,0]') AS d FROM t WHERE tag = 'x' ORDER BY d ASC LIMIT 5;
UPDATE t SET tag = 'z' WHERE id = 1;
DELETE FROM t WHERE id = 2;
BEGIN; COMMIT; ROLLBACK;
```

约束与语义：

- 建表必须有且仅有一个 `BIGINT PRIMARY KEY`，最多一个 `VECTOR(n)` 列；列类型仅
  `BIGINT`、`VARCHAR(n)`、`VECTOR(n)`。
- `WHERE` 只支持 `=`、`<`、`<=`、`>`、`>=` 的「列 op 字面量」比较，用 `AND` 连接；
  不支持 `OR`、`!=`、`LIKE`、`IN`、`BETWEEN`、`IS NULL`、列与列比较。
- 不支持 `JOIN`、子查询、`GROUP BY`、`HAVING`、`UNION`、聚合函数、`DISTINCT`、`LIMIT offset`、
  `NULL`/布尔字面量、`ALTER`/`DROP`。
- 向量检索先过滤再取 TopK；`L2_DISTANCE` 为欧氏距离（开方后），以 float32 落盘后计算；
  距离相同按主键升序；无 `ORDER BY` 时结果按主键升序。
- `VARCHAR(n)` 的 n 按 Unicode 字符数（码点数）计；字符串比较按码点顺序。
- 拒绝 NaN/Infinity、维度不符、超出 VARCHAR 长度、BIGINT 溢出等输入。
- 事务为单活动事务；`BEGIN` 中再 `BEGIN` 返回 `transaction_active`；
  事务外的 `COMMIT`/`ROLLBACK` 为 no-op 且返回 ok；进程被杀时未提交事务整体丢弃。

## 6. 错误处理

不支持的语句一律报错，不静默忽略。错误码：

| code | 触发场景 |
| --- | --- |
| `invalid_request` | 请求不是 JSON 对象，或缺少字符串型 `sql` |
| `unsupported_op` | `op` 不是 `execute`/`shutdown` |
| `syntax_error` | 词法/语法错误、语句尾有多余 token |
| `unsupported_sql` | 语法能识别但语义不支持（`OR`、`JOIN`、聚合、`NULL` 字面量等） |
| `invalid_schema` | 建表列定义非法（缺主键、多个向量列、重名列、非法长度/维度） |
| `duplicate_table` | 表已存在 |
| `unknown_table` / `unknown_column` | 表或列不存在 |
| `duplicate_key` | 主键冲突 |
| `column_count_mismatch` | `VALUES` 个数与列数不符 |
| `type_mismatch` | 值类型与列类型不符、`L2_DISTANCE` 参数不是向量列、整数超出 BIGINT 范围 |
| `invalid_vector` | 向量字面量非法或含 NaN/Inf、分量超出 float32 范围 |
| `dimension_mismatch` | 向量维度与列声明不符 |
| `value_too_long` | 字符串超出 `VARCHAR(n)` 的字符数上限 |
| `transaction_active` | 事务中再 `BEGIN` |
| `storage_error` | 磁盘读写、fsync、截断失败或日志损坏 |

诊断信息只写 stderr，前缀 `[minivectordb]`，不污染 stdout。

## 7. 从空数据目录开始的完整示例

下面的输出是在一个空数据目录上真实运行得到的（`--data-dir` 指向一个新建空目录）。
请求逐行写入 stdin，响应逐行从 stdout 读出。

请求：

```json
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, title VARCHAR(32), tag VARCHAR(16), v VECTOR(4));","search_mode":"exact"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'alpha','red','[1,0,0,0]'),(2,'beta','blue','[0,1,0,0]'),(3,'gamma','red','[1,1,0,0]');","search_mode":"exact"}
{"id":3,"op":"execute","sql":"SELECT id, title, tag FROM docs WHERE tag = 'red' ORDER BY id;","search_mode":"exact"}
{"id":4,"op":"execute","sql":"BEGIN;","search_mode":"exact"}
{"id":5,"op":"execute","sql":"INSERT INTO docs VALUES (4,'delta','green','[0,0,1,0]');","search_mode":"exact"}
{"id":6,"op":"execute","sql":"UPDATE docs SET tag = 'green' WHERE id = 1;","search_mode":"exact"}
{"id":7,"op":"execute","sql":"SELECT id, tag FROM docs ORDER BY id;","search_mode":"exact"}
{"id":8,"op":"execute","sql":"ROLLBACK;","search_mode":"exact"}
{"id":9,"op":"execute","sql":"SELECT id, tag FROM docs ORDER BY id;","search_mode":"exact"}
{"id":10,"op":"execute","sql":"SELECT id, title, L2_DISTANCE(v, '[1,0.5,0,0]') AS d FROM docs ORDER BY d ASC LIMIT 2;","search_mode":"exact"}
{"id":11,"op":"shutdown"}
```

响应：

```json
{"type":"ready"}
{"id":1,"ok":true}
{"id":2,"ok":true,"affected_rows":3}
{"id":3,"ok":true,"columns":["id","title","tag"],"rows":[[1,"alpha","red"],[3,"gamma","red"]]}
{"id":4,"ok":true}
{"id":5,"ok":true,"affected_rows":1}
{"id":6,"ok":true,"affected_rows":1}
{"id":7,"ok":true,"columns":["id","tag"],"rows":[[1,"green"],[2,"blue"],[3,"red"],[4,"green"]]}
{"id":8,"ok":true}
{"id":9,"ok":true,"columns":["id","tag"],"rows":[[1,"red"],[2,"blue"],[3,"red"]]}
{"id":10,"ok":true,"columns":["id","title","d"],"rows":[[1,"alpha",0.5],[3,"gamma",0.5]]}
{"id":11,"ok":true}
```

逐条对应赛题要求的覆盖面：

- **写入**：`id=2` 一次插入 3 行，`affected_rows:3`。
- **普通查询**：`id=3` 带 `WHERE` 过滤 + `ORDER BY`，只返回 tag 为 red 的两行。
- **事务内可见**：`id=7` 在 `BEGIN` 之后能看到自己未提交的插入（4 行）和更新（id=1 变 green）。
- **回滚**：`id=9` 回滚后回到 3 行，id=1 的 tag 复原为 red，id=4 消失。
- **向量检索**：`id=10` 取 TopK=2。`[1,0,0,0]` 与 `[1,1,0,0]` 到查询向量 `[1,0.5,0,0]` 的欧氏距离
  都是 0.5，并列时按主键升序，所以返回 1 和 3。

此时数据目录内容（字节数为实测值）：

```
docs.log  195      # 行数据，追加日志
meta.json 253      # 表结构目录
txn.log    25      # 提交点日志
```

**重启**：用同一个 `--data-dir` 再起一个进程，数据完好，被回滚的行不会复活。

```json
{"id":1,"op":"execute","sql":"SELECT id, title FROM docs ORDER BY id;","search_mode":"exact"}
{"id":2,"op":"shutdown"}
```

```json
{"type":"ready"}
{"id":1,"ok":true,"columns":["id","title"],"rows":[[1,"alpha"],[2,"beta"],[3,"gamma"]]}
{"id":2,"ok":true}
```

## 8. 本地自测

三个工具都不依赖 Docker，直接对裸进程跑（`--exe` 指向可执行文件）。

```bash
# 官方公开用例（mysqltest/ 下 20 个用例，复用官方 _assertions.py 的断言实现）
py -3 minivectordb/tools/local_check.py \
  --command '["./minivectordb/minivectordb.exe","--data-dir","{data_dir}"]' \
  --output protocol-results/local-9

# 崩溃与恢复：回滚持久性、SIGKILL、恢复过程中再被杀、COMMIT 竞争
py -3 minivectordb/tools/crash_check.py --output protocol-results/crash.json

# 规模与内存：20 万行 × 128 维，测吞吐、TopK 延迟、常驻内存、重启恢复
py -3 minivectordb/tools/scale_check.py --rows 200000 --queries 20 \
  --output protocol-results/scale-200k.json
```

在 Linux 上把 `./minivectordb/minivectordb.exe` 换成 `./minivectordb/minivectordb`、
`py -3` 换成 `python3` 即可。容器内的等价验证：

```bash
py -3 protocol_check/run.py --handshake-only
```

最近一次本地结果见 DESIGN.md 第 7 节。
