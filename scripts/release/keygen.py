#!/usr/bin/env python3
"""SP7 M2 Task 4 (spec D-2): release key material.

Deterministic from --seed-hex (test vectors, recovery of a known key); without a
seed the seed is os.urandom(32) — an ephemeral signing key, written with 0600
and NEVER committed. Prints the pinned public-key hex exactly as
STRATA_RELEASE_PUBKEY_HEX wants it. The primitives come from sign.py (the
verbatim RFC 8032 section-6 reference code).
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sign  # noqa: E402


def key_material(seed_hex: str | None = None) -> dict:
    if seed_hex is None:
        seed = os.urandom(32)
    else:
        if not re.fullmatch(r"[0-9a-f]{64}", seed_hex):
            raise SystemExit("--seed-hex must be 64 lowercase hex characters")
        seed = bytes.fromhex(seed_hex)
    return {"seed_hex": seed.hex(), "public_hex": sign.secret_to_public(seed).hex()}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seed-hex", help="derive deterministically from this 64-hex seed instead of os.urandom")
    parser.add_argument("--out-key", type=Path, help="write the seed here (0600); omit to print only")
    parser.add_argument("--print-seed", action="store_true", help="also print the seed (never in a shared terminal for a real key)")
    args = parser.parse_args(argv)

    material = key_material(args.seed_hex)
    print(f"pinned public key: {material['public_hex']}")
    print("pass it as: -DSTRATA_RELEASE_PUBKEY_HEX=<hex>")
    if args.out_key:
        if args.out_key.exists():
            raise SystemExit(f"refusing to overwrite an existing key file: {args.out_key}")
        fd = os.open(args.out_key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as out:
            out.write(material["seed_hex"] + "\n")
        print(f"seed written to {args.out_key} (mode 0600; keep it offline, never commit it)")
    if args.print_seed:
        print(f"seed: {material['seed_hex']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
