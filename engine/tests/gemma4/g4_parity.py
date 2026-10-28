"""Gemma 4 parity report (plans 2-3 Task 8; spec §9.3): strata-gemma4-engine against llama.cpp.

Compares two dumps of the same token ids (stdlib only, no numpy):
  --layers A B   [pos][30][2816] f32 (strata-gemma4-engine --dump-layers, g4_llama_dump --dump-layers)
  --logits A B   [step][262144] f32 (--dump-full-logits of both)
A is ours, B the reference. Prints per-layer relative error (worst position) and per-step KL(B||A),
top-1 agreement and top-5 overlap.

Usage: python -I g4_parity.py --layers ours.layers lc.layers --logits ours.logits lc.logits [--positions N]
"""
import argparse
import array
import math
import os
import sys

N_EMBD = 2816
N_LAYER = 30
N_VOCAB = 262144


def load(path):
    a = array.array("f")
    with open(path, "rb") as f:
        a.frombytes(f.read())
    if sys.byteorder != "little":
        a.byteswap()
    return a


def layer_report(ours, ref, positions):
    row = N_LAYER * N_EMBD
    n = min(len(ours), len(ref)) // row
    if positions:
        n = min(n, positions)
    print(f"layers: {n} positions compared")
    worst_all = 0.0
    for layer in range(N_LAYER):
        worst, worst_pos = 0.0, -1
        errs = []
        for p in range(n):
            base = p * row + layer * N_EMBD
            num = den = 0.0
            for i in range(base, base + N_EMBD):
                d = ours[i] - ref[i]
                num += d * d
                den += ref[i] * ref[i]
            if den == 0.0:
                continue  # a row llama.cpp did not keep (the last layer keeps only output rows)
            r = math.sqrt(num / den)
            errs.append(r)
            if r > worst:
                worst, worst_pos = r, p
        worst_all = max(worst_all, worst)
        errs.sort()
        med = errs[len(errs) // 2] if errs else 0.0
        print(f"  layer {layer:2d}: median relative error {med:.5f}, worst {worst:.5f} (position {worst_pos})")
    return worst_all


def softmax_log(x, base):
    m = max(x[base:base + N_VOCAB])
    s = 0.0
    for i in range(base, base + N_VOCAB):
        s += math.exp(x[i] - m)
    return m + math.log(s)


def top(x, base, k):
    seg = x[base:base + N_VOCAB]
    return sorted(range(N_VOCAB), key=lambda i: -seg[i])[:k]


def logit_report(ours, ref):
    n = min(len(ours), len(ref)) // N_VOCAB
    print(f"logits: {n} steps compared")
    kls, agree, overlap = [], 0, 0
    for s in range(n):
        base = s * N_VOCAB
        lo, lr = softmax_log(ours, base), softmax_log(ref, base)
        kl = 0.0
        for i in range(base, base + N_VOCAB):
            pr = math.exp(ref[i] - lr)
            if pr > 0.0:
                kl += pr * ((ref[i] - lr) - (ours[i] - lo))
        t_o, t_r = top(ours, base, 5), top(ref, base, 5)
        agree += 1 if t_o[0] == t_r[0] else 0
        overlap += len(set(t_o) & set(t_r))
        kls.append(kl)
        print(f"  step {s:3d}: KL {kl:.6f}  top1 ours {t_o[0]} ref {t_r[0]}  top5 overlap {len(set(t_o) & set(t_r))}")
    print(f"  mean KL {sum(kls) / max(1, n):.6f}, max KL {max(kls) if kls else 0:.6f}, top-1 agreement {agree}/{n}, "
          f"top-5 overlap {overlap}/{5 * n}")
    return kls


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--layers", nargs=2)
    ap.add_argument("--logits", nargs=2)
    ap.add_argument("--positions", type=int, default=0)
    args = ap.parse_args()
    if args.layers:
        layer_report(load(args.layers[0]), load(args.layers[1]), args.positions)
    if args.logits:
        logit_report(load(args.logits[0]), load(args.logits[1]))


if __name__ == "__main__":
    main()
