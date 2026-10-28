"""Per-box calibration harness for the unified engine (spec 2026-10-24, Stage C1).

Mirrors upstream tools/calibrate.py's model: an EXTERNAL driver that restarts the engine per candidate
(so each measurement is cache-fair) and writes the winning adaptive-tier profile to the per-box store
the engine reads on start. It tunes the only subsystem the engine has today -- the adaptive expert tier
(residency headroom, rebalance cadence, warm-start seed) -- via the STRATA_CAL_* env overrides, keyed by
the engine's own hardware_key (scraped from its `calibration: key=...` log line).

Usage:
  python calibrate.py --engine <strata-unified-engine.exe> --model <gguf> [--gpu 0] [--max-context 4096]
                      [--max-new 96] [--gens 4]
  python calibrate.py ... --write "headroom_mib=768;rebalance_every=16;warm_seed=4"   # skip the sweep, just store

The measurement needs a generate-capable family (GLM, gemma4 today; qwen4exp once its decode loop lands).
Coordinate-descent sweep: headroom, then rebalance_every, then warm_seed, each from the engine default.
A candidate wins only if its median decode tok/s beats the running best by > MIN_GAIN.
"""
import argparse
import os
import re
import statistics
import subprocess
import sys
import threading
import time

MIN_GAIN = 0.03
DEFAULT_PROFILE = {"headroom_mib": 1024, "rebalance_every": 8, "rebalance_margin": 1.0, "warm_seed": 4.0}
SWEEP = {  # coordinate-descent candidates per knob (the default is always included first)
    "headroom_mib": [1024, 768, 1536],
    "rebalance_every": [8, 16, 4],
    "warm_seed": [4.0, 0.0, 8.0],
}
ENV_OF = {
    "headroom_mib": "STRATA_CAL_HEADROOM_MIB",
    "rebalance_every": "STRATA_CAL_REBALANCE_EVERY",
    "rebalance_margin": "STRATA_CAL_REBALANCE_MARGIN",
    "warm_seed": "STRATA_CAL_WARM_SEED",
}
RE_KEY = re.compile(r"calibration: key=(\S+)")
RE_DONE = re.compile(r"^DONE\s+(\d+)\s+\d+\s+[\d.]+\s+([\d.]+)")  # produced, decode_ms


def store_path():
    p = os.environ.get("STRATA_CAL_FILE")
    if p:
        return p
    home = os.environ.get("USERPROFILE") or os.environ.get("HOME") or "."
    return os.path.join(home, ".strata", "unified-calibration.cal")


def env_for(profile):
    env = dict(os.environ)
    for k, v in profile.items():
        if k in ENV_OF:
            env[ENV_OF[k]] = str(v)
    return env


def run_engine(args, profile, prompt_ids, n_gens):
    """One fresh engine process at `profile`. Returns (hardware_key, [decode tok/s per measured gen]).
    gen1 is a throwaway warm-up (discarded) so the measured gens see warm caches (cache-fair)."""
    cmd = [args.engine, "--serve", "--native", args.model, "--gpu", str(args.gpu),
           "--max-context", str(args.max_context)]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, bufsize=1, env=env_for(profile))
    key = [None]
    errlines = []

    def drain_err():
        for l in p.stderr:
            errlines.append(l.rstrip())
            m = RE_KEY.search(l)
            if m:
                key[0] = m.group(1)
    threading.Thread(target=drain_err, daemon=True).start()
    for line in p.stdout:
        if line.startswith("READY"):
            break
    ids = ",".join(map(str, prompt_ids))
    rates = []
    for g in range(n_gens + 1):  # +1 warm-up
        p.stdin.write(f"GEN {args.max_new} temperature=0 {ids}\n")
        p.stdin.flush()
        done = None
        for line in p.stdout:
            line = line.rstrip()
            if line.startswith("DONE") or line.startswith("ERR"):
                done = line
                break
        if g == 0:
            continue  # warm-up discarded
        m = RE_DONE.match(done or "")
        if m:
            produced, dms = int(m.group(1)), float(m.group(2))
            rates.append(produced / (dms / 1000.0) if dms > 0 else 0.0)
        else:
            print(f"  gen failed: {done}", file=sys.stderr)
    p.stdin.write("QUIT\n")
    p.stdin.flush()
    try:
        p.wait(timeout=120)
    except subprocess.TimeoutExpired:
        p.kill()
    # the key may also come from a quick line; wait a beat for the drain thread
    time.sleep(0.2)
    return key[0], rates


def write_profile(key, profile):
    path = store_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    rec = (f"{key}|headroom_mib={int(profile['headroom_mib'])};"
           f"rebalance_every={int(profile['rebalance_every'])};"
           f"rebalance_margin={float(profile['rebalance_margin'])};"
           f"warm_seed={float(profile['warm_seed'])};version=1")
    lines = []
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            lines = [l.rstrip("\n") for l in f if l.strip() and not l.startswith(key + "|")]
    lines.append(rec)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, path)
    print(f"wrote {path}\n  {rec}")


def load_prompt_ids(args):
    sys.path.insert(0, r"C:\multi-strata\engines\maya-ref\tools")
    from strata_tokenizer import Tokenizer  # noqa: E402
    tk = Tokenizer.from_gguf(args.model)
    return tk.encode(args.prompt, parse_special=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--gpu", type=int, default=0)
    ap.add_argument("--max-context", type=int, default=4096)
    ap.add_argument("--max-new", type=int, default=96)
    ap.add_argument("--gens", type=int, default=4)
    ap.add_argument("--prompt", default="[gMASK]<sop>Write a short paragraph explaining how a neural network learns from data.")
    ap.add_argument("--write", default=None, help="skip the sweep; store this profile (k=v;...) under the key")
    args = ap.parse_args()

    prompt_ids = load_prompt_ids(args)

    if args.write is not None:
        # scrape the key with one default run, then store the given profile (round-trip / manual use)
        key, _ = run_engine(args, DEFAULT_PROFILE, prompt_ids[:4], n_gens=0)
        if not key:
            print("could not scrape the calibration key", file=sys.stderr)
            return 2
        prof = dict(DEFAULT_PROFILE)
        for kv in args.write.split(";"):
            if "=" in kv:
                k, v = kv.split("=", 1)
                prof[k] = float(v) if "." in v else int(v)
        write_profile(key, prof)
        return 0

    # coordinate-descent sweep
    best = dict(DEFAULT_PROFILE)
    key = None
    best_rate = None
    for knob, candidates in SWEEP.items():
        for cand in candidates:
            trial = dict(best)
            trial[knob] = cand
            k, rates = run_engine(args, trial, prompt_ids, args.gens)
            key = key or k
            if not rates:
                print(f"  {knob}={cand}: no rate", file=sys.stderr)
                continue
            med = statistics.median(rates)
            tag = ""
            if best_rate is None:
                best_rate, best = med, trial
                tag = " (baseline)"
            elif med > best_rate * (1.0 + MIN_GAIN):
                best_rate, best = med, trial
                tag = " (new best)"
            print(f"  {knob}={cand}: {med:.2f} tok/s{tag}")
    if not key:
        print("could not scrape the calibration key", file=sys.stderr)
        return 2
    print(f"best: {best}  ({best_rate:.2f} tok/s)")
    write_profile(key, best)
    return 0


if __name__ == "__main__":
    sys.exit(main())
