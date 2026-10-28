"""Model downloads (plan 2026-10-23, Task 2, D7): prepare a downloaded Qwen GGUF with upstream's pack tools.

A fake tools dir stands in for upstream/tools: iq_pack.py, strata_tokenizer.py and gguf_reader.py stubs that record
how they were run.  A fake shard is a text file listing its tensor names.  No numpy, no model files.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "prepare_download.py"

# Each stub appends [its name, the Python running it, isolated?, argv...] to calls.jsonl beside it, prints noise on
# stdout (which must not reach the protocol), and fails when a file named fail-<its name> is beside it.
STUB = '''
import json, pathlib, sys
HERE = pathlib.Path(__file__).resolve().parent
NAME = {name!r}
with open(HERE / "calls.jsonl", "a", encoding="utf-8") as log:
    log.write(json.dumps([NAME, sys.executable, sys.flags.isolated] + sys.argv[1:]) + "\\n")
print("noise from " + NAME)
if (HERE / ("fail-" + NAME)).exists():
    sys.exit(5)
{body}
'''
PACK_BODY = '''
out = pathlib.Path(sys.argv[sys.argv.index("--out") + 1])
out.mkdir(parents=True, exist_ok=True)
(out / "native_experts.txt").write_text("v4\\n")
'''
TOKENIZER_BODY = '''
out = pathlib.Path(sys.argv[sys.argv.index("--out") + 1])
(out / "tokenizer").mkdir(parents=True, exist_ok=True)
(out / "tokenizer" / "vocab.json").write_text("{}")
'''
READER = '''
import pathlib

if (pathlib.Path(__file__).resolve().parent / "fail-gguf_reader").exists():
    raise RuntimeError("not a GGUF")


class TensorInfo:
    def __init__(self, name):
        self.name = name


class GGUFFile:
    def __init__(self, path):
        self.path = pathlib.Path(path)
        self.tensors = [TensorInfo(n) for n in self.path.read_text().split()]
'''


def fake_tools(root: Path) -> Path:
    tools = root / "tools"
    tools.mkdir(parents=True)
    (tools / "iq_pack.py").write_text(STUB.format(name="iq_pack", body=PACK_BODY), encoding="utf-8")
    (tools / "strata_tokenizer.py").write_text(STUB.format(name="strata_tokenizer", body=TOKENIZER_BODY),
                                               encoding="utf-8")
    (tools / "gguf_reader.py").write_text(READER, encoding="utf-8")
    return tools


def shards(folder: Path, stem: str, tensors: list[str]) -> list[Path]:
    folder.mkdir(parents=True, exist_ok=True)
    n = len(tensors)
    paths = [folder / f"{stem}-{i:05d}-of-{n:05d}.gguf" for i in range(1, n + 1)] if n > 1 else \
        [folder / f"{stem}.gguf"]
    for path, names in zip(paths, tensors):
        path.write_text(names, encoding="utf-8")
    return paths


def calls(tools: Path) -> list:
    log = tools / "calls.jsonl"
    return [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()] if log.exists() else []


def prepare(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", str(SCRIPT), *args], capture_output=True, text=True, check=False)


def same_file(a: str, b: str) -> bool:
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def test_packs_tokenizes_finds_the_ple_shard_and_records_it(tmp_path):
    tools = fake_tools(tmp_path)
    first, second = shards(tmp_path / "models", "Qwen-IQ2_XS", ["blk.0.ffn_up token_embd.weight",
                                                                "per_layer_token_embd.weight"])
    pack = tmp_path / "packs" / "qwen-iq2_xs"
    result = prepare("--gguf", str(first), "--out", str(pack), "--tools", str(tools), "--pack-arg", "--compat-bf16")
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["STEP pack", "STEP tokenizer", "STEP ple", "DONE"]
    made = calls(tools)
    assert [c[0] for c in made] == ["iq_pack", "strata_tokenizer"]
    assert all(same_file(c[1], sys.executable) and c[2] == 1 for c in made)     # sys.executable, isolated (-I)
    assert made[0][3:] == ["--gguf", str(first), "--out", str(pack), "--compat-bf16"]
    assert made[1][3:] == ["--gguf", str(first), "--out", str(pack)]
    assert json.loads((pack / "prepared.json").read_text(encoding="ascii")) == {
        "pack": str(pack), "tokenizer": str(pack / "tokenizer"), "ple": str(second), "shards": 2}


def test_a_single_file_model_and_the_equals_form(tmp_path):
    tools = fake_tools(tmp_path)
    (only,) = shards(tmp_path / "m", "Qwen3.8-Flash-Next-Gyro-S-TQ1_0", ["output.weight per_layer_token_embd.weight"])
    pack = tmp_path / "pack"
    result = prepare(f"--gguf={only}", f"--out={pack}", f"--tools={tools}", "--pack-arg=--compat-bf16")
    assert result.returncode == 0, result.stderr
    assert calls(tools)[0][3:] == ["--gguf", str(only), "--out", str(pack), "--compat-bf16"]
    prepared = json.loads((pack / "prepared.json").read_text(encoding="ascii"))
    assert (prepared["ple"], prepared["shards"]) == (str(only), 1)


def test_no_ple_tensor_is_recorded_as_null(tmp_path):
    tools = fake_tools(tmp_path)
    first, _ = shards(tmp_path / "m", "X", ["a.weight", "b.weight"])
    pack = tmp_path / "pack"
    result = prepare("--gguf", str(first), "--out", str(pack), "--tools", str(tools))
    assert result.stdout.splitlines()[-1] == "DONE"
    assert calls(tools)[0][3:] == ["--gguf", str(first), "--out", str(pack)]        # no pack args given
    assert json.loads((pack / "prepared.json").read_text(encoding="ascii"))["ple"] is None


def test_tokenizer_only_skips_the_pack_for_a_gguf_direct_model(tmp_path):
    # Qwen3.6 (OWN_PATH): only the tokenizer is prepared; the engine reads the GGUF itself (--native).
    tools = fake_tools(tmp_path)
    (only,) = shards(tmp_path / "m", "Qwen3.6-35B-A3B-Q8_0", ["output.weight per_layer_token_embd.weight"])
    pack = tmp_path / "packs" / "qwen3.6-35b-a3b-q8_0"
    result = prepare("--gguf", str(only), "--out", str(pack), "--tools", str(tools), "--tokenizer-only")
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["STEP tokenizer", "DONE"]        # no STEP pack / STEP ple
    assert [c[0] for c in calls(tools)] == ["strata_tokenizer"]            # iq_pack is not run
    assert json.loads((pack / "prepared.json").read_text(encoding="ascii")) == {
        "pack": None, "tokenizer": str(pack / "tokenizer"), "ple": None, "shards": 1}


def test_tokenizer_only_with_a_pack_arg_is_refused(tmp_path):
    tools = fake_tools(tmp_path)
    (only,) = shards(tmp_path / "m", "X", ["a.weight"])
    pack = tmp_path / "pack"
    result = prepare("--gguf", str(only), "--out", str(pack), "--tools", str(tools), "--tokenizer-only", "--pack-arg", "--compat-bf16")
    assert result.returncode != 0 and "bad_arguments" in result.stdout


def test_paths_with_shell_characters_pass_as_argv(tmp_path):
    root = tmp_path / "a & b; $(echo x) 'q'"
    tools = fake_tools(root)
    first, second = shards(root / "m dir", "Y", ["x", "per_layer_token_embd.weight"])
    pack = root / "pack && out %PATH%"          # (no | < > on Windows file names)
    result = prepare("--gguf", str(first), "--out", str(pack), "--tools", str(tools))
    assert result.returncode == 0, result.stderr
    assert calls(tools)[0][3:5] == ["--gguf", str(first)]
    assert json.loads((pack / "prepared.json").read_text(encoding="ascii"))["ple"] == str(second)
    source = SCRIPT.read_text(encoding="utf-8")
    for shell in ("shell=True", "os.system", "os.popen", "Popen(\""):
        assert shell not in source


def run_failing(tmp_path: Path, which: str) -> tuple[subprocess.CompletedProcess, Path, Path]:
    tools = fake_tools(tmp_path)
    (tools / f"fail-{which}").write_text("")
    first, _ = shards(tmp_path / "m", "Z", ["x", "per_layer_token_embd.weight"])
    pack = tmp_path / "pack"
    pack.mkdir()
    (pack / "prepared.json").write_text('{"stale": true}')       # an earlier run's record must not survive a failure
    return prepare("--gguf", str(first), "--out", str(pack), "--tools", str(tools)), tools, pack


def test_a_failing_pack_step_stops_with_error(tmp_path):
    result, tools, pack = run_failing(tmp_path, "iq_pack")
    assert result.returncode != 0
    assert result.stdout.splitlines() == ["STEP pack", "ERROR pack_failed"]
    assert [c[0] for c in calls(tools)] == ["iq_pack"]
    assert not (pack / "prepared.json").exists()


def test_a_failing_tokenizer_step_stops_with_error(tmp_path):
    result, _, pack = run_failing(tmp_path, "strata_tokenizer")
    assert result.returncode != 0
    assert result.stdout.splitlines() == ["STEP pack", "STEP tokenizer", "ERROR tokenizer_failed"]
    assert not (pack / "prepared.json").exists()


def test_a_failing_ple_step_stops_with_error(tmp_path):
    result, _, pack = run_failing(tmp_path, "gguf_reader")
    assert result.returncode != 0
    assert result.stdout.splitlines() == ["STEP pack", "STEP tokenizer", "STEP ple", "ERROR ple_failed"]
    assert not (pack / "prepared.json").exists()


def test_refusals_before_any_step(tmp_path):
    tools = fake_tools(tmp_path)
    first, second = shards(tmp_path / "m", "W", ["x", "y"])
    pack = str(tmp_path / "pack")
    cases = [
        (["--out", pack, "--tools", str(tools)], "bad_arguments"),                               # no --gguf
        (["--gguf", str(first), "--out", pack, "--tools", str(tools), "--bogus", "1"], "bad_arguments"),
        (["--gguf", str(first), "--out", pack, "--tools", str(tools), "--pack-arg", "x; rm"], "bad_arguments"),
        (["--gguf", str(first), "--gguf", str(first), "--out", pack, "--tools", str(tools)], "bad_arguments"),
        (["--gguf", str(tmp_path / "m" / "w.bin"), "--out", pack, "--tools", str(tools)], "not_gguf"),
        (["--gguf", str(second), "--out", pack, "--tools", str(tools)], "not_first_shard"),
        (["--gguf", str(tmp_path / "absent.gguf"), "--out", pack, "--tools", str(tools)], "missing_gguf"),
        (["--gguf", str(first), "--out", pack, "--tools", str(tmp_path / "m")], "missing_tool"),
    ]
    for argv, reason in cases:
        result = prepare(*argv)
        assert result.returncode != 0, argv
        assert result.stdout.splitlines() == [f"ERROR {reason}"], (argv, result.stdout)
    second.unlink()
    result = prepare("--gguf", str(first), "--out", pack, "--tools", str(tools))
    assert result.stdout.splitlines() == ["ERROR missing_shard"]
    assert calls(tools) == []
