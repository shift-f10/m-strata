"""The zip reader's test fixtures (plugin spec 2026-10-30 section 12: a plugin's .zip is read by code written here): good
archives made with Python's zipfile, and hostile ones made byte by byte, in secure_frontend/tests/fixtures/zip/.
secure_frontend/tests/zip_reader_test.cpp reads them; tools/secure/test_zip_fixtures.py checks the good ones still list
and extract, through Python's zipfile, as the C++ test expects.

    python tools/secure/make_zip_fixtures.py --write
"""
from __future__ import annotations

import io
import struct
import sys
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "secure_frontend" / "tests" / "fixtures" / "zip"
MANIFEST = b'{"schema": "m-strata.plugin/1", "id": "lookup", "kind": "tool"}\n'
PROGRAM = bytes(range(256)) * 40 + b"lookup's program\n"


def made(entries: list[tuple[str, bytes, int]]) -> bytes:
    """An archive Python's zipfile writes: (name, data, method)."""
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as archive:
        for name, data, method in entries:
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 30, 12, 0, 0))
            info.compress_type = method
            info.external_attr = 0o644 << 16
            archive.writestr(info, data)
    return out.getvalue()


def good() -> dict[str, bytes]:
    return {
        "flat_deflated": made([("plugin.json", MANIFEST, zipfile.ZIP_DEFLATED), ("plugin.sig", b"sig\n", zipfile.ZIP_DEFLATED),
                               ("lookup", PROGRAM, zipfile.ZIP_DEFLATED)]),
        "folder_stored": made([("lookup/plugin.json", MANIFEST, zipfile.ZIP_STORED), ("lookup/plugin.sig", b"sig\n", zipfile.ZIP_STORED),
                               ("lookup/lookup", PROGRAM, zipfile.ZIP_STORED)]),
    }


# --- hostile archives, byte by byte (APPNOTE 4.3: local headers, then the central directory, then the end record) -----


def raw(entries: list[dict], comment: bytes = b"") -> bytes:
    """entries: name, data (the stored or compressed bytes), size, crc, method, flags, external, version_made, extra."""
    out = bytearray()
    central = bytearray()
    for e in entries:
        name = e["name"].encode("utf-8") if isinstance(e["name"], str) else e["name"]
        data = e["data"]
        size = e.get("size", len(data))
        crc = e.get("crc", zlib.crc32(e.get("plain", data)) & 0xFFFFFFFF)
        method = e.get("method", 0)
        flags = e.get("flags", 0)
        extra = e.get("extra", b"")
        offset = e.get("offset", len(out))
        local_name = e.get("local_name", name)
        out += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, flags, method, 0, 0, crc, len(data), size, len(local_name), len(extra))
        out += local_name + extra + data
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, e.get("version_made", 0x0314), 20, flags, method, 0, 0, crc,
                               e.get("compressed", len(data)), size, len(name), len(extra), 0, 0, 0,
                               e.get("external", 0o644 << 16), offset)
        central += name + extra
    start = len(out)
    out += central
    out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(entries), len(entries), len(central), start, len(comment)) + comment
    return bytes(out)


def entry(name, data=b"x", **more) -> dict:
    return {"name": name, "data": data, **more}


def bad() -> dict[str, bytes]:
    flat = good()["flat_deflated"]
    # a few compressed bytes that claim to inflate to 100 KB: past any honest DEFLATE ratio, refused unread
    deflated = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = deflated.compress(b"a" * 100) + deflated.flush()
    overlap = raw([entry("a", b"first"), entry("b", b"second")])
    # make b's central record point at a's local header
    second = overlap.rfind(b"PK\x01\x02")
    overlap = overlap[:second + 42] + struct.pack("<I", 0) + overlap[second + 46:]
    return {
        "traversal": raw([entry("../evil")]),
        "absolute": raw([entry("/etc/evil")]),
        "drive": raw([entry("C:evil")]),
        "backslash": raw([entry("dir\\evil")]),
        "control": raw([entry("evil\x01name")]),
        "symlink": raw([entry("link", b"/etc/passwd", external=(0o120777 << 16))]),
        "encrypted": raw([entry("secret", flags=0x0001)]),
        "zip64": raw([entry("big", extra=struct.pack("<HHQQ", 0x0001, 16, 1, 1))]),
        "bad_method": raw([entry("lzma", method=14)]),
        "bad_crc": raw([entry("wrong", b"hello", crc=0x12345678)]),
        "truncated": flat[: len(flat) - 30],
        "no_end": flat[: flat.rfind(b"PK\x05\x06")],
        "overlap": overlap,
        "local_disagrees": raw([entry("named", local_name=b"other")]),
        "too_many": raw([entry(f"f{i}") for i in range(257)]),
        "bomb": raw([entry("bomb", packed, method=8, size=100000, plain=b"a" * 100000)]),
        "duplicate": raw([entry("same"), entry("same")]),
        "empty_name": raw([entry("")]),
    }


def main(argv: list[str]) -> int:
    if "--write" in argv:
        FIXTURES.mkdir(parents=True, exist_ok=True)
        for name, data in {**good(), **bad()}.items():
            (FIXTURES / f"{name}.zip").write_bytes(data)
    for name, data in {**good(), **bad()}.items():
        print(f"{name}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
