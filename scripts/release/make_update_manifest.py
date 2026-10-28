#!/usr/bin/env python3
"""Generate the D-3 update manifest (``manifest.json``, strata-release-v1).

The bytes this writes are exactly what the C++ verifier
(``secure_frontend_release_manifest``) parses and what ``strata update``
consumes.  The closed component set {frontend, engine, cuda-runtime} carries
at most one file per class (the C++ schema rejects duplicate names): the
frontend class carries the CLI binary; engine and cuda-runtime are listed
only when they are actually present in the bundle.  sizes and sha256s are
computed from the shipped bytes — never supplied by the caller.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path


def component(bundle: Path, rel: str, name: str, notes_url: str) -> dict:
    payload = (bundle / rel).read_bytes()
    return {
        "name": name,
        "file": Path(rel).name,
        "size": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "notes_url": notes_url,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", required=True, type=Path, help="staged bundle directory")
    parser.add_argument("--version", required=True, help="release version (1..64 of [A-Za-z0-9._-])")
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--frontend-file", default="strata",
                        help="bundle bin/ filename carrying the frontend class")
    parser.add_argument("--notes-url", default="https://secure-strata.invalid/release-notes",
                        help="https release-notes URL listed with every component")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    components = [component(args.bundle, f"bin/{args.frontend_file}", "frontend", args.notes_url)]
    for engine_name in ("strata-engine", "secure-strata-engine"):
        if (args.bundle / "bin" / engine_name).is_file():
            components.append(component(args.bundle, f"bin/{engine_name}", "engine", args.notes_url))
            break
    cuda = sorted((args.bundle / "lib").glob("libcudart.so.*")) if (args.bundle / "lib").is_dir() else []
    if cuda:
        components.append(component(args.bundle, f"lib/{cuda[0].name}", "cuda-runtime", args.notes_url))

    document = {
        "format": "strata-release-v1",
        "version": args.version,
        "channel": args.channel,
        "components": components,
    }
    text = json.dumps(document, indent=2, sort_keys=True) + "\n"
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
