# MiniVectorDB 设计与验证记录

赛题 4.3 节要求的设计与验证记录。运行方式、SQL 支持范围与错误码见 [README.md](README.md)。

本文件里的所有数字都是本地实测值，命令与原始输出见第 7 节；正式成绩以平台对锁定 commit
的评测为准。

## 1. 总体结构

```
stdin ──▶ main.cpp   协议循环：逐行读请求，逐行写响应，每行 flush
            │
            ├─▶ json.cpp     JSON 解析/序列化（自研，无第三方库）
            ├─▶ parser.cpp   SQL 词法 + 递归下降语法分析 → Stmt
            └─▶ engine.cpp   执行器：事务、可见性、向量检索
                     │
                     └─▶ storage.cpp  追加日志、CRC32、catalog、恢复扫描
```

进程启动顺序（`main.cpp`）：创建数据目录 → 构造 `Engine`（**此时完成崩溃恢复**）→
输出 `{"type":"ready"}` → 进入请求循环。恢复放在握手之前，是为了保证任何驱动在看到
ready 后立刻发第一条请求时，索引已经建好，不会读到半成品状态。

单进程、单线程、单活动事务。所有写都由这一个线程按顺序追加，这是后面「已提交集合是日志前缀」
这条不变式成立的前提。

## 2. 记录布局

### 2.1 文件

`--data-dir` 下三类文件，没有 WAL 双写，也没有独立的索引文件：

| 文件 | 内容 | 写入方式 |
| --- | --- | --- |
| `meta.json` | 表结构目录（表名、列名/类型/长度/维度/主键、是否建了向量索引） | temp + fsync + rename 原子替换 |
| `<表名>.log` | 该表的行数据，追加日志 | 只追加；回滚时截断 |
| `txn.log` | 全局提交点日志，只有 `'T'` 记录 | 只追加 |

### 2.2 记录格式

每条记录 = 8 字节头 + 变长 payload：

```
偏移  长度  字段
0     4     u32le  payload 长度
4     4     u32le  payload 的 CRC32（多项式 0xEDB88320，反射式）
8     1     op：'I' 插入/更新版本，'D' 删除墓碑，'T' 事务提交
9     8     u64le  xid（事务号）
17    8     i64le  主键
25    ...   body
```

`kPrefixSize = 17`（op + xid + pk），body 只在 `'I'` 记录里存在。

body 按表结构顺序紧凑编码，无分隔符、无字段名：

- `BIGINT` → 8 字节 i64le
- `VARCHAR` → 4 字节 u32le 字节长度 + UTF-8 原文
- `VECTOR(n)` → n × 4 字节 float32（IEEE-754 小端）

向量以 float32 落盘，距离也在 float32 上计算，因此「写进去再读出来」与「直接算」结果一致
（公开用例 `vector_float32` 覆盖了这一点）。

一条 128 维向量行的实际开销：17 + 8 + 512 + 若干 VARCHAR ≈ 555 字节，其中 512 字节是向量本身。

### 2.3 为什么用「追加日志 + 前缀规则」而不是页式存储

赛题的性能口径按**物理读写字节**计费（含写回），而崩溃模型只杀进程、不断电。在这个前提下：

- 单追加文件、无 WAL 双写 → 一次写入只产生一份物理字节。
- 更新/删除不改旧记录，只追加新版本或墓碑 → 没有读-改-写，没有随机写放大。
- 提交点集中在 `txn.log` → 提交只需一次 fsync，而不是每表一次（每表一次会造成
  「部分表已提交」的中间态）。

代价是日志会随更新/删除增长，且没有压缩（见第 8 节已知限制）。

## 3. 内存中只放索引，不放行数据

这是为了满足赛题 2.3 节「不得靠把数据全装进内存来通过存储测试」。

每张表在内存里只有一个 `std::map<long long, RowState>`：

```cpp
struct RecPos { uint64_t off; uint32_t len; };   // payload 在日志里的位置和长度
struct RowState { RecPos pos; bool deleted = false; };
```

即「主键 → 当前版本在日志里的偏移」。行内容（含向量）**只在磁盘上**。

实测：20 万行 × 128 维，磁盘 554.9 字节/行，常驻 119.0 字节/行，常驻内存是数据量的
约 1/4.7。整个进程峰值 RSS 23.8 MiB，而数据文件 110,979,381 字节（105.8 MiB）。
`std::map` 每节点的三个指针加红黑树元数据是这 119 字节的主要来源，后续可换成开放寻址
哈希表把它压到 ~24 字节/行。

扫描一律走 `LogReader`：1 MiB 块缓冲、顺序前向读、查询路径不校验 CRC（CRC 只在恢复时校验）。
`LogReader::setEnd()` 把扫描终点钉在「本次扫描开始前的日志末尾」，所以在扫描过程中
追加写入（`UPDATE`/`DELETE` 就是这样边扫边写的）不会让扫描追上自己的写入而死循环。

## 4. 向量检索方案

### 4.1 当前实现：并行度为 1 的精确扫描 + 有界 TopK 堆

`exact` 与 `ann` 两种 `search_mode` 目前都走同一条精确扫描路径，所以召回率恒为 1.0，
不存在近似误差。

TopK 的关键优化是**不物化全部候选**：

```
ORDER BY <距离别名> ASC LIMIT k   →  k 个元素的 max-heap
```

堆的比较序是 `(距离升序, 主键升序)`，与通用排序路径的 tie-break 完全一致，因此有界堆的
输出与「全量排序后取前 k」逐行相同。只有进入堆的行才构造 JSON 值，未入选的行连字符串
都不生成。内存 O(k)，与表大小无关。

实测差分验证（20 万行）：有界堆路径与全量排序路径结果完全一致 —— 无过滤时 199000 行
匹配，带 `WHERE tag = 'tag-7'` 时 2041 行匹配。

引入有界堆之前，全量物化候选的峰值 RSS 是 87.4 MiB、TopK 中位数 235 ms；改成有界堆后
是 23.7 MiB、63 ms。

过滤先于 TopK：`WHERE` 在计算距离之前求值，被过滤掉的行不参与堆。

`CREATE VECTOR INDEX` 被接受并记录在 catalog 里（重启后保留），但当前不构建额外的
索引结构 —— 精确扫描本身就是答案，索引只会增加维护成本和写放大。

### 4.2 距离与排序语义

- `L2_DISTANCE(v, '[...]')` 返回欧氏距离（平方和后开方）。
- 先过滤，再算距离，再取 TopK。
- 距离并列按主键升序（公开用例 `vector_ties_and_filter` 覆盖）。
- 无 `ORDER BY` 时按主键升序输出：日志顺序是写入顺序，直接返回会不稳定，所以统一按主键排。
- 拒绝 NaN/Inf、维度不符、float32 溢出。

## 5. 事务处理

### 5.1 写入路径：先追加，再改索引

`INSERT`/`UPDATE`/`DELETE` 在**语句执行时**就把记录追加到表日志（`AppendLog::append`），
同时更新内存索引，并把被覆盖的旧 `RowState` 压进 `undo_` 栈。

这样做的目的是让未提交事务能读到自己的写入 —— 否则同一事务里 `INSERT` 之后 `SELECT`
看不到刚写的行。但追加的记录此刻还没有提交点，所以必须能在回滚时撤掉（见 5.3）。

`AppendLog` 用独立的 `FILE*` 写，`LogReader` 用另一个 `FILE*` 读。两个 stdio 流互不可见，
所以每次开扫描前必须先 `AppendLog::flush()`（只 `fflush`，不 fsync）。漏掉这一步会出
一个很隐蔽的 bug：自动提交语句因为 commit 时 sync 过所以正常，而显式事务里最后一条写入
对随后的 `SELECT` 不可见。这个 bug 在崩溃探针里被抓到过（事务内可见行数 602 而不是 603），
现已修复并由 `crash_check.py` 的 B1 断言守住。

### 5.2 提交：单点提交 + fsync

```
COMMIT（或自动提交的一条 DML）
  1. 对本次 touched 的每张表日志 fflush + fsync
  2. 向 txn.log 追加一条 'T' 记录（op='T', xid=本次事务号）
  3. 对 txn.log fflush + fsync
  4. 应答 ok
```

应答在第 3 步之后才发出，所以「已应答 = 已持久」。步骤 1 和 3 之间被杀，
`txn.log` 里没有这条 xid 的提交记录，恢复时整个事务被丢弃 —— 不会出现部分提交。

xid 由单写者顺序分配、顺序提交，因此**已提交集合永远是日志的一个前缀**：
恢复时判据就是 `xid <= maxCommitted`。

`txn.log` 是全局唯一提交点，不是每表一个。每表一个标记会让「A 表已提交、B 表未提交」
成为可恢复的中间态，违反原子性。

自动提交 = 每条 DML 一个 xid。`BEGIN` 之后的语句共用一个 xid，直到 `COMMIT`/`ROLLBACK`。

### 5.3 回滚：截断日志

回滚不能只改内存索引 —— 已经追加进日志的记录如果留在文件里，将来某次恢复在
`xid <= maxCommitted` 判据下可能把它当成已提交的 resurrect 出来（被回滚的 xid 小于
后来某个已提交 xid 时就会发生）。

所以 `Mark` 里同时记录两件事：

```cpp
struct Mark { size_t undo; std::map<std::string, uint64_t> logPos; };
```

`undo` 是撤销栈高度，`logPos` 是每张表日志当时的字节位置。回滚时：

1. 把每张表日志 `truncateBack()` 到 mark 记录的位置（先 fflush 再 ftruncate）；
2. 按 `undo_` 逆序恢复内存索引（有旧版本的还原，没有的删除）；
3. 把 `undo_` 截回 mark 高度。

语句级回滚同理：`exec()` 在每条会修改数据的语句前取 `stmtMark_`，语句出错时只回滚到
这个 mark，已提交内容不受影响。

`shutdown` 时若仍在事务中，等价于 `ROLLBACK`（公开用例 `transaction_shutdown_rollback`）。

### 5.4 事务语义边界

- 单活动事务；`BEGIN` 中再 `BEGIN` → `transaction_active`。
- 事务外的 `COMMIT`/`ROLLBACK` 是 no-op，返回 ok。
- 语句失败只回滚该语句，事务仍然存活（公开用例 `transaction_failed_insert`）。
- DDL（`CREATE TABLE`/`CREATE VECTOR INDEX`）立即持久化，**不参与回滚**（见第 8 节）。

## 6. 恢复流程

启动时按顺序执行，全程只读日志、不解码任何行内容：

```
1. 读 meta.json（temp+rename 写入，所以要么是旧版本要么是新版本，不会半截）
   → 得到表结构目录
2. 扫 txn.log（LogReader + CRC 校验）
   → maxCommitted = 最大的已提交 xid
   → committedEnd = 最后一条完好记录的结束偏移
   → 以 committedEnd 打开 txn.log，把后面的撕裂尾巴截掉
3. 对每张表，扫 <表名>.log（LogReader + CRC 校验）
   → xid > maxCommitted 的记录直接跳过（从未到达提交点）
   → 'D' 记录：从索引里 erase
   → 'I' 记录：索引[pk] = {payload 偏移, 长度}
   → 记录 committedEnd
   → 以 committedEnd 打开表日志并截断尾巴
4. nextXid = 所有日志里见过的最大 xid + 1
5. 输出 {"type":"ready"}
```

要点：

- **不解码行**。恢复只读记录头里的 op/xid/pk，body 一律跳过。这是恢复时间与数据量
  近似线性、且内存占用极低的原因。20 万行 × 128 维（105.8 MiB 日志）恢复 0.234 秒，
  峰值 RSS 21.3 MiB。
- **CRC 只在恢复时校验**。查询路径不校验，省掉一遍全量 CRC 计算。
- **截断到 committedEnd**，而不是截断到「最后一条 CRC 完好的记录」。两者差别在于：
  一条 CRC 完好但 xid 未提交的记录，必须被物理删掉，否则下次恢复可能误判。
- **恢复可重复执行且幂等**。第 3 步的截断本身也是可中断的：在恢复过程中再被杀，
  下一次恢复会重新扫、重新截到同一个位置。`crash_check.py` 的 C 阶段专门验证这一点。
- `meta.json` 用 temp + fsync + rename 写入，所以恢复途中被杀也不会留下半截 catalog。

### 6.1 恢复性能的一次修正

最初的实现里，`AppendLog::open()` 无条件调用 `intactEnd()` 重新扫一遍日志找完好尾部，
而 `intactEnd()` 每条记录都 `fseek` 一次，把 stdio 缓冲完全废掉；同时恢复扫描用的是
逐记录 `fread`。20 万行恢复要 1.45 秒。

两处修改：

1. `intactEnd()` 改为 1 MiB 块缓冲顺序读；
2. 恢复路径直接把扫描阶段已经算出的 `committedEnd` 作为 limit 传给 `AppendLog::open()`，
   跳过第二次全量扫描（`limit == UINT64_MAX` 时才走 `intactEnd()`，只剩 `CREATE TABLE`
   在新建空文件上用）。

结果：1.45 s → 0.234 s，约 6.2 倍。

## 7. 本地验证记录

环境：Windows 11，MinGW g++ 8.1.0，`-std=c++17 -O2 -Wall -Wextra`，Python 3.12.2。
无 Docker/WSL，因此协议与崩溃验证都直接对裸进程跑（协议本身只依赖 stdin/stdout，
不需要容器）。资源与崩溃结论最终以 Linux 复测为准。

### 7.1 公开用例：20/20 PASS

```
py -3 minivectordb/tools/local_check.py \
  --command '["./minivectordb/minivectordb.exe","--data-dir","{data_dir}"]' \
  --output protocol-results/local-9
```

`local_check.py` 复用官方 `protocol_check/_assertions.py` 的断言实现，直接驱动裸进程，
支持用例里的 `--restart` 与 `--mode exact|ann` 指令。

```
sql_crud: PASS              sql_comparisons: PASS       sql_ordering: PASS
sql_empty: PASS             sql_primary_key: PASS       sql_unicode: PASS
sql_bigint: PASS            sql_unsupported: PASS       storage_restart: PASS
vector_euclidean: PASS      transaction_commit: PASS    transaction_rollback: PASS
storage_mutations: PASS     storage_multiple_tables: PASS
vector_ties_and_filter: PASS  vector_float32: PASS      vector_index_maintenance: PASS
transaction_failed_insert: PASS  transaction_autocommit_atomic: PASS
transaction_shutdown_rollback: PASS
```

公开用例里期望的错误码共三种：`duplicate_key`、`unsupported_sql`、`dimension_mismatch`，
实现均按此返回。

### 7.2 崩溃与恢复：12/12 PASS

```
py -3 minivectordb/tools/crash_check.py --output protocol-results/crash.json
```

总耗时 12.4 秒，12 项断言全过，0 失败。四个阶段：

**A 回滚的持久性**（3 项）。`BEGIN` → `INSERT (9,'x')` → `UPDATE id=1` → `ROLLBACK` →
再写两行 → 干净关闭 → 重启 → 再重启。三次读到的都是 `[(1,'a'),(3,'c'),(4,'d')]`，
被回滚的 `9` 和被回滚的更新都没有复活。

**B 大事务中被 SIGKILL**（5 项）。基线 3 行，`BEGIN` 后单行插入 600 行（约 90 字节/行，
远超 stdio 缓冲，OS 已持有）+ 一次 `UPDATE`，事务内 `SELECT` 看到 603 行且首行是
`(1,'Z')`（证明事务能读到自己的写入）；此时 `t.log` 45880 字节。SIGKILL 后重启：
只剩基线 3 行，`t.log` 被截回 210 字节。崩溃后继续写入、`BEGIN`/`COMMIT`、再重启，
结果 `基线 + [(5,'e'),(6,'f')]`，引擎未被损坏。

**C 恢复过程中反复被杀**（3 项，对应赛题「恢复中再次被杀」的要求）。先写入 20000 行
并干净关闭；再故意制造脏状态（`BEGIN` + 1000 行未提交 + SIGKILL，`t.log` 1627815 字节）；
然后连续 12 次启动进程并在握手前随机时刻（0/2/5/10/20/50 ms）杀掉，其中 **5 次确实死在
恢复过程中**（没等到 ready 行）。之后正常启动：20000 行完整，`t.log` 1548894 字节
（说明中断的恢复已经把未提交尾巴截掉了一部分，且后续恢复能接着做完，不重复也不破坏），
干净重启后仍然一致。

**D COMMIT 竞争，全有或全无**（1 项汇总，对应赛题「已请求但未应答的提交必须全有或全无」）。
8 次独立试验：每次新建数据目录，写基线 3 行，`BEGIN` + 300 行插入 + 一次 `UPDATE`，
然后发出 `COMMIT` 请求并在 0/0/1/2/5/10/20/50 ms 后**不读应答直接杀进程**。重启后分类：

```
trial  1 delay=0.0    -> rolled back      trial  5 delay=0.005  -> committed
trial  2 delay=0.0    -> rolled back      trial  6 delay=0.01   -> committed
trial  3 delay=0.001  -> rolled back      trial  7 delay=0.02   -> committed
trial  4 delay=0.002  -> committed        trial  8 delay=0.05   -> committed
```

5 次完整提交、3 次完整回滚、**0 次部分提交**。判定标准是逐行比对：要么等于
`[(1,'Z'),(2,'b'),(3,'c')] + 300 行`，要么等于 `[(1,'a'),(2,'b'),(3,'c')]`；
行数介于两者之间、或 `id=1` 的更新与插入不同步，都算失败。

### 7.3 规模与内存：20 万行 × 128 维

```
py -3 minivectordb/tools/scale_check.py --rows 200000 --queries 20 \
  --output protocol-results/scale-200k.json
```

| 指标 | 实测值 |
| --- | --- |
| 载入 200000 行（每批 200 行的多行 INSERT） | 20.202 s，9900 行/秒 |
| 载入期峰值 RSS | 23,801,856 B（22.7 MiB） |
| 日志文件大小 | 110,979,381 B（105.8 MiB） |
| 磁盘上每行 | 554.9 B |
| 常驻内存每行 | 119.0 B（≈ 磁盘的 1/4.7） |
| exact TopK（`ORDER BY d ASC LIMIT 10`）中位数 | 63 ms（20 次查询） |
| exact TopK 最大值 | 78 ms |
| 查询期峰值 RSS | 24,899,584 B（23.7 MiB） |
| 有界堆 == 全量排序（无过滤） | 一致，199000 行匹配 |
| 有界堆 == 全量排序（`WHERE tag='tag-7'`） | 一致，2041 行匹配 |
| 更新 1000 行 + 删除 1000 行 | 通过，标签校验正确 |
| 干净关闭耗时 | 0.016 s |
| 重启恢复耗时（199000 行进索引） | 0.234 s |
| 重启后峰值 RSS | 22,339,584 B（21.3 MiB） |
| 重启前后 TopK 结果 | 完全相同 |

关键结论：**105.8 MiB 的数据，进程峰值 RSS 23.7 MiB**。常驻内存与数据量脱钩，
超内存规模下靠顺序流式扫描工作，符合赛题 2.3 节的要求。

内存峰值出现在查询期而不是载入期（23.7 vs 22.7 MiB），因为有界堆会持有 k 行的解码结果；
k 是 `LIMIT` 值，与表大小无关。

### 7.4 Linux 工具链与容器复现（ubuntu-24.04，g++ 13）

7.1–7.3 全部在 Windows（MinGW g++ 8.1.0）裸进程上完成，而正式评测环境是 Linux x86_64 容器。
本机无 Docker/WSL，因此用一次 CI 作业把判题路径整体复现：临时 workflow 在 `ubuntu-24.04` runner 上依次执行

```
g++ -std=c++17 -O2 -Wall -Wextra -pthread minivectordb/src/*.cpp -o /tmp/mvdb-native
docker build -t vectordb-ci .
sudo python3 protocol_check/run.py --image vectordb-ci --output /tmp/ci-image
python3 protocol_check/run.py --command '["/tmp/mvdb-native","--data-dir","{data_dir}"]' --output protocol-results/ci-native
python3 minivectordb/tools/crash_check.py --exe /tmp/mvdb-native --output protocol-results/ci-crash
```

镜像阶段走的是官方 `protocol_check/_transport.py`，即 `--read-only`、`--cap-drop=ALL`、
`--network=none`、`--cpus=2`、`--memory=2g`、`--pids-limit=128`，与赛题 3.1 的资源口径一致。

| 阶段 | 结果 |
| --- | --- |
| g++ 13 裸编译（`-Wall -Wextra`） | exit 0，零警告 |
| `docker build .` | success |
| 官方 `protocol_check/run.py --image`（20 个公开用例） | exit 0 |
| 官方 `protocol_check/run.py --command`（裸二进制） | exit 0 |
| `crash_check.py`（12 项崩溃/恢复断言） | exit 0 |
| 整轮耗时 | 58 s（run 42965293402）/ 57 s（run 42965580664） |

复现记录：`https://github.com/WatingCL/seekdb/actions/runs/42965293402`、
`https://github.com/WatingCL/seekdb/actions/runs/42965580664`。

两条与移植性有关的实测结论：

1. 源码此前从未在 Linux 编译器上验证过。MinGW 8.1 的 libstdc++ 会传递包含常用 C 头，
   掩盖了两处真实缺失——`src/json.cpp` 使用 `snprintf` 但未包含 `<cstdio>`，
   `src/parser.cpp` 使用 `errno`/`ERANGE` 但未包含 `<cerrno>`；
   另外 `std::move` 所在的 `<utility>` 在 `engine.cpp`/`storage.cpp`/`parser.cpp` 中补齐。
   现已全部显式包含。
2. 提交树只有 105 个文件、0.4 MiB，但 `docker build .` 的构建上下文默认是整个仓库克隆
   （含 `.git`），因此新增根目录 `.dockerignore`，只保留 `minivectordb/`。
   上表第二行（57 s 那次）就是加了 `.dockerignore` 之后重跑的，结果不变。

## 8. 已实现功能与已知限制

### 8.1 已实现

- JSONL 协议：握手、`execute`、`shutdown`、`id` 原样回传、逐行 flush、stdout 纯净、stderr 日志。
- SQL：`CREATE TABLE`、`CREATE VECTOR INDEX`、`INSERT`（含列清单与多行）、`SELECT`
  （`*`、列、`L2_DISTANCE(...) AS d`、`WHERE` AND 链、`ORDER BY` 多键升降序、`LIMIT`）、
  `UPDATE`、`DELETE`、`BEGIN`/`COMMIT`/`ROLLBACK`。
- 类型：`BIGINT`、`VARCHAR(n)`（按 Unicode 字符数）、`VECTOR(n)`（float32）。
- 持久化：追加日志 + CRC32 + 全局提交点；干净关闭与 SIGKILL 都能正确恢复。
- 事务：单活动事务、语句级原子性、事务内读己写、回滚截断日志、关闭时回滚。
- 向量检索：精确扫描 + 有界 TopK 堆，召回率 1.0，过滤先于 TopK，距离并列按主键升序。
- 内存：只常驻主键索引，行数据全在磁盘；20 万行 × 128 维峰值 RSS 23.7 MiB。
- 错误处理：16 种错误码，不支持的语句一律报错而非静默忽略。

### 8.2 已知限制

1. **`ann` 模式等价于 `exact`**。没有实现 HNSW 或任何近似索引，`search_mode:"ann"`
   走同一条精确扫描路径。好处是召回率恒为 1.0、无索引维护开销；代价是查询延迟随行数
   线性增长（20 万行 63 ms）。若平台对 `ann` 有延迟要求，这是首要待补项。
2. **DDL 不参与事务回滚**。`BEGIN; CREATE TABLE ...; ROLLBACK;` 之后表仍然存在。
   catalog 走 `meta.json` 原子替换，没有纳入 xid 提交点体系。
3. **没有日志压缩**。更新和删除只追加新版本/墓碑，旧记录永远留在文件里，所以
   `<表名>.log` 会随写入量单调增长，全表扫描的字节数也随之增长。20 万行载入
   105.8 MiB 是纯净下界；反复更新同一批行会让文件显著变大。需要 compaction 才能回收。
4. **扫描是单线程的**。没有 SIMD（AVX2/FMA）、没有并行分段扫描。L2 距离是标量循环。
   这是性能口径下最大的未开采空间。
5. **每行解码会物化一个 `Value`**（含 `std::vector<float>` 与 `std::string`），
   即使命中堆之前就要分配。可以改成直接从记录 body 里读 float32 指针，省掉一次拷贝
   和每行两次堆分配。
6. **主键索引用 `std::map`**，119 字节/行里有相当一部分是红黑树节点开销。
   换成开放寻址哈希表可以降到 ~24 字节/行。
7. **提交要 2 次 fsync**（表日志 + `txn.log`）。崩溃模型只杀进程不断电时，
   进入 page cache 的 `write()` 就能活过 `kill -9`，所以 fsync 是保险而非必需；
   在写入密集的负载下可以考虑降级为 fflush，但这会牺牲断电语义。
8. **不支持** `ALTER`/`DROP`/`JOIN`/子查询/聚合/`OR`/`NULL`/`LIMIT offset`（按赛题范围，
   这些一律返回 `unsupported_sql`）。
9. **单进程单线程**，没有并发控制，也不支持多个进程同时打开同一个数据目录。
10. **平台环境仍未验证**。Linux 侧已在 `ubuntu-24.04` + g++ 13 上复现通过（见 7.4：裸编译、
    `docker build`、官方 `protocol_check` 镜像与裸进程两条路、崩溃套件全绿），
    但那台 runner 不是评测平台；隐藏用例、page cache 计费口径与平台的构建网络策略仍不可知。
    规模测试（7.3）目前只有 Windows 数据，Linux 侧待补。

## 9. 待办（按优先级）

1. 在 Linux 上补跑规模测试（7.3），并把 7.4 的复现挪到自己的 Linux 机器/云主机上常态化执行。
2. AVX2/FMA 的 L2 距离内核 + 直接从记录 body 读 float32，去掉每行的 `Value` 物化。
3. 并行分段扫描（赛题给 2 核）。
4. 日志 compaction，回收被更新/删除的记录。
5. 真正的 ANN 索引（增量 HNSW），仅当平台对 `ann` 模式的延迟有硬要求时才需要。
