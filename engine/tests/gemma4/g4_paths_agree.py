"""Box-only check (plans 2-3 Task 6): the engine's two forward paths agree. The prompt path
(dequantise + cuBLAS GEMMs + cuBLAS attention, chunks of more than 4 tokens) and the decode path
(matvec kernels + split-K attention, one token at a time) must give the same next-token
distribution after the same tokens.

Usage: python -I g4_paths_agree.py ENGINE GGUF IDS   (IDS: comma-separated, more than 8 ids)
Exit 0 when KL < 1e-3 and the top-1 ids agree.
"""
import array
import math
import os
import subprocess
import sys
import tempfile

V = 262144


def logits(path):
    a = array.array("f")
    with open(path, "rb") as f:
        a.frombytes(f.read())
    return a


def run(engine, gguf, tokens, force, out):
    cmd = [engine, "--native", gguf, "--max-context", "4096", "--tokens", tokens, "--dump-full-logits", out]
    if force:
        cmd += ["--force", force]
    else:
        cmd += ["--n", "1"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("engine failed: " + r.stderr[-2000:])


def main():
    engine, gguf, ids = sys.argv[1], sys.argv[2], sys.argv[3].split(",")
    if len(ids) <= 8:
        sys.exit("need more than 8 ids")
    with tempfile.TemporaryDirectory() as d:
        a, b = os.path.join(d, "batch.f32"), os.path.join(d, "decode.f32")
        run(engine, gguf, ",".join(ids), "", a)                     # one prompt chunk
        run(engine, gguf, ",".join(ids[:4]), ",".join(ids[4:]), b)  # four decode steps, then one per id
        la, lb = logits(a), logits(b)
    base_b = (len(lb) // V - 1) * V
    ma, mb = max(la[0:V]), max(lb[base_b:base_b + V])
    sa = math.log(sum(math.exp(x - ma) for x in la[0:V])) + ma
    sb = math.log(sum(math.exp(x - mb) for x in lb[base_b:base_b + V])) + mb
    kl = 0.0
    for i in range(V):
        pa = math.exp(la[i] - sa)
        kl += pa * ((la[i] - sa) - (lb[base_b + i] - sb))
    ta = max(range(V), key=lambda i: la[i])
    tb = max(range(V), key=lambda i: lb[base_b + i])
    print(f"KL(prompt path || decode path) = {kl:.2e}; top-1 {ta} vs {tb}")
    return 0 if kl < 1e-3 and ta == tb else 1


if __name__ == "__main__":
    sys.exit(main())
