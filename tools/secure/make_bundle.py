#!/usr/bin/env python3
"""Dev-only, never shipped: build a Strata model bundle from verified Qwen shards.

SP4 spec §2.7 / D5 (B'). Until the owner's determinism check (D5 (A)) or a C++
pack builder (D5 (C)) gives the Qwen --pack files pinned digests, a developer
builds the pack locally with upstream's pinned tools/iq_pack.py and publishes
it to the offline box as a *third-party* bundle:

  1. download the two shards of a quant (any way; this tool re-hashes them
     against tools/secure/qwen_bundle_pins.json),
  2. python3 -I tools/secure/make_bundle.py --model qwen3.8-flash-next --quant iq2_xs \
       --shards-dir <dir> --out <usb>/qwen-iq2_xs --gguf-py <llama.cpp>/gguf-py
  3. on the offline box: strata --config <file> import <usb>/qwen-iq2_xs --trust-manifest sha256:<printed hex>

The manifest is written under tag "<quant>-local" so it never shadows a future
catalog entry. Needs the dev venv with numpy and llama.cpp's gguf-py for the
real pack tool (upstream/tools/_paths.py reads STRATA_GGUF_PY).
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DEFAULT_PINS = HERE / "qwen_bundle_pins.json"
DEFAULT_PACK_TOOL = REPO / "upstream" / "tools" / "iq_pack.py"
PACK_REQUIRED = ("index.txt", "dense.bin", "native_experts.txt", "conversions.json")
TOKENIZER_REQUIRED = ("vocab.json", "merges.txt", "token_type.json", "tokenizer.json")
CHUNK = 8 * 1024 * 1024
MARKER = b"strata-bundle 1\n"
HOSTS = ["huggingface.co"]
REDIRECT_HOSTS = ["*.aws.cdn.hf.co", "cas-bridge.xethub.hf.co"]


class BundleError(Exception):
    """A refusal; the message never contains file contents."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def load_quant(pins_path: Path, model: str, quant: str) -> tuple[dict, dict]:
    pins = json.loads(pins_path.read_text(encoding="utf-8"))
    if model not in pins or quant not in pins[model]["quants"]:
        raise BundleError(f"no pins for {model}:{quant}")
    return pins[model], pins[model]["quants"][quant]


def verify_shards(shards_dir: Path, quant_pins: dict) -> list[Path]:
    paths = []
    for shard in quant_pins["shards"]:
        path = shards_dir / shard["file"]
        if not path.is_file() or path.is_symlink():
            raise BundleError(f"missing shard {shard['file']}")
        if path.stat().st_size != shard["size"]:
            raise BundleError(f"size mismatch for {shard['file']}")
        if sha256_file(path) != shard["sha256"]:
            raise BundleError(f"sha256 mismatch for {shard['file']}")
        paths.append(path)
    return paths


def prepare_output(out: Path) -> None:
    if out.exists():
        if not out.is_dir() or any(out.iterdir()):
            raise BundleError("output directory exists and is not empty")
    else:
        out.mkdir(mode=0o700, parents=False)
    os.chmod(out, 0o700)


def check_pack_layout(pack: Path) -> None:
    for name in PACK_REQUIRED:
        if not (pack / name).is_file():
            raise BundleError(f"pack tool did not produce {name}")
    for name in TOKENIZER_REQUIRED:
        if not (pack / "tokenizer" / name).is_file():
            raise BundleError(f"pack tool did not produce tokenizer/{name}")


def pack_files(pack: Path) -> list[tuple[str, Path]]:
    files = []
    for path in sorted(pack.rglob("*")):
        if path.is_symlink():
            raise BundleError("pack contains a symbolic link")
        if path.is_file():
            relative = path.relative_to(pack).as_posix()
            if path.stat().st_size == 0:
                # The manifest requires every file size >= 1 (pre-flight F-26).
                raise BundleError(f"pack tool produced an empty file {relative}")
            files.append(("pack/" + relative, path))
    return files


def build_bundle(args: argparse.Namespace) -> str:
    model_pins, quant_pins = load_quant(Path(args.pins), args.model, args.quant)
    shards = verify_shards(Path(args.shards_dir), quant_pins)
    out = Path(args.out)
    prepare_output(out)
    files_dir = out / "files"
    (files_dir / "weights").mkdir(parents=True, mode=0o700)
    pack = files_dir / "pack"
    env = dict(os.environ)
    if args.gguf_py:
        env["STRATA_GGUF_PY"] = args.gguf_py
    result = subprocess.run([args.python, str(args.pack_tool), "--gguf", str(shards[0]), "--out", str(pack)],
                            env=env, check=False)
    if result.returncode != 0:
        raise BundleError(f"pack tool failed with exit code {result.returncode}")
    check_pack_layout(pack)
    entries = []
    for shard, pin in zip(shards, quant_pins["shards"]):
        target = files_dir / "weights" / shard.name
        if args.move_shards:
            shutil.move(str(shard), target)
        else:
            shutil.copyfile(shard, target)
        if sha256_file(target) != pin["sha256"]:
            raise BundleError(f"copy of {shard.name} does not match its pin")
        url = f"https://huggingface.co/{model_pins['repo']}/resolve/{model_pins['revision']}/{quant_pins['dir']}/{shard.name}"
        entries.append({"path": f"weights/{shard.name}", "role": "weights", "size": pin["size"],
                        "sha256": pin["sha256"], "sources": [url]})
    for relative, path in pack_files(pack):
        role = "tokenizer" if relative.startswith("pack/tokenizer/") else "pack"
        entries.append({"path": relative, "role": role, "size": path.stat().st_size,
                        "sha256": sha256_file(path), "sources": []})
    names = [e["path"] for e in entries if e["role"] == "weights"]
    launch = {"native": names[0]}
    ple_index = args.ple_shard - 1
    if len(names) <= 2 and 0 <= ple_index < len(names):
        launch["ple_gguf"] = names[ple_index]
    launch["pack"] = "pack"
    launch["tokenizer_pack"] = "pack/tokenizer"
    manifest = {
        "format": "strata-model-manifest",
        "schema_version": 1,
        "name": args.model,
        "tag": f"{args.quant}-local",
        "version": args.version_label or f"{datetime.date.today():%Y.%m.%d}.1",
        "sequence": args.sequence,
        "profile": "qwen",
        "engine": {"family": "strata", "min_version": args.engine_min_version},
        "license": model_pins["license"],
        "hosts": HOSTS,
        "redirect_hosts": REDIRECT_HOSTS,
        "files": entries,
        "launch": launch,
    }
    manifest_bytes = (json.dumps(manifest, indent=2, ensure_ascii=True) + "\n").encode("ascii")
    (out / "manifest.json").write_bytes(manifest_bytes)
    digest = hashlib.sha256(manifest_bytes).hexdigest()
    sums = [(digest, "manifest.json")] + [(e["sha256"], "files/" + e["path"]) for e in entries]
    sums.sort(key=lambda item: item[1].encode("ascii"))
    (out / "SHA256SUMS").write_bytes("".join(f"{h}  {p}\n" for h, p in sums).encode("ascii"))
    (out / "STRATA-BUNDLE").write_bytes(MARKER)  # written last (§5.1)
    return digest


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--model", required=True)
    parser.add_argument("--quant", required=True)
    parser.add_argument("--shards-dir", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--pins", default=str(DEFAULT_PINS))
    parser.add_argument("--pack-tool", default=str(DEFAULT_PACK_TOOL))
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--gguf-py", default="")
    parser.add_argument("--move-shards", action="store_true")
    parser.add_argument("--version-label", default="")
    parser.add_argument("--sequence", type=int, default=1)
    parser.add_argument("--engine-min-version", default="0.1.44.1")
    parser.add_argument("--ple-shard", type=int, default=2,
                        help="shard holding per_layer_token_embd: 2 for the original GSQ-RCO files (setup.py step 7)")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        digest = build_bundle(args)
    except (BundleError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"manifest sha256: {digest}")
    print(f"strata --config <config.json> import {args.out} --trust-manifest sha256:{digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
