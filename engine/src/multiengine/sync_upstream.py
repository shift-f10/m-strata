"""Bring engine/'s copy of upstream Strata's engine to an upstream commit - the update of the multiengine_strata family.

Reads the commit from a local clone's git objects only (its working tree is never touched) and writes, under engine/,
every upstream path of the engine's scope whose content differs (line endings aside), in the checkout's CRLF form:
src, include, tests, cmake, data, third_party/ggml, third_party/ggml-tq, the root CMakeLists.txt, and the bench files
engine/ tracks.  The root CMakeLists.txt gets our one include of cmake/multiengine.cmake appended.  Our own files are
not upstream paths, so they are never written: src/multiengine, cmake/multiengine.cmake, src/gemma4 (with its own core
copy), tests/gemma4, third_party/project-maya, the raon1 tests.  An upstream deletion is reported, not applied.

    python -I engine/src/multiengine/sync_upstream.py <upstream clone> <commit or tag> [--dry]

Then: read the diff for the supply chain, build, check every family against its references, and record the commit in
docs/PROVENANCE.md (the multiengine section) and docs/AGENT_HANDOFF.md.
"""
import pathlib
import subprocess
import sys

ENGINE = pathlib.Path(__file__).resolve().parents[2]
REPO = ENGINE.parent
SCOPES = ["src", "include", "tests", "cmake", "data", "third_party/ggml", "third_party/ggml-tq", "CMakeLists.txt"]
OURS = ("src/multiengine/", "cmake/multiengine", "src/gemma4/", "tests/gemma4/", "tests/core/raon1_geometry_test.cpp",
        "tests/oracle/raon1_prefill_parity.py")
INCLUDE_LINE = (b"\n# multi-strata: the multiengine executable over its family libraries (ours, not upstream's)\n"
                b"include(cmake/multiengine.cmake)\n")


def git(cwd, *args, stdin=None):
    return subprocess.run(["git", "-C", str(cwd), *args], input=stdin, capture_output=True, check=True).stdout


def names(cwd, *args):
    return [p for p in git(cwd, *args).decode().split("\0") if p]


def norm(b: bytes) -> bytes:
    return b.replace(b"\r\n", b"\n")


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    up, ref, dry = sys.argv[1], sys.argv[2], "--dry" in sys.argv[3:]
    commit = git(up, "rev-parse", "--verify", f"{ref}^{{commit}}").decode().strip()
    paths = names(up, "ls-tree", "-r", "--name-only", "-z", commit, "--", *SCOPES)
    up_bench = set(names(up, "ls-tree", "-r", "--name-only", "-z", commit, "--", "bench"))
    ours_bench = [p[len("engine/"):] for p in names(REPO, "ls-files", "-z", "engine/bench")]
    paths += [p for p in ours_bench if p in up_bench]
    gone = [p for p in ours_bench if p not in up_bench]

    out = git(up, "cat-file", "--batch", stdin="".join(f"{commit}:{p}\n" for p in paths).encode())
    blobs, pos = {}, 0
    for p in paths:
        nl = out.index(b"\n", pos)
        size = int(out[pos:nl].split()[2])
        blobs[p] = out[nl + 1:nl + 1 + size]
        pos = nl + 1 + size + 1

    written, added = [], []
    for p in paths:
        data = blobs[p]
        if p == "CMakeLists.txt":
            data = norm(data).rstrip(b"\n") + b"\n" + INCLUDE_LINE
        binary = b"\0" in data
        want = data if binary else norm(data).replace(b"\n", b"\r\n")
        dst = ENGINE / p
        if dst.exists():
            have = dst.read_bytes()
            if (have == want) if binary else (norm(have) == norm(want)):
                continue
            written.append(p)
        else:
            added.append(p)
        if not dry:
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(want)
    tracked = set(p[len("engine/"):] for p in names(REPO, "ls-files", "-z", "--", *[f"engine/{s}" for s in SCOPES]))
    upstream_set = set(paths)
    print(f"upstream {commit}: {len(paths)} paths in scope, {len(written)} rewritten, {len(added)} new"
          + (" (dry run: nothing written)" if dry else ""))
    for p in written:
        print("  M", p)
    for p in added:
        print("  A", p)
    for p in gone:
        print("  gone upstream (left in place):", p)
    # tracked here, not upstream's, and not one of ours: deleted (or renamed) upstream since the last sync
    unknown = sorted(p for p in tracked - upstream_set if not p.startswith(OURS))
    for p in unknown:
        print("  not in upstream and not ours (deleted upstream? left in place):", p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
