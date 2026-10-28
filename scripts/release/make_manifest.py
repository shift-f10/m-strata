#!/usr/bin/env python3
"""Generate the real ``secure-release-manifest.json`` for a secure release.

The output is judged by ``scripts/secure_distribution_check.py --release``
(unchanged contract, see ``docs/security/deployment.md``).  The caller
supplies the release version and may pin ``--created-utc`` and ``--source``
for byte-reproducible builds; nothing here invents either.
"""
from __future__ import annotations

import argparse
import datetime
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from manifest_lib import release_manifest  # noqa: E402

REPO = Path(__file__).resolve().parents[2]


def git_revision() -> str:
    return subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"],
                          check=True, capture_output=True, text=True).stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="release version string")
    parser.add_argument("--created-utc",
                        default=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                        help="pin the build timestamp for reproducible output (YYYY-MM-DDTHH:MM:SSZ)")
    parser.add_argument("--source", default=None,
                        help="build provenance for every component (default: current git revision)")
    parser.add_argument("--out", type=Path, default=None,
                        help="write to this file instead of stdout")
    args = parser.parse_args()

    if not args.version.strip():
        print("make_manifest: --version must not be empty", file=sys.stderr)
        return 2
    source = args.source if args.source is not None else git_revision()
    document = release_manifest(args.version, args.created_utc, source)
    text = json.dumps(document, indent=2, sort_keys=True) + "\n"
    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
