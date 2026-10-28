#!/usr/bin/env python3
"""Prepare a downloaded Qwen GGUF for the engine with upstream's own pack tools (model downloads plan, D7).

    python -I tools/secure/prepare_download.py --gguf <first shard> --out <pack dir> --tools <upstream tools dir>
        [--pack-arg FLAG ...]

The server runs this after a catalog download whose `prepare` is "iq_pack", with the configured Python
(`models.prepare_python`) and upstream's read-only tools dir (`models.prepare_tools`).  What upstream setup.py does for
a FAMILIES model, in the same order and with the same arguments:

  1. <tools>/iq_pack.py --gguf <shard 1> --out <pack> [the family's pack args, e.g. --compat-bf16]
  2. <tools>/strata_tokenizer.py --gguf <shard 1> --out <pack>        (writes <pack>/tokenizer/)
  3. the shard holding the tensor per_layer_token_embd.weight (setup's --ple-gguf), read with <tools>/gguf_reader.py in
     a separate isolated Python (`-I`, sys.path set to the tools dir), so nothing of the tools runs in this process
  4. <pack>/prepared.json = {"pack", "tokenizer", "ple" (a shard path or null), "shards"}

stdout is the protocol, one line each: `STEP pack`, `STEP tokenizer`, `STEP ple`, then `DONE`, or `ERROR <reason>` with
a fixed reason (bad_arguments, not_gguf, missing_gguf, not_first_shard, missing_shard, missing_tool, pack_failed,
tokenizer_failed, ple_failed, write_failed) and a non-zero exit.  The tools' own output goes to stderr.  Every child is
`sys.executable -I <script> ...` as an argv list: no shell.  A `--pack-arg` is one plain flag (`--name`), passed
as the next argument (`--pack-arg --compat-bf16`) or joined (`--pack-arg=--compat-bf16`); argparse is not used because
it refuses a value that starts with `--`.  An earlier prepared.json is removed first, so a failed run leaves none.

Needs, in the Python that runs it: numpy (iq_pack.py), regex (strata_tokenizer.py), and for --compat-bf16 llama.cpp's
gguf-py (iq_pack.py finds it through STRATA_GGUF_PY, as upstream setup.py sets it).
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

PLE_TENSOR = "per_layer_token_embd.weight"
TOOLS = ("iq_pack.py", "strata_tokenizer.py", "gguf_reader.py")
OPTIONS = ("--gguf", "--out", "--tools")
PACK_ARG = re.compile(r"--[a-z0-9][a-z0-9-]{0,63}")
SHARD = re.compile(r"(?P<stem>.+)-(?P<i>\d{5})-of-(?P<n>\d{5})(?P<ext>\.gguf)", re.IGNORECASE)
# Run as `python -I -c PROBE <tools> <shard>...`: prints the index of the first shard holding the PLE table, or -1.
PROBE = f"""import pathlib, sys
sys.path.insert(0, sys.argv[1])
import gguf_reader
for i, p in enumerate(sys.argv[2:]):
    if any(t.name == {PLE_TENSOR!r} for t in gguf_reader.GGUFFile(pathlib.Path(p)).tensors):
        print(i)
        break
else:
    print(-1)
"""


class Refusal(Exception):
    """Stop with `ERROR <reason>`; the reason is one of the fixed words in the module docstring."""

    def __init__(self, reason: str):
        super().__init__(reason)
        self.reason = reason


def parse(argv: list[str]) -> tuple[dict[str, str], list[str], bool]:
    values: dict[str, str] = {}
    pack_args: list[str] = []
    tokenizer_only = False
    i = 0
    while i < len(argv):
        name, joined, value = argv[i].partition("=")
        if name == "--tokenizer-only" and not joined:
            # Qwen3.6 (OWN_PATH): the engine reads the GGUF itself, only the tokenizer is prepared.
            tokenizer_only = True
            i += 1
            continue
        if name not in (*OPTIONS, "--pack-arg"):
            raise Refusal("bad_arguments")
        if not joined:
            i += 1
            if i == len(argv):
                raise Refusal("bad_arguments")
            value = argv[i]
        if name == "--pack-arg":
            if not PACK_ARG.fullmatch(value):
                raise Refusal("bad_arguments")
            pack_args.append(value)
        elif name in values or not value:
            raise Refusal("bad_arguments")
        else:
            values[name] = value
        i += 1
    if any(o not in values for o in OPTIONS):
        raise Refusal("bad_arguments")
    if tokenizer_only and pack_args:
        raise Refusal("bad_arguments")
    return values, pack_args, tokenizer_only


def shard_list(first: Path) -> list[Path]:
    """The model's shards from its first one (<stem>-00001-of-0000N.gguf), or the one file of a single-file model."""
    if first.suffix.lower() != ".gguf":
        raise Refusal("not_gguf")
    if not first.is_file():
        raise Refusal("missing_gguf")
    m = SHARD.fullmatch(first.name)
    if m is None:
        return [first]
    n = int(m["n"])
    if int(m["i"]) != 1 or n < 1:
        raise Refusal("not_first_shard")
    shards = [first.with_name(f"{m['stem']}-{k:05d}-of-{m['n']}{m['ext']}") for k in range(1, n + 1)]
    if not all(s.is_file() for s in shards):
        raise Refusal("missing_shard")
    return shards


def child_stdout():
    """The tools' own output goes to our stderr: stdout carries the protocol only."""
    try:
        return sys.stderr.fileno()
    except (AttributeError, OSError, ValueError):
        return subprocess.DEVNULL


def run(argv: list[str], reason: str) -> None:
    try:
        rc = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=child_stdout(), check=False).returncode
    except OSError:
        raise Refusal(reason) from None
    if rc != 0:
        raise Refusal(reason)


def find_ple(tools: Path, shards: list[Path]) -> Path | None:
    try:
        done = subprocess.run([sys.executable, "-I", "-c", PROBE, str(tools), *map(str, shards)],
                              stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True, check=False)
    except OSError:
        raise Refusal("ple_failed") from None
    lines = done.stdout.split()
    if done.returncode != 0 or not lines or not re.fullmatch(r"-1|\d+", lines[-1]):
        raise Refusal("ple_failed")
    index = int(lines[-1])
    if index >= len(shards):
        raise Refusal("ple_failed")
    return shards[index] if index >= 0 else None


def step(name: str) -> None:
    print(f"STEP {name}", flush=True)


def prepare(argv: list[str]) -> None:
    values, pack_args, tokenizer_only = parse(argv)
    first = Path(os.path.abspath(values["--gguf"]))
    out = Path(os.path.abspath(values["--out"]))
    tools = Path(os.path.abspath(values["--tools"]))
    shards = shard_list(first)
    if not all((tools / t).is_file() for t in TOOLS):
        raise Refusal("missing_tool")
    record = out / "prepared.json"
    try:
        out.mkdir(parents=True, exist_ok=True)
        record.unlink(missing_ok=True)
    except OSError:
        raise Refusal("write_failed") from None
    if not tokenizer_only:
        step("pack")
        run([sys.executable, "-I", str(tools / "iq_pack.py"), "--gguf", str(first), "--out", str(out), *pack_args],
            "pack_failed")
    step("tokenizer")
    run([sys.executable, "-I", str(tools / "strata_tokenizer.py"), "--gguf", str(first), "--out", str(out)],
        "tokenizer_failed")
    ple = None
    if not tokenizer_only:
        step("ple")
        ple = find_ple(tools, shards)
    # tokenizer_only (Qwen3.6): pack null, no PLE; the engine loads the GGUF directly via --native.
    data = json.dumps({"pack": None if tokenizer_only else str(out), "tokenizer": str(out / "tokenizer"),
                       "ple": str(ple) if ple else None, "shards": len(shards)}, indent=2, ensure_ascii=True) + "\n"
    tmp = out / "prepared.json.tmp"
    try:
        tmp.write_bytes(data.encode("ascii"))
        os.replace(tmp, record)
    except OSError:
        raise Refusal("write_failed") from None


def main(argv=None) -> int:
    try:
        prepare(sys.argv[1:] if argv is None else list(argv))
    except Refusal as r:
        print(f"ERROR {r.reason}", flush=True)
        return 2 if r.reason == "bad_arguments" else 1
    print("DONE", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
