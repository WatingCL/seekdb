"""gen_spec.py <out_dir> <name> <tau>

250K x 256 float32 vectors whose coordinate std is exp(-i/tau) in a random
rotation, plus 4096 queries and exact top-100 ground truth (same file layout as
gen_data.py). tau=250 gives a set whose baseline Recall@100 matches the
contest's hidden 256-d data; tau=120 is a medium-hard variant."""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_data import exact_topk  # noqa: E402

out, name, tau = sys.argv[1], sys.argv[2], float(sys.argv[3])
n, nq, dim = 250000, 4096, 256
rng = np.random.default_rng(11)
std = np.exp(-np.arange(dim) / tau).astype(np.float32)
rot, _ = np.linalg.qr(rng.standard_normal((dim, dim)))
rot = rot.astype(np.float32)


def make(m):
    return ((rng.standard_normal((m, dim)).astype(np.float32) * std) @ rot).astype(np.float32)


base, query = make(n), make(nq)
d = os.path.join(out, name)
os.makedirs(d, exist_ok=True)
base.tofile(f"{d}/base.f32")
query.tofile(f"{d}/query.f32")
exact_topk(base, query, 100).tofile(f"{d}/gt100.i64")
with open(f"{d}/base.tsv", "w") as f:
    for i, row in enumerate(base, 1):
        f.write(f"{i}\t[{','.join(f'{v:.6g}' for v in row)}]\n")
with open(f"{d}/meta.txt", "w") as f:
    f.write(f"n={n} dim={dim} nq={nq}\n")
print(name, base.shape)
