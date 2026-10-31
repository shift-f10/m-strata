"""The zip reader's fixtures (plugin spec 2026-10-30 section 12): the good archives list and extract through Python's
zipfile to the bytes and SHA-256s the C++ test (secure_frontend/tests/zip_reader_test.cpp) expects, and each hostile one
is what its name says - so the fixtures, the generator and the test agree."""
from __future__ import annotations

import hashlib
import importlib.util
import re
import struct
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "secure_frontend" / "tests" / "fixtures" / "zip"
CPP_TEST = ROOT / "secure_frontend" / "tests" / "zip_reader_test.cpp"


def generator():
    spec = importlib.util.spec_from_file_location("make_zip_fixtures", ROOT / "tools" / "secure" / "make_zip_fixtures.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_the_good_archives_extract_to_what_the_cpp_test_expects():
    expected = {name: (int(size), sha) for name, size, sha in
                re.findall(r'\{"([a-z.]+)", ([0-9]+)U, "([0-9a-f]{64})"\}', CPP_TEST.read_text(encoding="utf-8"))}
    assert set(expected) == {"plugin.json", "plugin.sig", "lookup"}
    for archive, prefix in (("flat_deflated", ""), ("folder_stored", "lookup/")):
        with zipfile.ZipFile(FIXTURES / f"{archive}.zip") as z:
            assert [i.filename for i in z.infolist()] == [prefix + n for n in ("plugin.json", "plugin.sig", "lookup")]
            for name, (size, sha) in expected.items():
                data = z.read(prefix + name)
                assert len(data) == size and hashlib.sha256(data).hexdigest() == sha, (archive, name)


def test_each_fixture_is_the_generators():
    made = {**generator().good(), **generator().bad()}
    for path in FIXTURES.glob("*.zip"):
        assert path.read_bytes() == made[path.stem], path.name
    assert {p.stem for p in FIXTURES.glob("*.zip")} == set(made)


def test_the_hostile_ones_are_what_they_say():
    def central(name: str) -> list[tuple[int, int, int, int, int, bytes]]:
        data = (FIXTURES / f"{name}.zip").read_bytes()
        out, at = [], data.find(b"PK\x01\x02")
        while at != -1:
            made, _, flags, method = struct.unpack("<HHHH", data[at + 4:at + 12])
            compressed, size, name_len = struct.unpack("<IIH", data[at + 20:at + 30])
            external, = struct.unpack("<I", data[at + 38:at + 42])
            out.append((flags, method, compressed, size, external, data[at + 46:at + 46 + name_len]))
            at = data.find(b"PK\x01\x02", at + 4)
        return out
    assert central("traversal")[0][5] == b"../evil" and central("absolute")[0][5].startswith(b"/")
    assert central("symlink")[0][4] >> 28 == 0o12 >> 0  # S_IFLNK in the external attributes' high bits
    assert central("encrypted")[0][0] & 1 and central("bad_method")[0][1] == 14
    assert len(central("too_many")) == 257
    flags, method, compressed, size, _, _ = central("bomb")[0]
    assert method == 8 and size > compressed * 1000
