"""scripts/security/engine_review.py: the engine supply-chain gate (docs/security/engine-supply-chain.md).

Hermetic: the diff cases build a throwaway git repository under tmp_path; the binary case
reads a Windows system binary and is skipped elsewhere.
"""
from __future__ import annotations

import importlib.util
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "security" / "engine_review.py"
_SPEC = importlib.util.spec_from_file_location("engine_review", SCRIPT)
review = importlib.util.module_from_spec(_SPEC)
sys.modules["engine_review"] = review  # dataclasses resolve their module's namespace through sys.modules
_SPEC.loader.exec_module(review)

BENIGN = {
    "src/ok.cpp": "int add(int a, int b) { return a + b; }\n",
    "CMakeLists.txt": "cmake_minimum_required(VERSION 3.24)\nproject(x)\n",
    "tools/ok.py": "print('hello')\n",
}
SUSPICIOUS = {
    "src/net.cpp": "#include <winsock2.h>\nint open_it() { return socket(AF_INET, SOCK_STREAM, 0); }\n",
    "src/run.cpp": "void go() { system(\"calc\"); }\n",
    "src/plugin.cpp": "void* h = LoadLibraryA(\"evil.dll\");\n",
    "CMakeLists.txt": "cmake_minimum_required(VERSION 3.24)\nproject(x)\nFetchContent_Declare(dep GIT_REPOSITORY https://example.invalid/dep.git)\n",
    "tools/fetch.py": "import subprocess\nsubprocess.run(['curl', 'x'])\nimport urllib.request\nurllib.request.urlopen('http://x')\n",
    "tools/blob.py": "DATA = '" + "QUJD" * 60 + "'\nimport base64\nbase64.b64decode(DATA)\n",
    "scripts/get.sh": "curl -O https://example.invalid/payload\n",
    "app/run.mjs": "import { spawn } from 'node:child_process';\nfetch('https://example.invalid');\n",
    "release/SHA256SUMS": "0" * 64 + "  payload.bin\n",
    "requirements.txt": "evilpkg==1.0\n",
}


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)


def make_repo(tmp_path: Path) -> Path:
    repo = tmp_path / "engine"
    repo.mkdir()
    git(repo, "init", "-q")
    git(repo, "config", "user.email", "test@example.invalid")
    git(repo, "config", "user.name", "test")
    git(repo, "config", "core.autocrlf", "false")
    for relative, text in BENIGN.items():
        path = repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "old")
    git(repo, "tag", "old")
    for relative, text in SUSPICIOUS.items():
        path = repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    (repo / "data").mkdir()
    (repo / "data" / "blob.bin").write_bytes(b"\x00\x01binary\x00payload")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "new")
    git(repo, "tag", "new")
    return repo


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, "-I", str(SCRIPT), *args], capture_output=True, text=True)


def test_every_rule_family_is_found(tmp_path):
    repo = make_repo(tmp_path)
    rules = {finding.rule for finding in review.scan_diff(repo, "old", "new")}
    for expected in ("net", "exec", "load", "fetch", "url", "py-exec", "py-net", "py-obf", "blob", "sh-fetch", "js-exec",
                     "js-net", "digest", "deps", "binary"):
        assert expected in rules, (expected, sorted(rules))


def test_benign_change_has_no_finding(tmp_path):
    repo = make_repo(tmp_path)
    (repo / "src" / "ok.cpp").write_text("int add(int a, int b) { return b + a; }\n", encoding="utf-8")
    git(repo, "commit", "-q", "-am", "benign")
    assert review.scan_diff(repo, "new", "HEAD") == []


def test_finding_id_survives_a_line_shift():
    assert review.finding_id("net", "src/a.cpp", "  socket(x)  ") == review.finding_id("net", "src/a.cpp", "socket(x)")
    assert review.finding_id("net", "src/a.cpp", "socket(x)") != review.finding_id("net", "src/b.cpp", "socket(x)")


def test_gate_requires_a_ruling_for_every_finding(tmp_path):
    repo = make_repo(tmp_path)
    rulings = tmp_path / "rulings.json"
    common = ["diff", "--repo", str(repo), "--old", "old", "--new", "new", "--rulings", str(rulings)]
    assert run(*common).returncode == 1, "no rulings file: every finding is unruled"
    assert run(*common, "--write-template").returncode == 0
    assert run(*common).returncode == 1, "a template's empty rulings still block"
    data = json.loads(rulings.read_text(encoding="utf-8"))
    for entry in data["rulings"].values():
        entry["ruling"], entry["why"] = "benign", "test"
    rulings.write_text(json.dumps(data), encoding="utf-8")
    report = tmp_path / "report.md"
    passed = run(*common, "--report", str(report))
    assert passed.returncode == 0, passed.stderr
    assert "unruled: 0; refused: 0" in report.read_text(encoding="utf-8")
    first = next(iter(data["rulings"]))
    data["rulings"][first]["ruling"] = "refused"
    rulings.write_text(json.dumps(data), encoding="utf-8")
    assert run(*common).returncode == 1, "a refused finding blocks the bump"


def test_template_keeps_existing_rulings(tmp_path):
    repo = make_repo(tmp_path)
    rulings = tmp_path / "rulings.json"
    common = ["diff", "--repo", str(repo), "--old", "old", "--new", "new", "--rulings", str(rulings)]
    run(*common, "--write-template")
    data = json.loads(rulings.read_text(encoding="utf-8"))
    first = next(iter(data["rulings"]))
    data["rulings"][first].update(ruling="accepted", why="kept")
    rulings.write_text(json.dumps(data), encoding="utf-8")
    run(*common, "--write-template")
    again = json.loads(rulings.read_text(encoding="utf-8"))
    assert again["rulings"][first]["ruling"] == "accepted" and again["rulings"][first]["why"] == "kept"


@pytest.mark.skipif(os.name != "nt", reason="reads a Windows system binary")
def test_binary_gate_flags_a_networking_binary(tmp_path):
    curl = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "curl.exe"
    if not curl.exists():
        pytest.skip("no System32 curl.exe")
    imports = review.pe_imports(curl.read_bytes())
    assert "ws2_32.dll" in imports, sorted(imports)
    allow = tmp_path / "allow.json"
    allow.write_text(json.dumps({"imports": ["*.dll"], "loadable": ["*.dll"]}), encoding="utf-8")
    result = run("binary", "--exe", str(curl), "--allowlist", str(allow))
    assert result.returncode == 1 and "network library ws2_32.dll" in result.stderr, result.stderr


def _synthetic_pe(thunk: int, hint_name: bytes = b"") -> bytes:
    """PE32+ with one section (VA 0x1000, raw 0x200) whose import directory names
    KERNEL32.dll with one thunk: an ordinal (bit 63) or the RVA of a hint/name entry."""
    image = bytearray(0x400)
    image[0:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x40)
    image[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HH", image, 0x44, 0x8664, 1)  # machine x64, one section
    struct.pack_into("<H", image, 0x54, 240)         # SizeOfOptionalHeader
    struct.pack_into("<H", image, 0x58, 0x20B)       # PE32+ magic
    struct.pack_into("<II", image, 0x58 + 120, 0x1000, 40)  # data directory 1: imports
    struct.pack_into("<IIII", image, 0x58 + 240 + 8, 0x200, 0x1000, 0x200, 0x200)  # section header
    struct.pack_into("<IIIII", image, 0x200, 0x1040, 0, 0, 0x1060, 0x1040)  # descriptor; the next stays zero
    struct.pack_into("<Q", image, 0x240, thunk)       # one thunk, then a zero thunk
    image[0x260:0x26C] = b"KERNEL32.dll"
    image[0x280:0x280 + len(hint_name)] = hint_name   # hint (2 bytes) + name, at RVA 0x1080
    return bytes(image)


ORDINAL_THUNK = (1 << 63) | 0x123  # kernel32.dll ordinal 291: no name in the binary to check


def test_binary_gate_refuses_an_unnamed_ordinal_import(tmp_path):
    exe = tmp_path / "ordinal.exe"
    exe.write_bytes(_synthetic_pe(ORDINAL_THUNK))
    allow = tmp_path / "allow.json"
    allow.write_text(json.dumps({"imports": ["kernel32.dll"], "loadable": []}), encoding="utf-8")
    result = run("binary", "--exe", str(exe), "--allowlist", str(allow))
    assert result.returncode == 1 and "imports kernel32.dll ordinal 291 by number" in result.stderr, result.stderr


def test_binary_gate_passes_the_same_import_by_name(tmp_path):
    exe = tmp_path / "named.exe"
    exe.write_bytes(_synthetic_pe(0x1080, b"\0\0GetTickCount\0"))
    allow = tmp_path / "allow.json"
    allow.write_text(json.dumps({"imports": ["kernel32.dll"], "loadable": []}), encoding="utf-8")
    result = run("binary", "--exe", str(exe), "--allowlist", str(allow))
    assert result.returncode == 0, result.stdout + result.stderr


def test_binary_gate_accepts_a_ruled_ordinal(tmp_path):
    exe = tmp_path / "ordinal.exe"
    exe.write_bytes(_synthetic_pe(ORDINAL_THUNK))
    allow = tmp_path / "allow.json"
    allow.write_text(json.dumps({"imports": ["kernel32.dll"], "loadable": [],
                                 "allowed_ordinals": {"kernel32.dll#291": "binds GetTickCount (ruled in the bump's review)"}}),
                     encoding="utf-8")
    result = run("binary", "--exe", str(exe), "--allowlist", str(allow))
    assert result.returncode == 0, result.stdout + result.stderr


def test_pe_parser_refuses_a_non_pe_file():
    with pytest.raises(ValueError):
        review.pe_imports(b"not a binary")
