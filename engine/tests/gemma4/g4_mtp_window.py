"""Box-only check (spec §12, MTP): a verify window gives the logits of the same tokens' single decode steps, bit for
bit - so text accepted through MTP is exactly what plain decode writes. The engine's --spec-check mode reads each
window's tokens first as decode steps, then goes back and reads them as one window, and compares every row's 262,144
logits (the expert tier settled first, so both see the same residency).

Runs: the CPU lane on with windows of 4 over 1,800 tokens after a 293-token prompt (positions past the sliding ring's
2,048-row wrap and many attention-split boundaries), the lane off with windows of 8, and plain launches (--no-graph)
with windows of 3.

Usage: python -I g4_mtp_window.py ENGINE GGUF DRAFTER IDS_FILE
Exit 0 when every row of every run is identical.
"""
import subprocess
import sys

RUNS = [
    (["--spec", "4"], 1800),
    (["--spec", "8", "--cpu-lane", "off"], 400),
    (["--spec", "3", "--no-graph"], 120),
]


def main():
    engine, gguf, drafter, ids_file = sys.argv[1:5]
    with open(ids_file, encoding="utf-8") as f:
        ids = f.read().strip()
    fails = 0
    for args, rows in RUNS:
        cmd = [engine, "--native", gguf, "--mtp", drafter, "--max-context", "4096", "--tokens", ids, "--spec-check", str(rows)] + args
        r = subprocess.run(cmd, capture_output=True, text=True)
        line = next((l for l in r.stdout.splitlines() if l.startswith("spec-check:")), "(no spec-check line)")
        ok = r.returncode == 0 and line.startswith("spec-check:")   # the engine exits 0 only when every row matched
        print(("PASS " if ok else "FAIL ") + " ".join(args) + ": " + line)
        if not ok:
            fails += 1
            print(r.stderr[-3000:])
    print("ALL PASS" if fails == 0 else "%d run(s) FAILED" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
