"""Box-only check (design 2026-10-26, phase 5): a streamed prompt chunk's two expert paths agree. The MMQ path
(llama.cpp's MMQ on the slots and on the misses streamed into the ring, the BF16 layers on the grouped kernel from the
ring, the buffers lent by the expert cache) and the grouped kernel reading the misses over PCIe in place
(STRATA_G4_NO_MMQ=1) must compute the same model after the same prompt of at least 64 tokens (the MMQ path's floor).

Two gates:
- layer 0 (the first MMQ layer, before any routing can flip): every position's output within 1e-2 of the grouped
  path's, relative to that position's largest value. Measured: median 9e-5, worst 1.3e-3 (the hidden's int8
  re-quantization flips a value now and then); a wrong expert, row or stride is O(1) on the tokens it touches.
- the next-token distribution: KL below llama.cpp's own CPU-vs-CUDA floor on this model (0.042,
  docs/validation/gemma4/2026-10-22-engine-parity-and-speed.md; the phase's documented gate) and the same top-1.
  MMQ's stream-k split depends on the launch's shape, so its last bits - and, through the hidden's int8
  re-quantization and the routing, the end result - move with the slot split: measured KL 5e-4 to 1.2e-2.

Usage: python -I g4_prompt_paths.py ENGINE GGUF IDS_FILE   (IDS_FILE: comma-separated ids, 64 or more)
"""
import array
import math
import os
import subprocess
import sys
import tempfile

V = 262144
EMBD = 2816
LAYERS = 30


def floats(path):
    a = array.array("f")
    with open(path, "rb") as f:
        a.frombytes(f.read())
    return a


def run(engine, gguf, ids, grouped, logits_out, layers_out):
    env = dict(os.environ)
    env.pop("STRATA_G4_NO_MMQ", None)
    if grouped:
        env["STRATA_G4_NO_MMQ"] = "1"
    # chunks of 128: two streamed chunks (the expert cache rebalanced between them) and a 37-token tail on the
    # grouped kernel, the BF16 layers' groups sharing the ring with the Q8_0 ones
    cmd = [engine, "--native", gguf, "--max-context", "4096", "--chunk", "128", "--tokens", ids, "--n", "1",
           "--dump-full-logits", logits_out, "--dump-layers", layers_out]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        sys.exit("engine failed (%s): %s" % ("grouped" if grouped else "mmq", r.stderr[-2000:]))


def main():
    engine, gguf, ids_file = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(ids_file, encoding="utf-8") as f:
        ids = f.read().strip()
    n_pos = len(ids.split(","))
    if n_pos < 64:
        sys.exit("need 64 or more ids")
    with tempfile.TemporaryDirectory() as d:
        paths = [os.path.join(d, n) for n in ("g.f32", "g.layers", "m.f32", "m.layers")]
        run(engine, gguf, ids, True, paths[0], paths[1])
        run(engine, gguf, ids, False, paths[2], paths[3])
        la, ya, lb, yb = (floats(p) for p in paths)
    # layer 0 of every prompt position (the sink is [pos][layer][2816]), each relative to its own largest value
    rel0 = 0.0
    for p in range(n_pos):
        at = (p * LAYERS) * EMBD
        big = max(abs(ya[at + i]) for i in range(EMBD))
        worst = max(abs(ya[at + i] - yb[at + i]) for i in range(EMBD))
        rel0 = max(rel0, worst / big if big > 0 else worst)
    ma, mb = max(la[0:V]), max(lb[0:V])
    sa = math.log(sum(math.exp(x - ma) for x in la[0:V])) + ma
    sb = math.log(sum(math.exp(x - mb) for x in lb[0:V])) + mb
    kl = 0.0
    for i in range(V):
        pa = math.exp(la[i] - sa)
        kl += pa * ((la[i] - sa) - (lb[i] - sb))
    ta = max(range(V), key=lambda i: la[i])
    tb = max(range(V), key=lambda i: lb[i])
    print(f"layer 0: worst token's relative difference {rel0:.2e}; KL(grouped || MMQ) = {kl:.2e}; top-1 {ta} vs {tb}")
    return 0 if rel0 < 1e-2 and kl < 0.042 and ta == tb else 1


if __name__ == "__main__":
    sys.exit(main())
