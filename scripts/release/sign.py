#!/usr/bin/env python3
"""SP7 M2 Task 4 (spec D-2): the release-side Ed25519 signer, pure Python.

Provenance (lane ruling R-T0-b): the Ed25519 primitives below are the RFC 8032
section 6 reference code, transcribed VERBATIM from the fetched RFC 8032 text
(rfc-editor.org); the only edits were dropping the text's page headers. Do not
"improve" them: they are the exact code that generated the lane's verified
fixtures, and scripts/release/sign.py --self-test re-derives the RFC's own test
vectors (committed at secure_frontend/tests/fixtures/release/rfc8032-vectors.json)
against them. Interlock: the C++ verifier (secure_frontend_ed25519) accepts this
signer's output - see secure_frontend_release_manifest_tests.python_signed_fixture_verifies.

No signing key lives in this repository; --key-file names one at run time and
keygen.py generates one. Dependencies: the standard library only.

Usage:
  sign.py --key-file KEY --manifest MANIFEST --out ENVELOPE
  sign.py --verify --key-hex PIN --envelope ENVELOPE
  sign.py --self-test
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import sys
from pathlib import Path

# --- RFC 8032 section 6 reference code begins here (verbatim; see docstring) ---

def sha512(s):
    return hashlib.sha512(s).digest()

# Base field Z_p
p = 2**255 - 19

def modp_inv(x):
    return pow(x, p-2, p)

# Curve constant
d = -121665 * modp_inv(121666) % p

# Group order
q = 2**252 + 27742317777372353535851937790883648493

def sha512_modq(s):
    return int.from_bytes(sha512(s), "little") % q

## Then follows functions to perform point operations.

# Points are represented as tuples (X, Y, Z, T) of extended
# coordinates, with x = X/Z, y = Y/Z, x*y = T/Z

def point_add(P, Q):
    A, B = (P[1]-P[0]) * (Q[1]-Q[0]) % p, (P[1]+P[0]) * (Q[1]+Q[0]) % p;
    C, D = 2 * P[3] * Q[3] * d % p, 2 * P[2] * Q[2] % p;
    E, F, G, H = B-A, D-C, D+C, B+A;
    return (E*F, G*H, F*G, E*H);











# Computes Q = s * Q
def point_mul(s, P):
    Q = (0, 1, 1, 0)  # Neutral element
    while s > 0:
        if s & 1:
            Q = point_add(Q, P)
        P = point_add(P, P)
        s >>= 1
    return Q

def point_equal(P, Q):
    # x1 / z1 == x2 / z2  <==>  x1 * z2 == x2 * z1
    if (P[0] * Q[2] - Q[0] * P[2]) % p != 0:
        return False
    if (P[1] * Q[2] - Q[1] * P[2]) % p != 0:
        return False
    return True

## Now follows functions for point compression.

# Square root of -1
modp_sqrt_m1 = pow(2, (p-1) // 4, p)

# Compute corresponding x-coordinate, with low bit corresponding to
# sign, or return None on failure
def recover_x(y, sign):
    if y >= p:
        return None
    x2 = (y*y-1) * modp_inv(d*y*y+1)
    if x2 == 0:
        if sign:
            return None
        else:
            return 0

    # Compute square root of x2
    x = pow(x2, (p+3) // 8, p)
    if (x*x - x2) % p != 0:
        x = x * modp_sqrt_m1 % p
    if (x*x - x2) % p != 0:
        return None

    if (x & 1) != sign:
        x = p - x
    return x










# Base point
g_y = 4 * modp_inv(5) % p
g_x = recover_x(g_y, 0)
G = (g_x, g_y, 1, g_x * g_y % p)

def point_compress(P):
    zinv = modp_inv(P[2])
    x = P[0] * zinv % p
    y = P[1] * zinv % p
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")

def point_decompress(s):
    if len(s) != 32:
        raise Exception("Invalid input length for decompression")
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1

    x = recover_x(y, sign)
    if x is None:
        return None
    else:
        return (x, y, 1, x*y % p)

## These are functions for manipulating the private key.

def secret_expand(secret):
    if len(secret) != 32:
        raise Exception("Bad size of private key")
    h = sha512(secret)
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= (1 << 254)
    return (a, h[32:])

def secret_to_public(secret):
    (a, dummy) = secret_expand(secret)
    return point_compress(point_mul(a, G))

















## The signature function works as below.

def sign(secret, msg):
    a, prefix = secret_expand(secret)
    A = point_compress(point_mul(a, G))
    r = sha512_modq(prefix + msg)
    R = point_mul(r, G)
    Rs = point_compress(R)
    h = sha512_modq(Rs + A + msg)
    s = (r + h * a) % q
    return Rs + int.to_bytes(s, 32, "little")

## And finally the verification function.

def verify(public, msg, signature):
    if len(public) != 32:
        raise Exception("Bad public key length")
    if len(signature) != 64:
        Exception("Bad signature length")
    A = point_decompress(public)
    if not A:
        return False
    Rs = signature[:32]
    R = point_decompress(Rs)
    if not R:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= q: return False
    h = sha512_modq(Rs + public + msg)
    sB = point_mul(s, G)
    hA = point_mul(h, A)
    return point_equal(sB, point_add(R, hA))
# --- RFC 8032 section 6 reference code ends here ---

# The D-3 envelope framing (ours, minisign-style): manifest bytes, the marker
# line, then the base64 Ed25519 signature over exactly the manifest bytes.
FRAMING = b"\n--STRATA-SIGNATURE-V1\n"

VECTORS = Path(__file__).resolve().parents[2] / "secure_frontend" / "tests" / "fixtures" / "release" / "rfc8032-vectors.json"


def read_seed(key_file: Path) -> bytes:
    text = key_file.read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"[0-9a-f]{64}", text):
        raise SystemExit("the key file must hold one 64-character lowercase hex seed")
    return bytes.fromhex(text)


def envelope(manifest: bytes, seed: bytes) -> bytes:
    """The signed D-3 envelope for `manifest` bytes under the 32-byte `seed`.
    One trailing newline after the base64 token is part of the canonical shape
    (R-T2-c allows at most one; the lane's fixtures use exactly one)."""
    return manifest + FRAMING + base64.b64encode(sign(seed, manifest)) + b"\n"


def split_envelope(env: bytes):
    """Strictly split an envelope (one marker, canonical base64, 64-byte signature)."""
    if env.count(FRAMING) != 1:
        return None
    manifest, _, rest = env.partition(FRAMING)
    if rest.endswith(b"\n"):
        rest = rest[:-1]
    if rest.endswith(b"\r"):
        rest = rest[:-1]
    if len(rest) != 88:
        return None
    try:
        signature = base64.b64decode(rest.decode("ascii"), validate=True)
    except (ValueError, UnicodeDecodeError):
        return None
    if len(signature) != 64:
        return None
    return manifest, signature


def verify_envelope(env: bytes, public_key: bytes) -> bool:
    parts = split_envelope(env)
    if parts is None or len(public_key) != 32:
        return False
    manifest, signature = parts
    return verify(public_key, manifest, signature)


def self_test() -> str:
    vectors = json.loads(VECTORS.read_text(encoding="utf-8"))
    for name, vector in vectors.items():
        seed = bytes.fromhex(vector["sk"][:64])
        message = bytes.fromhex(vector["msg"])
        if secret_to_public(seed).hex() != vector["pk"]:
            raise AssertionError(f"RFC vector {name}: the public key does not derive as published")
        if sign(seed, message).hex() != vector["sig"]:
            raise AssertionError(f"RFC vector {name}: the signature does not reproduce")
        if not verify(bytes.fromhex(vector["pk"]), message, bytes.fromhex(vector["sig"])):
            raise AssertionError(f"RFC vector {name}: the official signature must verify")
    return f"self-test OK: {len(vectors)} RFC 8032 test vectors reproduced by this signer"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--key-file", type=Path, help="file holding the 64-hex Ed25519 seed")
    parser.add_argument("--manifest", type=Path, help="the manifest bytes to sign")
    parser.add_argument("--out", type=Path, help="where to write the envelope")
    parser.add_argument("--verify", action="store_true", help="verify instead of sign")
    parser.add_argument("--key-hex", help="the pinned public key (64 lowercase hex)")
    parser.add_argument("--envelope", type=Path, help="the envelope to verify")
    parser.add_argument("--self-test", action="store_true", help="re-derive the RFC test vectors and exit")
    args = parser.parse_args(argv)

    if args.self_test:
        print(self_test())
        return 0
    if args.verify:
        if not args.key_hex or not args.envelope:
            raise SystemExit("--verify needs --key-hex and --envelope")
        if not re.fullmatch(r"[0-9a-f]{64}", args.key_hex):
            raise SystemExit("--key-hex must be 64 lowercase hex characters")
        if verify_envelope(args.envelope.read_bytes(), bytes.fromhex(args.key_hex)):
            print("envelope verifies against the pinned key")
            return 0
        print("envelope does NOT verify against the pinned key", file=sys.stderr)
        return 1
    if not args.key_file or not args.manifest or not args.out:
        raise SystemExit("signing needs --key-file, --manifest and --out")
    env = envelope(args.manifest.read_bytes(), read_seed(args.key_file))
    args.out.write_bytes(env)
    print(f"wrote {args.out} ({len(env)} bytes, manifest + framing + signature)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
