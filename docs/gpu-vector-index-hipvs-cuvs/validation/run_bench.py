"""Local imitation of the AMD contest flow (build timing + nq=4096 queries + recall).

Usage:
  run_bench.py --port 3891 --data ~/oc/amd/data/sift250k --load        # (re)create table and LOAD DATA
  run_bench.py --port 3891 --data ~/oc/amd/data/sift250k --rounds 1    # timed build + queries
"""
import argparse
import json
import os
import statistics
import threading
import time

import numpy as np
import pymysql

INDEX_DDL = ("CREATE VECTOR INDEX idx_items ON items(v) WITH (distance=l2, type=hnsw, "
             "lib={lib}, m=16, ef_construction=200, ef_search=64)")


def connect(port, db=None):
    return pymysql.connect(host="127.0.0.1", port=port, user="root", password="", database=db,
                           local_infile=True, autocommit=True, read_timeout=3600, write_timeout=3600)


def sql_exec(conn, sql):
    with conn.cursor() as cur:
        cur.execute(sql)
        return cur.fetchall()


def vec_literal(v):
    return "[" + ",".join(f"{x:.6g}" for x in v) + "]"


def query_sql(v, k):
    return f"SELECT id FROM items ORDER BY l2_distance(v, '{vec_literal(v)}') APPROXIMATE LIMIT {k}"


def load(args, meta):
    c = connect(args.port)
    sql_exec(c, "SET GLOBAL ob_query_timeout=36000000000")
    sql_exec(c, f"CREATE DATABASE IF NOT EXISTS {args.db}")
    sql_exec(c, f"USE {args.db}")
    sql_exec(c, "DROP TABLE IF EXISTS items")
    sql_exec(c, f"CREATE TABLE items(id BIGINT PRIMARY KEY, v VECTOR({meta['dim']}))")
    t = time.time()
    sql_exec(c, f"LOAD DATA LOCAL INFILE '{os.path.join(args.data, 'base.tsv')}' INTO TABLE items "
                "FIELDS TERMINATED BY '\\t' LINES TERMINATED BY '\\n' (id, v)")
    print(f"load {time.time() - t:.1f}s rows={sql_exec(c, 'SELECT COUNT(*) FROM items')[0][0]}")
    c.close()


def run_queries(args, queries, k, nthreads):
    res = [None] * len(queries)
    lat = [0.0] * len(queries)
    nxt = [0]
    lock = threading.Lock()

    def worker():
        c = connect(args.port, args.db)
        sql_exec(c, "SET ob_query_timeout=36000000000")
        while True:
            with lock:
                i = nxt[0]
                nxt[0] += 1
            if i >= len(queries):
                break
            t = time.perf_counter()
            rows = sql_exec(c, query_sql(queries[i], k))
            lat[i] = time.perf_counter() - t
            res[i] = [r[0] for r in rows]
        c.close()

    t0 = time.perf_counter()
    ths = [threading.Thread(target=worker) for _ in range(nthreads)]
    for th in ths:
        th.start()
    for th in ths:
        th.join()
    wall = time.perf_counter() - t0
    return res, lat, wall


def recall(res, gt, k):
    hit, bad = 0, 0
    for i, r in enumerate(res):
        if len(r) != k or len(set(r)) != k:
            bad += 1
        hit += len(set(r[:k]) & set(gt[i, :k].tolist()))
    return hit / (k * len(res)), bad


def one_round(args, meta, queries, gt):
    c = connect(args.port, args.db)
    sql_exec(c, "SET ob_query_timeout=36000000000")
    if not args.reuse:
        try:
            sql_exec(c, "DROP INDEX idx_items ON items")
        except pymysql.err.MySQLError:
            pass
    probe = queries[0]
    t0 = time.perf_counter()
    if not args.reuse:
        sql_exec(c, INDEX_DDL.format(lib=args.lib))
    t1 = time.perf_counter()
    first = [r[0] for r in sql_exec(c, query_sql(probe, 10))]
    t2 = time.perf_counter()
    out = {"ddl_s": t1 - t0, "first_query_s": t2 - t1, "build_s": t2 - t0,
           "first_ok": len(first) == 10 and len(set(first)) == 10}
    print(f"build {out['build_s']:.2f}s (ddl {out['ddl_s']:.2f}s + first query {out['first_query_s']:.3f}s)", flush=True)
    if not args.skip_queries:
        for k in (10, 100):
            res, lat, wall = run_queries(args, queries[:args.nq], k, args.threads)
            r, bad = recall(res, gt, k)
            lat_sorted = sorted(lat)
            out[f"top{k}"] = {"qps": len(res) / wall, "p95_ms": lat_sorted[int(0.95 * len(lat)) - 1] * 1000,
                              "recall": r, "bad_rows": bad}
            print(f"top{k}: qps={out[f'top{k}']['qps']:.1f} p95={out[f'top{k}']['p95_ms']:.2f}ms "
                  f"recall={r:.4f} bad={bad}", flush=True)
    c.close()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=3891)
    ap.add_argument("--db", default="amdbench")
    ap.add_argument("--data", required=True)
    ap.add_argument("--lib", default="cuvs")
    ap.add_argument("--load", action="store_true")
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--nq", type=int, default=4096)
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--skip-queries", action="store_true")
    ap.add_argument("--reuse", action="store_true", help="query the existing index (no DROP/CREATE)")
    ap.add_argument("--out")
    args = ap.parse_args()
    args.data = os.path.expanduser(args.data)
    meta = dict(kv.split("=") for kv in open(os.path.join(args.data, "meta.txt")).read().split())
    meta = {k: int(v) for k, v in meta.items()}
    if args.load:
        load(args, meta)
    if args.rounds <= 0:
        return
    queries = np.fromfile(os.path.join(args.data, "query.f32"), dtype=np.float32).reshape(-1, meta["dim"])
    gt = np.fromfile(os.path.join(args.data, "gt100.i64"), dtype=np.int64).reshape(-1, 100)
    rounds = [one_round(args, meta, queries, gt) for _ in range(args.rounds)]
    summary = {"data": args.data, "lib": args.lib, "rounds": rounds,
               "build_median_s": statistics.median(r["build_s"] for r in rounds)}
    print(json.dumps({k: v for k, v in summary.items() if k != "rounds"}))
    if args.out:
        with open(args.out, "w") as f:
            json.dump(summary, f, indent=1)


if __name__ == "__main__":
    main()
