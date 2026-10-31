"""The inflater's test fixtures (plugin spec 2026-10-30 section 12: a plugin's .zip is inflated by code written here, with
its own tests): raw DEFLATE streams made with Python's zlib, and malformed ones made bit by bit, in
secure_frontend/tests/fixtures/inflate/. secure_frontend/tests/inflate_test.cpp checks each one's output by its length and
SHA-256 (CASES below); tools/secure/test_inflate_fixtures.py checks the committed files still decode to them.

    python tools/secure/make_inflate_fixtures.py --write    (only when a case is added: zlib versions may compress differently)
"""
from __future__ import annotations

import hashlib
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "secure_frontend" / "tests" / "fixtures" / "inflate"
WORDS = [b"the", b"model", b"answers", b"plugin", b"signed", b"key", b"server", b"engine", b"expert", b"cache", b"token",
         b"window", b"search", b"folder", b"trust", b"zip", b"stream", b"block", b"length", b"distance"]


def text(n: int, seed: int) -> bytes:
    """Words picked by an LCG: compressible, with matches near and far."""
    out = bytearray()
    x = seed
    while len(out) < n:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out += WORDS[x % len(WORDS)]
        out += b" " if (x >> 8) % 7 else b".\n"
    return bytes(out[:n])


def periodic(n: int, period: int) -> bytes:
    """Pseudo-random bytes repeated every `period` bytes: matches at exactly that distance (near the window's 32 KiB)."""
    x = 7
    block = bytearray()
    for _ in range(period):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        block.append((x >> 16) & 0xFF)
    return (bytes(block) * (n // period + 1))[:n]


def deflate(data: bytes, level: int, strategy: int = zlib.Z_DEFAULT_STRATEGY) -> bytes:
    packer = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
    return packer.compress(data) + packer.flush()


class Bits:
    """RFC 1951's bit order: fields least significant bit first; Huffman codes most significant bit first."""

    def __init__(self) -> None:
        self.bits: list[int] = []

    def field(self, value: int, count: int) -> "Bits":
        self.bits += [(value >> i) & 1 for i in range(count)]
        return self

    def code(self, value: int, count: int) -> "Bits":
        self.bits += [(value >> (count - 1 - i)) & 1 for i in range(count)]
        return self

    def bytes(self) -> bytes:
        padded = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(sum(bit << i for i, bit in enumerate(padded[n:n + 8])) for n in range(0, len(padded), 8))


def good() -> dict[str, tuple[bytes, bytes]]:
    """name: (the data, its raw DEFLATE stream)."""
    return {
        "empty": (b"", deflate(b"", 6)),
        "stored": (text(5000, 1), deflate(text(5000, 1), 0)),
        "fixed": (text(300, 2), deflate(text(300, 2), 9, zlib.Z_FIXED)),
        "dynamic": (text(100000, 3), deflate(text(100000, 3), 9)),
        "far_matches": (periodic(200000, 32000), deflate(periodic(200000, 32000), 9)),
        "run_of_zeros": (bytes(70000), deflate(bytes(70000), 9)),
    }


def bad() -> dict[str, bytes]:
    """name: a malformed stream (the error the inflater must give is the C++ test's)."""
    dynamic = good()["dynamic"][1]
    return {
        # BFINAL 1, BTYPE 11 (reserved)
        "bad_block_type": Bits().field(1, 1).field(3, 2).bytes(),
        # BFINAL 1, BTYPE 00, then LEN 5 with an NLEN that is not its complement
        "bad_stored_length": Bits().field(1, 1).field(0, 2).bytes() + bytes([5, 0, 0x12, 0x34]) + b"hello",
        # BFINAL 1, BTYPE 01 (fixed), length 3 (code 257: 7 bits 0000001), distance 1 (code 0: 5 bits) before any output
        "bad_distance": Bits().field(1, 1).field(1, 2).code(0b0000001, 7).code(0, 5).code(0, 7).bytes(),
        # a dynamic stream cut in the middle
        "truncated": dynamic[: len(dynamic) // 2],
    }


def main(argv: list[str]) -> int:
    if "--write" in argv:
        FIXTURES.mkdir(parents=True, exist_ok=True)
        for name, (_, packed) in good().items():
            (FIXTURES / f"{name}.deflate").write_bytes(packed)
        for name, packed in bad().items():
            (FIXTURES / f"{name}.deflate").write_bytes(packed)
    for name, (data, packed) in good().items():
        print(f'{{"{name}", {len(data)}U, "{hashlib.sha256(data).hexdigest()}"}},  // {len(packed)} bytes compressed')
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
