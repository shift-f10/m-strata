"""Teacher-forced logits against a reference: mean / max KL, top-1 agreement and the forced tokens' NLL, per file.
Usage: python -I flint_kl.py <ids.txt> <n_rows> <reference.bin> <other.bin> [<other.bin> ...]
The rows are the last n_rows positions of ids; row r predicts ids[len - n_rows + r + 1] (the last row predicts nothing)."""
import sys

import numpy as np

ids = [int(x) for x in open(sys.argv[1]).read().replace(",", " ").split()]
n, V = int(sys.argv[2]), 150144
ref = np.fromfile(sys.argv[3], dtype=np.float32).reshape(n, V)


def logp(x):
    x = x.astype(np.float64)
    m = x.max(axis=1, keepdims=True)
    return x - m - np.log(np.exp(x - m).sum(axis=1, keepdims=True))


lr = logp(ref)
nxt = ids[len(ids) - n + 1:]
nll_ref = -np.mean([lr[r, nxt[r]] for r in range(n - 1)])
print(f"reference {sys.argv[3].split(chr(92))[-1]}: NLL of the forced tokens {nll_ref:.4f}")
for path in sys.argv[4:]:
    lo = logp(np.fromfile(path, dtype=np.float32).reshape(n, V))
    kl = np.sum(np.exp(lr) * (lr - lo), axis=1)
    top = np.mean(np.argmax(lr, axis=1) == np.argmax(lo, axis=1))
    nll = -np.mean([lo[r, nxt[r]] for r in range(n - 1)])
    q = np.quantile(kl, [0.5, 0.9, 0.99])
    print(f"{path.split(chr(92))[-1]}: mean KL {kl.mean():.5f} (median {q[0]:.5f}, p90 {q[1]:.5f}, p99 {q[2]:.5f}, max {kl.max():.4f} "
          f"at row {int(kl.argmax())}), top-1 {100 * top:.1f}%, NLL {nll:.4f}")
