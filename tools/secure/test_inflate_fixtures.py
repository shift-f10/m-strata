"""The inflater's fixtures (plugin spec 2026-10-30 section 12): each good stream decodes, through Python's own zlib, to
exactly the length and SHA-256 the C++ test (secure_frontend/tests/inflate_test.cpp) expects, and each malformed one is
refused by zlib for the reason the C++ test names - so the fixtures, the generator and the test agree, whatever zlib
version made them."""
from __future__ import annotations

import hashlib
import importlib.util
import re
import sys
import zlib
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "secure_frontend" / "tests" / "fixtures" / "inflate"
CPP_TEST = ROOT / "secure_frontend" / "tests" / "inflate_test.cpp"


def generator():
    spec = importlib.util.spec_from_file_location("make_inflate_fixtures", ROOT / "tools" / "secure" / "make_inflate_fixtures.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def cpp_cases() -> dict[str, tuple[int, str]]:
    text = CPP_TEST.read_text(encoding="utf-8")
    return {name: (int(size), sha) for name, size, sha in re.findall(r'\{"([a-z_]+)", ([0-9]+)U, "([0-9a-f]{64})"\}', text)}


def test_each_good_fixture_decodes_to_what_the_cpp_test_expects():
    cases = cpp_cases()
    made = generator().good()
    assert set(cases) == set(made), "the C++ table names every good case"
    for name, (size, sha) in cases.items():
        data = zlib.decompress((FIXTURES / f"{name}.deflate").read_bytes(), -15)
        assert len(data) == size and hashlib.sha256(data).hexdigest() == sha, name
        assert data == made[name][0], f"{name}: the generator's data"


@pytest.mark.parametrize("name, words", [("bad_block_type", "invalid block type"),
                                         ("bad_stored_length", "invalid stored block lengths"),
                                         ("bad_distance", "invalid distance too far back"),
                                         ("truncated", "incomplete or truncated stream")])
def test_each_malformed_fixture_is_refused_by_zlib_too(name, words):
    with pytest.raises(zlib.error, match=words):
        zlib.decompress((FIXTURES / f"{name}.deflate").read_bytes(), -15)
    assert (FIXTURES / f"{name}.deflate").read_bytes() == generator().bad()[name], f"{name}: the generator's bytes"
