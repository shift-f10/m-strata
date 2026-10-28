"""Tests for the dev-only bundle builder (SP4 Task 13). Pure local files; no network, no real model."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("make_bundle", HERE / "make_bundle.py")
mb = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
_spec.loader.exec_module(mb)

# Every file is non-empty (Task 8 requires manifest file size >= 1; pre-flight F-26)
# unless a test names it in EMPTY.
FAKE_PACK_TOOL = '''
import argparse, pathlib
p = argparse.ArgumentParser(); p.add_argument("--gguf"); p.add_argument("--out"); a = p.parse_args()
out = pathlib.Path(a.out); (out / "tokenizer").mkdir(parents=True)
for name, text in {"index.txt": "i\\n", "dense.bin": "d", "native_experts.txt": "n\\n", "conversions.json": "{}\\n"}.items():
    if name not in SKIP:
        (out / name).write_text("" if name in EMPTY else text)
for name, text in {"vocab.json": '{"a":0}', "merges.txt": "#version: 0.2\\n", "token_type.json": "[1]", "tokenizer.json": "{}"}.items():
    (out / "tokenizer" / name).write_text("" if "tokenizer/" + name in EMPTY else text)
'''


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def setup_inputs(tmp_path: Path, skip: tuple[str, ...] = (), empty: tuple[str, ...] = ()) -> list[str]:
    shards = tmp_path / "shards"
    shards.mkdir()
    one, two = b"shard one", b"shard two"
    (shards / "M-IQ2_XS-00001-of-00002.gguf").write_bytes(one)
    (shards / "M-IQ2_XS-00002-of-00002.gguf").write_bytes(two)
    pins = {
        "qwen3.8-flash-next": {
            "repo": "ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF",
            "revision": "ed59f92082b1e93c0e96d60a8b11aab089b52f09",
            "license": {"id": "Apache-2.0", "url": "https://huggingface.co/ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF",
                        "bundle_allowed": True, "notice": "Test notice."},
            "quants": {"iq2_xs": {"dir": "IQ2_XS", "shards": [
                {"file": "M-IQ2_XS-00001-of-00002.gguf", "size": len(one), "sha256": sha(one)},
                {"file": "M-IQ2_XS-00002-of-00002.gguf", "size": len(two), "sha256": sha(two)}]}},
        }
    }
    (tmp_path / "pins.json").write_text(json.dumps(pins))
    (tmp_path / "pack_tool.py").write_text(f"SKIP = {skip!r}\nEMPTY = {empty!r}\n" + FAKE_PACK_TOOL)
    return ["--model", "qwen3.8-flash-next", "--quant", "iq2_xs", "--shards-dir", str(shards),
            "--out", str(tmp_path / "bundle"), "--pins", str(tmp_path / "pins.json"),
            "--pack-tool", str(tmp_path / "pack_tool.py"), "--python", sys.executable,
            "--version-label", "2026.10.14.1", "--engine-min-version", "0.1.44.1"]


def test_builds_complete_bundle_and_prints_trust_digest(tmp_path, capsys):
    assert mb.main(setup_inputs(tmp_path)) == 0
    bundle = tmp_path / "bundle"
    manifest_bytes = (bundle / "manifest.json").read_bytes()
    digest = sha(manifest_bytes)
    printed = capsys.readouterr().out
    assert f"manifest sha256: {digest}" in printed
    assert f"--trust-manifest sha256:{digest}" in printed
    assert (bundle / "STRATA-BUNDLE").read_bytes() == b"strata-bundle 1\n"
    manifest = json.loads(manifest_bytes)
    assert manifest["format"] == "strata-model-manifest" and manifest["schema_version"] == 1
    assert manifest["name"] == "qwen3.8-flash-next" and manifest["tag"] == "iq2_xs-local"
    assert manifest["profile"] == "qwen" and manifest["engine"] == {"family": "strata", "min_version": "0.1.44.1"}
    paths = [f["path"] for f in manifest["files"]]
    assert "weights/M-IQ2_XS-00001-of-00002.gguf" in paths and "pack/tokenizer/vocab.json" in paths
    shard = next(f for f in manifest["files"] if f["path"].endswith("00001-of-00002.gguf"))
    assert shard["sources"] == ["https://huggingface.co/ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF/resolve/"
                                "ed59f92082b1e93c0e96d60a8b11aab089b52f09/IQ2_XS/M-IQ2_XS-00001-of-00002.gguf"]
    assert all(f["sources"] == [] for f in manifest["files"] if f["path"].startswith("pack/"))
    assert all(f["size"] >= 1 for f in manifest["files"])
    roles = {f["path"]: f["role"] for f in manifest["files"]}
    assert roles["pack/tokenizer/merges.txt"] == "tokenizer" and roles["pack/index.txt"] == "pack"
    assert manifest["launch"] == {"native": "weights/M-IQ2_XS-00001-of-00002.gguf",
                                  "ple_gguf": "weights/M-IQ2_XS-00002-of-00002.gguf",
                                  "pack": "pack", "tokenizer_pack": "pack/tokenizer"}
    sums = (bundle / "SHA256SUMS").read_text().splitlines()
    sum_paths = [line[66:] for line in sums]
    assert sum_paths == sorted(sum_paths, key=lambda p: p.encode()) and "manifest.json" in sum_paths
    for line in sums:
        target = bundle / line[66:]
        assert line[:64] == sha(target.read_bytes())
    assert not (bundle / "MISSING").exists()
    assert all(32 <= ord(c) <= 126 or c == "\n" for c in manifest_bytes.decode("ascii"))


def test_refuses_shard_digest_mismatch(tmp_path):
    args = setup_inputs(tmp_path)
    (tmp_path / "shards" / "M-IQ2_XS-00002-of-00002.gguf").write_bytes(b"tampered")
    assert mb.main(args) == 2
    assert not (tmp_path / "bundle").exists()


def test_refuses_non_empty_output(tmp_path):
    args = setup_inputs(tmp_path)
    (tmp_path / "bundle").mkdir()
    (tmp_path / "bundle" / "x").write_text("x")
    assert mb.main(args) == 2


def test_refuses_incomplete_pack_layout(tmp_path):
    assert mb.main(setup_inputs(tmp_path, skip=("dense.bin",))) == 2
    assert not (tmp_path / "bundle" / "STRATA-BUNDLE").exists()


@pytest.mark.parametrize("empty", ["tokenizer/merges.txt", "conversions.json"])
def test_refuses_empty_pack_file(tmp_path, capsys, empty):
    # Task 8 rejects a manifest file of size 0 (pre-flight F-26): never emit one.
    assert mb.main(setup_inputs(tmp_path, empty=(empty,))) == 2
    assert not (tmp_path / "bundle" / "manifest.json").exists()
    assert not (tmp_path / "bundle" / "STRATA-BUNDLE").exists()
    assert "empty" in capsys.readouterr().err


# SP4 Task 20 (Task 13 carry): the C++ bundle reader is tested against a bundle this tool
# wrote. The fake pack tool emits a tokenizer SP1's load_local_tokenizer_pack accepts
# (the same bytes as secure_frontend/tests/support/acquire_fixtures.hpp's qwen_payload).
CPP_FIXTURE = HERE.parent.parent / "secure_frontend" / "tests" / "fixtures" / "bundle" / "make-bundle-iq2_xs"
CPP_FIXTURE_PACK_TOOL = '''
import argparse, pathlib
p = argparse.ArgumentParser(); p.add_argument("--gguf"); p.add_argument("--out"); a = p.parse_args()
out = pathlib.Path(a.out); (out / "tokenizer").mkdir(parents=True)
for name, text in {"index.txt": "index\\n", "dense.bin": "dense", "native_experts.txt": "experts\\n",
                   "conversions.json": "{}\\n"}.items():
    (out / name).write_bytes(text.encode())
for name, text in {"vocab.json": '{"a":0,"b":1,"ab":2}', "merges.txt": "a b\\n", "token_type.json": "[1,1,1]",
                   "tokenizer.json": "{}\\n"}.items():
    (out / "tokenizer" / name).write_bytes(text.encode())
'''


def build_cpp_fixture(work: Path, out: Path) -> str:
    args = setup_inputs(work)
    (work / "pack_tool.py").write_text(CPP_FIXTURE_PACK_TOOL)
    args[args.index("--out") + 1] = str(out)
    assert mb.main(args) == 0
    return sha((out / "manifest.json").read_bytes())


def tree(root: Path) -> dict[str, bytes]:
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in sorted(root.rglob("*")) if p.is_file()}


def test_cpp_bundle_fixture_is_this_tools_output(tmp_path):
    built = tmp_path / "bundle"
    build_cpp_fixture(tmp_path, built)
    assert tree(built) == tree(CPP_FIXTURE), (
        "secure_frontend/tests/fixtures/bundle/make-bundle-iq2_xs drifted from make_bundle.py output; regenerate it")


def test_pins_file_matches_spec_facts():
    pins = json.loads((HERE / "qwen_bundle_pins.json").read_text())
    gsq = pins["qwen3.8-flash-next"]
    assert gsq["revision"] == "ed59f92082b1e93c0e96d60a8b11aab089b52f09"
    one, two = gsq["quants"]["iq2_xs"]["shards"]
    assert one["size"] == 39225954592 and one["sha256"] == "92cee27ae5bbadcd732416a0f7a7f0acc092399dbbe8f5a5efa707c2ec0a49d7"
    assert two["size"] == 28800138432 and two["sha256"] == "316b46f3a2dbd68c900f43136ab9449f9dcc3725dfd8c794847c204bc161e113"
    assert set(gsq["quants"]) == {"iq2_xs", "iq3_xxs", "iq3_s"}
    coder = pins["qwen3.8-flash-next-coder"]
    assert coder["revision"] == "5348543e0147355ac9cbcb031184a3546350988e" and set(coder["quants"]) == {"iq1_m"}
    for model in pins.values():
        assert model["license"]["id"] == "Apache-2.0" and model["license"]["bundle_allowed"] is True
