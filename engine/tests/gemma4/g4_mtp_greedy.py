"""Box-only check (spec §12, MTP): greedy output with the drafter is byte for byte the output without it. Each prompt
runs twice through --tokens (128 greedy tokens, the expert tier settled after the prompt and still during decode, as
--tokens runs keep it): plain, then with --mtp, which drafts 3 tokens a step and verifies them in one window. The CPU
lane on (the default) for every prompt, and off for the first. Prints each run's speed and the drafts accepted.

Both runs pin the expert slots (--expert-slots 50): the drafter's VRAM would otherwise leave its run fewer slots, and
a streamed prompt's MMQ products round by the slot split (their last bits move with it, measured in entry i of the
handoff), so the two runs would read a long prompt to different bits before MTP did anything.

Usage: python -I g4_mtp_greedy.py ENGINE GGUF DRAFTER IDS_FILE IDS [IDS ...]   (IDS_FILE: ids in a file; IDS: inline)
Exit 0 when every pair is identical.
"""
import subprocess
import sys


def run(engine, gguf, ids, extra):
    cmd = [engine, "--native", gguf, "--max-context", "4096", "--tokens", ids, "--n", "128", "--settle", "--expert-slots", "50"] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("engine failed: " + " ".join(extra) + "\n" + r.stderr[-3000:])
    gen = next((l for l in r.stdout.splitlines() if l.startswith("gen:")), None)
    speed = next((l for l in r.stderr.splitlines() if " tok/s" in l and ("steps in" in l or "tokens in" in l)), "")
    return gen, speed


def main():
    engine, gguf, drafter, ids_file = sys.argv[1:5]
    with open(ids_file, encoding="utf-8") as f:
        prompts = [f.read().strip()] + sys.argv[5:]
    fails = 0
    for i, ids in enumerate(prompts):
        for lane in (["--cpu-lane", "on"], ["--cpu-lane", "off"]) if i == 0 else (["--cpu-lane", "on"],):
            plain, s0 = run(engine, gguf, ids, lane)
            mtp, s1 = run(engine, gguf, ids, lane + ["--mtp", drafter])
            same = plain is not None and plain == mtp
            what = "prompt %d (%d tokens), %s" % (i, len(ids.split(",")), " ".join(lane))
            print(("PASS " if same else "FAIL ") + what + ": greedy with MTP = without")
            print("  plain: " + s0.strip())
            print("  mtp:   " + s1.strip())
            if not same:
                fails += 1
                print("  plain " + str(plain)[:400])
                print("  mtp   " + str(mtp)[:400])
    print("ALL PASS" if fails == 0 else "%d pair(s) FAILED" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
