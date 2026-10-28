"""flint's seams (STRATA_FLINT_SEAMS) against the oracle's (flint_seams.exe), layer by layer in graph order; prints each
seam's worst position (cosine, relative error) and the first seam under the threshold.
Usage: python -I flint_seams_cmp.py <ours_dir> <oracle_dir> [threshold=0.999] [layers=0-48]"""
import os
import sys

import numpy as np

ours_dir, or_dir = sys.argv[1], sys.argv[2]
thr = float(sys.argv[3]) if len(sys.argv) > 3 else 0.999
lo, hi = (int(x) for x in (sys.argv[4] if len(sys.argv) > 4 else "0-48").split("-"))
ORDER = ["attn_norm", "kqv_out", "ffn_inp", "ffn_norm", "ffn_moe_logits", "ffn_moe_out", "ffn_shexp", "ffn_out", "l_out"]


def load(d, name):
    p = os.path.join(d, name + ".bin")
    if not os.path.exists(p):
        return None
    raw = np.fromfile(p, dtype=np.uint8)
    ne = np.frombuffer(raw[:32].tobytes(), dtype=np.int64)
    return np.frombuffer(raw[32:].tobytes(), dtype=np.float32).reshape(int(ne[1]) * int(ne[2]) * int(ne[3]), int(ne[0]))


first = None
for il in range(lo, hi + 1):
    for s in ORDER:
        a, b = load(ours_dir, f"{s}-{il}"), load(or_dir, f"{s}-{il}")
        if a is None or b is None:
            continue
        rows = min(a.shape[0], b.shape[0])
        a, b = a[a.shape[0] - rows:], b[b.shape[0] - rows:]
        cs = [float(np.dot(a[r], b[r]) / (np.linalg.norm(a[r]) * np.linalg.norm(b[r]) + 1e-30)) for r in range(rows)]
        rel = [float(np.linalg.norm(a[r] - b[r]) / (np.linalg.norm(b[r]) + 1e-30)) for r in range(rows)]
        w = int(np.argmin(cs))
        bad = [r for r in range(rows) if cs[r] < thr]
        flag = "  <-- " + f"{len(bad)} positions under {thr}: {bad[:8]}" if bad else ""
        print(f"{s}-{il:<2d} worst pos {w:2d}: cos {cs[w]:.6f} rel {rel[w]:.4f}; max rel {max(rel):.4f}{flag}")
        if bad and first is None:
            first = (il, s, bad)
if first:
    print(f"first seam under {thr}: {first[1]}-{first[0]} at positions {first[2][:12]}")
