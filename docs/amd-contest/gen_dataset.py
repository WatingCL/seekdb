#!/usr/bin/env python3
"""Dataset generator for the seekdb x AMD hipVS contest benchmark.

Produces everything the bench harness needs for one dataset:

  base.tsv       250K rows "id<TAB>[v0,v1,...]" for LOAD DATA (untimed phase)
  queries.sql    4096 evaluation statements, statement #1 is the timed
                 first query (same SQL shape as the contest evaluation)
  gt10.i32       ground-truth top-10 ids, int32 row-major [nq][10]
  gt100.i32      ground-truth top-100 ids, int32 row-major [nq][100]
  base.f32       raw base vectors, float32 [n][dim]
  queries.f32    raw query vectors, float32 [nq][dim]
  meta.json      n, dim, nq, seed, generator, created_utc

Datasets
--------
sift           SIFT250K proxy: first 250K rows of SIFT1M base (128-d) and the
               first 4096 SIFT queries. Downloads the public texmex corpus
               (~130MB) once into --cache. Exact ground truth is recomputed
               with brute force restricted to the 250K subset.
hidden-proxy   Hidden250K proxy: 256-d clustered Gaussian mixture designed to
               mimic real embedding data (many tight clusters, heavy-tailed
               cluster sizes). The contest Hidden250K is 250K x 256.

Ground truth is always computed exactly with numpy in query chunks, so recall
numbers are trustworthy without relying on any vendor ground-truth file.
"""
import argparse
import datetime
import json
import pathlib
import sys
import urllib.request

N_BASE = 250_000
N_QUERY = 4_096
SEED = 20250919


def log(msg):
    print(f"[gen_dataset] {msg}", file=sys.stderr, flush=True)


def fvecs_read(path):
    import numpy as np
    raw = np.fromfile(path, dtype=np.uint8)
    dim = int(np.frombuffer(raw[:4].tobytes(), dtype="<i4")[0])
    rows = raw.size // (4 + 4 * dim)
    out = np.empty((rows, dim), dtype=np.float32)
    for i in range(rows):
        off = i * (4 + 4 * dim) + 4
        out[i] = np.frombuffer(raw[off:off + 4 * dim].tobytes(), dtype="<f4")
    return out


def download(url, dest):
    if dest.exists() and dest.stat().st_size > 0:
        log(f"reuse cached {dest.name}")
        return
    log(f"downloading {url}")
    tmp = dest.with_suffix(dest.suffix + ".part")
    urllib.request.urlretrieve(url, tmp)
    tmp.rename(dest)


def load_sift(cache):
    import numpy as np
    cache.mkdir(parents=True, exist_ok=True)
    tar = cache / "sift.tar.gz"
    download("ftp://ftp.irisa.fr/local/texmex/corpus/sift.tar.gz", tar)
    import tarfile
    with tarfile.open(tar) as tf:
        members = {m.name.rsplit("/", 1)[-1]: m for m in tf.getmembers()}
        tf.extract(members["sift_base.fvecs"], path=cache)
        tf.extract(members["sift_query.fvecs"], path=cache)
    base = fvecs_read(cache / "sift_base.fvecs")[:N_BASE]
    queries = fvecs_read(cache / "sift_query.fvecs")[:N_QUERY]
    return np.ascontiguousarray(base), np.ascontiguousarray(queries)


def gen_hidden_proxy():
    """256-d clustered Gaussian: tight clusters with heavy-tailed sizes."""
    import numpy as np
    rng = np.random.default_rng(SEED)
    dim = 256
    n_clusters = 2048
    # cluster sizes: heavy tail (Zipf-ish), summing to N_BASE
    weights = 1.0 / np.arange(1, n_clusters + 1) ** 0.8
    sizes = np.floor(weights / weights.sum() * N_BASE).astype(np.int64)
    while sizes.sum() > N_BASE:
        sizes[int(np.argmax(sizes))] -= 1
    while sizes.sum() < N_BASE:
        sizes[int(np.argmin(sizes[sizes > 0]))] += 1
    centers = rng.normal(scale=3.0, size=(n_clusters, dim)).astype(np.float32)
    base = np.empty((N_BASE, dim), dtype=np.float32)
    pos = 0
    for c, sz in enumerate(sizes):
        if sz == 0:
            continue
        base[pos:pos + sz] = centers[c] + rng.normal(scale=1.0, size=(sz, dim)).astype(np.float32)
        pos += sz
    assert pos == N_BASE
    # queries: fresh draws near random existing points (harder than pure
    # center draws, avoids trivially separable queries)
    pick = rng.choice(N_BASE, size=N_QUERY, replace=False)
    queries = (base[pick] + rng.normal(scale=0.05, size=(N_QUERY, dim)).astype(np.float32)).astype(np.float32)
    return base, queries


def exact_gt(base, queries, k, chunk=128):
    """Exact brute-force top-k (by squared L2), chunked over queries.

    Uses the dot-product identity d = |q|^2 + |b|^2 - 2 q.b so the per-chunk
    working set is O(chunk * n) instead of O(chunk * n * dim). float32 BLAS
    accumulation is exact enough for top-k ordering on continuous data.
    """
    import numpy as np
    base = np.ascontiguousarray(base, dtype=np.float32)
    queries = np.ascontiguousarray(queries, dtype=np.float32)
    bn = (base * base).sum(axis=1)  # [n]
    nq = queries.shape[0]
    gt = np.empty((nq, k), dtype=np.int32)
    for s in range(0, nq, chunk):
        q = queries[s:s + chunk]
        d = (q * q).sum(axis=1)[:, None] + bn[None, :] - 2.0 * (q @ base.T)  # [c, n]
        part = np.argpartition(d, k - 1, axis=1)[:, :k]
        rows = np.arange(q.shape[0])[:, None]
        order = np.argsort(d[rows, part], axis=1)
        gt[s:s + chunk] = part[rows, order]
    return gt


def vec_str(v):
    return "[" + ",".join(f"{x:.7f}" for x in v) + "]"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", choices=["sift", "hidden-proxy"], required=True)
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--cache", default=".cache/datasets", help="download cache")
    ap.add_argument("--n-query", type=int, default=N_QUERY)
    args = ap.parse_args()

    try:
        import numpy as np
    except ImportError:
        raise SystemExit("numpy is required (pip install numpy)")

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    if args.dataset == "sift":
        base, queries = load_sift(pathlib.Path(args.cache))
    else:
        base, queries = gen_hidden_proxy()
    n, dim = base.shape
    nq = min(args.n_query, queries.shape[0])
    queries = np.ascontiguousarray(queries[:nq])
    log(f"base={n}x{dim} queries={nq}x{dim}")

    log("computing exact ground truth top-100 (chunked brute force)")
    gt100 = exact_gt(base, queries, 100)
    gt10 = np.ascontiguousarray(gt100[:, :10])

    log("writing base.tsv (LOAD DATA input; this is the big one)")
    with (out / "base.tsv").open("w") as f:
        for i in range(n):
            f.write(f"{i}\t{vec_str(base[i])}\n")

    log("writing queries.sql (statement #1 is the timed first query)")
    with (out / "queries.sql").open("w") as f:
        f.write("use bench;\nset ob_query_timeout=600000000;\n")
        for i in range(nq):
            f.write(f"select id from items order by l2_distance(v,'{vec_str(queries[i])}') approximate limit 10;\n")
    with (out / "queries100.sql").open("w") as f:
        f.write("use bench;\nset ob_query_timeout=600000000;\n")
        for i in range(nq):
            f.write(f"select id from items order by l2_distance(v,'{vec_str(queries[i])}') approximate limit 100;\n")

    log("writing raw arrays and ground truth")
    base.tofile(out / "base.f32")
    queries.tofile(out / "queries.f32")
    gt10.tofile(out / "gt10.i32")
    gt100.tofile(out / "gt100.i32")
    (out / "probe0.txt").write_text(vec_str(queries[0]))

    meta = {
        "dataset": args.dataset,
        "n": int(n),
        "dim": int(dim),
        "nq": int(nq),
        "seed": SEED,
        "generator": "gen_dataset.py",
        "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    }
    (out / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    log(f"done -> {out}")


if __name__ == "__main__":
    main()
