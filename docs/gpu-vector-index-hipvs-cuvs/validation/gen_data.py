"""Generate a local stand-in for the contest datasets plus exact ground truth.

Usage: gen_data.py <out_dir> <name> <n> <dim> <nq> [--sift path/to/sift_base.fvecs path/to/sift_query.fvecs]

Writes:
  base.tsv      id<TAB>[f,f,...]   (LOAD DATA input, ids 1..n)
  base.f32      raw float32 n x dim
  query.f32     raw float32 nq x dim
  gt100.i64     exact top-100 ids (1-based, matching base.tsv ids) nq x 100
"""
import os
import sys

import numpy as np


def read_fvecs(path, limit):
    raw = np.fromfile(path, dtype=np.int32)
    dim = raw[0]
    rows = raw.reshape(-1, dim + 1)[:limit, 1:]
    return rows.copy().view(np.float32), int(dim)


def clustered(n, dim, rng, n_clusters=1024):
    """Anisotropic Gaussian mixture: closer to real embeddings than iid noise."""
    centers = rng.standard_normal((n_clusters, dim)).astype(np.float32) * 4.0
    scales = rng.uniform(0.3, 1.5, size=(n_clusters, 1)).astype(np.float32)
    lowrank = rng.standard_normal((dim // 4, dim)).astype(np.float32) / np.sqrt(dim)
    assign = rng.integers(0, n_clusters, size=n)
    x = np.empty((n, dim), dtype=np.float32)
    step = 65536
    for s in range(0, n, step):
        e = min(n, s + step)
        z = rng.standard_normal((e - s, dim // 4)).astype(np.float32) @ lowrank * 3.0
        z += rng.standard_normal((e - s, dim)).astype(np.float32) * 0.5
        x[s:e] = centers[assign[s:e]] + z * scales[assign[s:e]]
    return x


def exact_topk(base, query, k):
    bn = (base * base).sum(1)
    out = np.empty((len(query), k), dtype=np.int64)
    for s in range(0, len(query), 256):
        q = query[s:s + 256]
        d = bn[None, :] - 2.0 * (q @ base.T)
        idx = np.argpartition(d, k, axis=1)[:, :k]
        part = np.take_along_axis(d, idx, 1)
        order = np.argsort(part, axis=1, kind="stable")
        out[s:s + 256] = np.take_along_axis(idx, order, 1)
    return out + 1  # ids are 1-based


def main():
    out_dir, name, n, dim, nq = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
    rng = np.random.default_rng(20261028)
    if "--sift" in sys.argv:
        i = sys.argv.index("--sift")
        base, dim = read_fvecs(sys.argv[i + 1], n)
        query, _ = read_fvecs(sys.argv[i + 2], nq)
    elif "--raw" in sys.argv:
        i = sys.argv.index("--raw")
        base = np.fromfile(sys.argv[i + 1], dtype=np.float32).reshape(-1, dim)[:n]
        query = np.fromfile(sys.argv[i + 2], dtype=np.float32).reshape(-1, dim)[:nq]
    else:
        allx = clustered(n + nq, dim, rng)
        perm = rng.permutation(n + nq)
        base, query = allx[perm[:n]], allx[perm[n:]]
    d = os.path.join(out_dir, name)
    os.makedirs(d, exist_ok=True)
    base.astype(np.float32).tofile(os.path.join(d, "base.f32"))
    query.astype(np.float32).tofile(os.path.join(d, "query.f32"))
    with open(os.path.join(d, "base.tsv"), "w") as f:
        for i, row in enumerate(base, 1):
            f.write(f"{i}\t[{','.join(f'{v:.6g}' for v in row)}]\n")
    gt = exact_topk(base.astype(np.float32), query.astype(np.float32), 100)
    gt.tofile(os.path.join(d, "gt100.i64"))
    with open(os.path.join(d, "meta.txt"), "w") as f:
        f.write(f"n={len(base)} dim={dim} nq={len(query)}\n")
    print(name, base.shape, query.shape, "gt", gt.shape)


if __name__ == "__main__":
    main()
