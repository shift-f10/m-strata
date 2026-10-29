"""Box-only check and measurement (spec §12, MTP) through --serve, as the frontend runs the engine: the same requests
to a plain engine (given --spec and --spec-min-p but no --mtp: they must change nothing) and to one with the drafter.

Checks: DONE keeps the 16-field layout, its fields 6 and 7 are the drafts accepted and offered - 0 0 without the
drafter, accepted <= offered and offered > 0 with it; the T lines are DONE's count; a seeded sampled request
(Gemma's card sampling) runs through the drafter; STOP still ends an answer as "cancel". Prints each request's decode
speed with and without the drafter and the acceptance rate; whether the greedy answers are the same tokens is printed
too (the expert tier moves at different positions in the two runs, so with the CPU lane on their last bits may part;
g4_mtp_greedy.py is the bit-for-bit gate).

Usage: python -I g4_mtp_serve.py ENGINE GGUF DRAFTER IDS_FILE IDS [IDS ...] [--context N] [--rounds N]
                                  [--both "ARGS"] [--mtp-args "ARGS"] [--same-gate]
  --both: engine flags for both runs (e.g. "--cpu-lane off"); --mtp-args: the drafter run's (default "--spec 4");
  --same-gate: every answer, greedy and sampled (seeded), must be the same tokens - for runs whose result does not
  depend on the expert tier's timing: with "--cpu-lane off" (decode's experts all on the grouped kernel) and --no-mmq
  (STRATA_G4_NO_MMQ=1: a streamed prompt's experts on it too; MMQ's products round by the slot split).
"""
import os
import queue
import subprocess
import sys
import threading

FINISHES = {"stop", "length", "cancel", "nonfinite"}


class Engine:
    def __init__(self, cmd, env=None):
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
                                  env=env)
        self.lines = queue.Queue()
        self.err = []
        threading.Thread(target=lambda: [self.lines.put(l.rstrip("\n")) for l in self.p.stdout], daemon=True).start()
        threading.Thread(target=lambda: [self.err.append(l) for l in self.p.stderr], daemon=True).start()
        first = self.read()
        while first.startswith("INFO "):
            first = self.read()
        self.ready = first

    def read(self, timeout=900):
        return self.lines.get(timeout=timeout)

    def gen(self, line, stop_after=None):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        toks = []
        while True:
            l = self.read()
            if l.startswith("PP "):
                continue
            if l.startswith("T "):
                toks.append(int(l[2:]))
                if stop_after is not None and len(toks) == stop_after:
                    self.p.stdin.write("STOP\n")
                    self.p.stdin.flush()
                continue
            return toks, l.split()

    def quit(self):
        self.p.stdin.write("QUIT\n")
        self.p.stdin.flush()
        return self.p.wait(timeout=120)


def main():
    argv = sys.argv[1:]
    ctx = "8192"
    rounds = 2
    if "--context" in argv:
        i = argv.index("--context")
        ctx = argv[i + 1]
        del argv[i:i + 2]
    if "--rounds" in argv:
        i = argv.index("--rounds")
        rounds = int(argv[i + 1])
        del argv[i:i + 2]
    both, mtp_args, same_gate = [], ["--spec", "4"], False
    if "--both" in argv:
        i = argv.index("--both")
        both = argv[i + 1].split()
        del argv[i:i + 2]
    if "--mtp-args" in argv:
        i = argv.index("--mtp-args")
        mtp_args = argv[i + 1].split()
        del argv[i:i + 2]
    if "--same-gate" in argv:
        argv.remove("--same-gate")
        same_gate = True
    baseline = "--baseline" in argv   # the second run without the drafter too: plain decode's own run-to-run agreement
    if baseline:
        argv.remove("--baseline")
    env = dict(os.environ)
    env.pop("STRATA_G4_NO_MMQ", None)
    if "--no-mmq" in argv:
        argv.remove("--no-mmq")
        env["STRATA_G4_NO_MMQ"] = "1"
    engine, gguf, drafter, ids_file = argv[:4]
    with open(ids_file, encoding="utf-8") as f:
        prompts = [f.read().strip()] + argv[4:]
    fails = []

    def expect(cond, what):
        print(("PASS " if cond else "FAIL ") + what)
        if not cond:
            fails.append(what)

    requests = []
    for r in range(rounds):   # round 0 warms the expert tier for each prompt; later rounds are the measured ones
        for i, ids in enumerate(prompts):
            requests.append(("r%d greedy p%d" % (r, i), "GEN 256 " + ids))
    requests.append(("sampled p0", "GEN 128 temperature=1 top_p=0.95 top_k=64 seed=7 " + prompts[0]))
    # the penalties read the history: the host sampler on every row's full logits, the history extended row by row
    requests.append(("penalized p0",
                     "GEN 128 temperature=0.8 top_k=40 penalty_repeat=1.15 penalty_present=0.3 penalty_last_n=64 seed=11 " + prompts[0]))
    results = {}
    base = [engine, "--serve", "--native", gguf, "--max-context", ctx] + both
    for name, extra in (("plain", ["--spec", "4", "--spec-min-p", "0.7"]), ("mtp", [] if baseline else ["--mtp", drafter] + mtp_args)):
        e = Engine(base + extra, env)
        expect(e.ready == "READY %s stop" % ctx, name + ": READY " + e.ready)
        out = {}
        for label, line in requests:
            toks, done = e.gen(line)
            ok = done[0] == "DONE" and len(done) == 16 and done[5] in FINISHES and int(done[1]) == len(toks)
            expect(ok, "%s %s: DONE layout and count: %s" % (name, label, " ".join(done)))
            if not ok:
                continue
            acc, off = int(done[6]), int(done[7])
            if name == "plain" or baseline:
                expect(acc == 0 and off == 0, "%s %s: no drafts without --mtp (--spec alone does nothing)" % (name, label))
            else:
                expect(0 <= acc <= off and off > 0, "%s %s: drafts accepted %d of %d offered" % (name, label, acc, off))
            out[label] = (toks, float(done[4]), acc, off, done[5])
        toks, done = e.gen("GEN 200 " + prompts[0], stop_after=3)
        expect(done[0] == "DONE" and done[5] == "cancel" and len(toks) <= 3 + 8, "%s: STOP ends the answer as cancel (%d tokens)" % (name, len(toks)))
        expect(e.quit() == 0, name + ": QUIT exits 0")
        results[name] = out
    print("")
    print("%-16s %8s %10s %10s %10s %s" % ("request", "tokens", "plain t/s", "mtp t/s", "accepted", "same tokens"))
    tot = {"plain": [0, 0.0], "mtp": [0, 0.0]}
    acc_all = off_all = 0
    for label, _ in requests:
        if label not in results.get("plain", {}) or label not in results.get("mtp", {}):
            continue
        p, m = results["plain"][label], results["mtp"][label]
        sp = 1000.0 * len(p[0]) / p[1] if p[1] > 0 else 0.0
        sm = 1000.0 * len(m[0]) / m[1] if m[1] > 0 else 0.0
        same = p[0] == m[0]
        first = next((i for i, (a, b) in enumerate(zip(p[0], m[0])) if a != b), min(len(p[0]), len(m[0])))
        print("%-16s %8d %10.1f %10.1f %4d/%-5d %s" % (label, len(m[0]), sp, sm, m[2], m[3], "yes" if same else "no (from token %d)" % first))
        if same_gate:
            expect(same, label + ": the answer with the drafter is the answer without it")
        if not label.startswith("r0"):
            tot["plain"][0] += len(p[0])
            tot["plain"][1] += p[1]
            tot["mtp"][0] += len(m[0])
            tot["mtp"][1] += m[1]
            acc_all += m[2]
            off_all += m[3]
    if tot["plain"][1] > 0 and tot["mtp"][1] > 0:
        sp = 1000.0 * tot["plain"][0] / tot["plain"][1]
        sm = 1000.0 * tot["mtp"][0] / tot["mtp"][1]
        print("measured rounds: plain %.1f tok/s, mtp %.1f tok/s (x%.2f); drafts accepted %d of %d (%.0f%%)" %
              (sp, sm, sm / sp if sp > 0 else 0.0, acc_all, off_all, 100.0 * acc_all / max(off_all, 1)))
    print("FAILED: " + "; ".join(fails) if fails else "ALL PASS")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
