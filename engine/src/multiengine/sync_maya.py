"""Bring engine/third_party/maya, our copy of Project Maya's engine, to a Maya commit - the update of the Maya families.

Reads the commit from a local clone's git objects only (its working tree is never touched) and writes, under
engine/third_party/maya, every path of the engine's scope whose content differs (line endings aside): src, include,
cmake, tests, CMakeLists.txt, VERSION, the MIT licence and the licences of the code the kernels carry
(third_party/ggml, exllamav3, beellama.cpp), the three expert profiles Maya's setup starts each family from
(data/expert-profile-{glm,ds4,mm3}.bin, which the install bundles), and MiniMax-M3.1's chat template, GGUF manifest and
licence (tools/mm3/chat_template.jinja, tools/mm3/download_gguf.json, LICENSE.minimax-m31; byte for byte).  Every file is
Maya's as Maya ships it - nothing of ours is written into
the copy, its CMakeLists.txt included (cmake/multiengine.cmake builds it as its own project).  Text files take the
checkout's CRLF form except where Maya's .gitattributes keeps bytes or LF (the configure-time patch, *.sh).  A path
deleted upstream is reported, not applied.

    python -I engine/src/multiengine/sync_maya.py <maya clone> <commit or tag> [--dry]

Then: read the diff for the supply chain, build, check every Maya family against Maya's own programs built from the
same copy, and record the commit in docs/PROVENANCE.md (the multiengine section) and docs/AGENT_HANDOFF.md.
"""
import pathlib
import subprocess
import sys

ENGINE = pathlib.Path(__file__).resolve().parents[2]
REPO = ENGINE.parent
DEST = ENGINE / "third_party" / "maya"
SCOPES = ["src", "include", "cmake", "tests", "third_party/ggml", "third_party/exllamav3/LICENSE",
          "third_party/beellama.cpp/LICENSE", "CMakeLists.txt", "VERSION", "LICENSE", "data/expert-profile-glm.bin",
          "data/expert-profile-ds4.bin", "data/expert-profile-mm3.bin",
          # MiniMax-M3.1-Flash (owner 2026-10-28, under the MiniMax Community License): Maya's chat template, which
          # every maya-mm3 pack carries, its GGUF quants' manifest (the download cards) and the licence they come under
          "tools/mm3/chat_template.jinja", "tools/mm3/download_gguf.json", "LICENSE.minimax-m31"]
RAW = ("third_party/ggml/",)   # Maya's .gitattributes: third_party/ggml/*.patch -text (applied at configure time)
# kept byte for byte: the template is copied into each pack as Maya ships it, the manifest and licence are cited by digest
EXACT = ("tools/mm3/", "LICENSE.minimax-m31")


def git(cwd, *args, stdin=None):
    return subprocess.run(["git", "-C", str(cwd), *args], input=stdin, capture_output=True, check=True).stdout


def names(cwd, *args):
    return [p for p in git(cwd, *args).decode().split("\0") if p]


def norm(b: bytes) -> bytes:
    return b.replace(b"\r\n", b"\n")


def exact(path: str, data: bytes) -> bool:
    """Whether the file is kept byte for byte: binary, or a patch Maya marks -text."""
    return b"\0" in data or (path.startswith(RAW) and path.endswith(".patch")) or path.startswith(EXACT)


def checkout_form(path: str, data: bytes) -> bytes:
    if exact(path, data):
        return data
    if path.endswith(".sh"):
        return norm(data)
    return norm(data).replace(b"\n", b"\r\n")


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    up, ref, dry = sys.argv[1], sys.argv[2], "--dry" in sys.argv[3:]
    commit = git(up, "rev-parse", "--verify", f"{ref}^{{commit}}").decode().strip()
    paths = names(up, "ls-tree", "-r", "--name-only", "-z", commit, "--", *SCOPES)

    out = git(up, "cat-file", "--batch", stdin="".join(f"{commit}:{p}\n" for p in paths).encode())
    blobs, pos = {}, 0
    for p in paths:
        nl = out.index(b"\n", pos)
        size = int(out[pos:nl].split()[2])
        blobs[p] = out[nl + 1:nl + 1 + size]
        pos = nl + 1 + size + 1

    written, added = [], []
    for p in paths:
        want = checkout_form(p, blobs[p])
        dst = DEST / p
        if dst.exists():
            have = dst.read_bytes()
            if (have == want) if exact(p, blobs[p]) else (norm(have) == norm(want)):
                continue
            written.append(p)
        else:
            added.append(p)
        if not dry:
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(want)
    rel = DEST.relative_to(REPO).as_posix() + "/"
    tracked = set(p[len(rel):] for p in names(REPO, "ls-files", "-z", "--", rel))
    on_disk = set(p.relative_to(DEST).as_posix() for p in DEST.rglob("*") if p.is_file()) if DEST.exists() else set()
    print(f"maya {commit}: {len(paths)} paths in scope, {len(written)} rewritten, {len(added)} new"
          + (" (dry run: nothing written)" if dry else ""))
    for p in written:
        print("  M", p)
    for p in added:
        print("  A", p)
    for p in sorted((tracked | on_disk) - set(paths)):
        print("  not in maya (deleted upstream? left in place):", p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
