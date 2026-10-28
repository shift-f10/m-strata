"""raon1 batched prefill vs the official llama.cpp graph (box-only: needs the model and an oracle dump).

The engine runs --tokens-file teacher-forced with STRATA_GLM_KL_DUMP=<file>,<n>: every token but the last n goes
through the batched prompt path (prefill), the last n one at a time through the decode, and the logits after each of
the n land in <file> (n x n_vocab F32).  The decode was checked against the official graph on its own (KL 0.0011
nats, top-1 100% over 180 positions), so what these n positions measure beyond it is the state the prompt path left:
every layer's KV / recurrent state and the hyper-connection streams of the prompt's positions.

The oracle is the official llama.cpp (KRAFTON raon-1-mini-preview) build's logits for the same token file: a small
program linked against its llama.lib (see the engine-parity-oracle note) writes the last N positions' logits as
N x n_vocab F32; this script reads its last n rows.

FAILS when the prompt did not go through the batched path ("token by token", or no "glm prefill: <k> tokens in"
line for the whole head), or when the logits drift from the oracle: mean KL(oracle || engine) above --max-mean-kl,
any position above --max-kl, or top-1 agreement below --min-top1.

Usage:
  python raon1_prefill_parity.py --exe multiengine.exe --pack <raon1 pack> --tokens seq.txt --oracle kl_oracle.bin
         --oracle-n 180 [--n 16] [--chunk 64] [--env K=V ...]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

import numpy as np


def logsoftmax(x):
    m = x.max(axis=1, keepdims=True)
    return x - m - np.log(np.exp(x - m).sum(axis=1, keepdims=True))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--pack", required=True)
    ap.add_argument("--tokens", required=True, help="comma/space separated token ids")
    ap.add_argument("--oracle", required=True, help="the official build's logits: oracle-n x n_vocab F32")
    ap.add_argument("--oracle-n", type=int, required=True, help="positions in the oracle dump (its last rows)")
    ap.add_argument("--n", type=int, default=16, help="positions decoded after the prompt (the rest is prefill)")
    ap.add_argument("--chunk", type=int, default=0, help="force the prompt chunk (STRATA_GLM_PREFILL_CHUNK)")
    ap.add_argument("--env", action="append", default=[], help="extra K=V for the engine")
    ap.add_argument("--max-context", type=int, default=0, help="the engine's --max-context (0: its default)")
    ap.add_argument("--kv", default="", help="the engine's --kv (raon1: f32 when unset, q8_0, q4_0)")
    ap.add_argument("--out", default="", help="keep the engine's logits here (default: a temporary file)")
    ap.add_argument("--max-mean-kl", type=float, default=0.005)
    ap.add_argument("--max-kl", type=float, default=0.05)
    ap.add_argument("--min-top1", type=float, default=0.9375)
    ap.add_argument("--timeout", type=int, default=900)
    a = ap.parse_args()

    toks = [int(x) for x in open(a.tokens, encoding="ascii").read().replace(",", " ").split()]
    n, head = a.n, len(toks) - a.n
    if a.oracle_n < n or head < 8:
        sys.exit("need oracle-n >= n and at least 8 prompt tokens")

    out = os.path.abspath(a.out) if a.out else os.path.join(tempfile.mkdtemp(prefix="raon1_pf_"), "kl.bin")
    env = dict(os.environ)
    env["STRATA_GLM_KL_DUMP"] = "%s,%d" % (out, n)
    env.pop("STRATA_GLM_SLOW", None)   # the fast path: the prompt path is the fast path's
    if a.chunk > 0:
        env["STRATA_GLM_PREFILL_CHUNK"] = str(a.chunk)
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v
    cmd = [os.path.abspath(a.exe), "--glm-pack", a.pack, "--tokens-file", os.path.abspath(a.tokens), "--max-new", "1"]
    if a.max_context > 0:
        cmd += ["--max-context", str(a.max_context)]
    if a.kv:
        cmd += ["--kv", a.kv]
    p = subprocess.run(cmd, env=env, capture_output=True, text=True, errors="replace", timeout=a.timeout)
    log = p.stderr
    pf = [l for l in log.splitlines() if l.startswith("glm prefill") or "latent cache" in l]
    print("\n".join(pf[-12:]))
    fails = []
    if p.returncode != 0:
        fails.append("engine exit %d: %s" % (p.returncode, log.strip().splitlines()[-1:]))
    if "token by token" in log:
        fails.append("the prompt path refused the model (token by token)")
    m = re.findall(r"glm prefill: (\d+) tokens in", log)
    if not m or int(m[-1]) != head:
        fails.append("no batched prompt of %d tokens in the log (saw %s)" % (head, m))
    if fails or not os.path.exists(out):
        print("FAIL:\n  " + "\n  ".join(fails or ["no logits written"]))
        return 1

    V = os.path.getsize(out) // 4 // n
    T = np.fromfile(out, dtype=np.float32).reshape(n, V).astype(np.float64)
    R = np.fromfile(a.oracle, dtype=np.float32).reshape(a.oracle_n, V)[a.oracle_n - n:].astype(np.float64)
    lr, lt = logsoftmax(R), logsoftmax(T)
    kl = (np.exp(lr) * (lr - lt)).sum(axis=1)
    top1 = (R.argmax(axis=1) == T.argmax(axis=1)).mean()
    nll_r = np.array([-lr[i, toks[head + i + 1]] for i in range(n - 1)])
    nll_t = np.array([-lt[i, toks[head + i + 1]] for i in range(n - 1)])
    print("prefill %d tokens, %d decoded: top1 %.1f%%  meanKL %.5f  maxKL %.5f  NLL oracle %.3f engine %.3f"
          % (head, n, 100 * top1, kl.mean(), kl.max(), nll_r.mean(), nll_t.mean()))
    if not np.isfinite(T).all():
        fails.append("non-finite logits")
    if kl.mean() > a.max_mean_kl:
        fails.append("mean KL %.5f > %.5f" % (kl.mean(), a.max_mean_kl))
    if kl.max() > a.max_kl:
        fails.append("max KL %.5f > %.5f (position %d)" % (kl.max(), a.max_kl, head + int(kl.argmax())))
    if top1 < a.min_top1:
        fails.append("top-1 %.4f < %.4f" % (top1, a.min_top1))
    if fails:
        print("FAIL:\n  " + "\n  ".join(fails))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
