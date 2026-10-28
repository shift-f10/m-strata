"""Box-only smoke test of strata-gemma4-engine --serve (plans 2-3 Task 7): the strata_0143 records
the frontend's protocol client accepts, end to end over the child's stdio.

Usage: python -I g4_serve_smoke.py ENGINE GGUF PROMPT_IDS [--context N]
Checks: INFO then READY <ctx> stop; GEN -> PP lines, T lines, DONE with the 15-field layout and a
finish literal; a follow-up GEN that extends the held tokens reuses them (DONE's reused field);
STOP mid-answer ends with finish "cancel"; a bad GEN gets one ERR line; QUIT exits 0.
"""
import subprocess
import sys
import threading
import queue
import time

FINISHES = {"stop", "length", "cancel", "nonfinite"}


def main():
    engine, gguf, ids = sys.argv[1], sys.argv[2], sys.argv[3]
    ctx = sys.argv[sys.argv.index("--context") + 1] if "--context" in sys.argv else "8192"
    p = subprocess.Popen([engine, "--serve", "--native", gguf, "--max-context", ctx], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(l.rstrip("\n")) for l in p.stdout], daemon=True).start()
    threading.Thread(target=lambda: [sys.stderr.write("  [engine] " + l) for l in p.stderr], daemon=True).start()
    fails = []

    def expect(cond, what):
        print(("PASS " if cond else "FAIL ") + what)
        if not cond:
            fails.append(what)

    def read(timeout=600):
        return lines.get(timeout=timeout)

    first = read()
    while first.startswith("INFO "):
        print("  " + first)
        first = read()
    expect(first == f"READY {ctx} stop", "READY <ctx> stop: " + first)

    def gen(line, stop_after=None):
        p.stdin.write(line + "\n")
        p.stdin.flush()
        toks, pp = [], 0
        while True:
            l = read()
            if l.startswith("PP "):
                pp += 1
                f = l.split()
                expect(len(f) == 5 and f[1].isdigit() and f[2].isdigit(), "PP shape: " + l)
            elif l.startswith("T "):
                toks.append(int(l[2:]))
                if stop_after is not None and len(toks) == stop_after:
                    p.stdin.write("STOP\n")
                    p.stdin.flush()
            elif l.startswith("DONE "):
                return toks, pp, l.split()
            else:
                return toks, pp, l.split()

    toks, pp, done = gen(f"GEN 48 {ids}")
    expect(done[0] == "DONE" and len(done) == 16 and done[5] in FINISHES, "DONE layout: " + " ".join(done))
    expect(int(done[1]) == len(toks), "DONE counts the T lines")
    expect(pp >= 1, "a prompt longer than 4 tokens prints PP")
    expect(done[5] == "stop" and toks and toks[-1] in (1, 106), "the answer ends on an end-of-turn id: " + str(toks[-1:]))
    print("  answer ids: " + ",".join(map(str, toks)))
    # the follow-up turn holds the first prompt and the answer (all but the last id, which was never read)
    follow = ids + "," + ",".join(map(str, toks)) + ",107,105,2364,107"
    t0 = time.time()
    toks2, _, done2 = gen(f"GEN 16 temperature=0.7 top_p=0.9 seed=7 {follow}")
    expect(done2[0] == "DONE" and int(done2[8]) == len(ids.split(",")) + len(toks) - 1,
           "a follow-up reuses the held tokens: reused " + (done2[8] if len(done2) > 8 else "?"))
    print(f"  follow-up in {time.time() - t0:.2f} s")
    toks3, _, done3 = gen(f"GEN 200 {ids}", stop_after=3)
    expect(done3[0] == "DONE" and done3[5] == "cancel" and len(toks3) <= 6, "STOP ends the answer as cancel: " + " ".join(done3[:6]))
    p.stdin.write("GEN 8 1,2,999999999\n")
    p.stdin.flush()
    err = read()
    expect(err.startswith("ERR "), "an out-of-vocabulary id gets ERR: " + err)
    p.stdin.write("QUIT\n")
    p.stdin.flush()
    expect(p.wait(timeout=60) == 0, "QUIT exits 0")
    print("FAILED: " + "; ".join(fails) if fails else "ALL PASS")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
