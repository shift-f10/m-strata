"""Model downloads (plan 2026-10-23, Task 2): the download catalog, generated from the pinned upstream and Maya trees.

Generation reads the REAL pinned files (upstream/setup.py, engines/maya-ref/maya.py); the refusal cases run on copies
of them with one fact changed.  No network, no model files.
"""

from __future__ import annotations

import importlib.util
import json
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SCRIPT = HERE / "download_catalog.py"
COMMITTED = REPO / "secure_frontend" / "catalog" / "downloads.json"
_spec = importlib.util.spec_from_file_location("download_catalog", SCRIPT)
dc = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
_spec.loader.exec_module(dc)

HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")
ID = re.compile(r"[a-z0-9._-]{1,64}")
REPO_NAME = re.compile(r"[A-Za-z0-9._-]+/[A-Za-z0-9._-]+")
ISTA = "ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF"
ISTA_REV = "ed59f92082b1e93c0e96d60a8b11aab089b52f09"
MAYA_REPO = "peasantsmith/GLM-5.3-Flash-Maya-GGUF"
KEYS = {"id", "name", "source", "family", "engine", "about", "repo", "revision", "files", "prepare", "pack_args",
        "expert_profile", "mmproj"}


@pytest.fixture(scope="module")
def generated():
    return dc.build(dc.UPSTREAM, dc.MAYA, dc.UPSTREAM_VERSION)


def by_id(doc: dict) -> dict:
    return {m["id"]: m for m in doc["models"]}


def every_file(m: dict) -> list:
    return m["files"] + ([m["mmproj"]] if m["mmproj"] else [])


def test_iq3_s_comes_from_ista_at_its_pinned_commit(generated):
    doc, _ = generated
    m = by_id(doc)["qwen3.8-flash-next-iq3_s"]
    assert m["name"] == "Qwen3.8-Flash-Next IQ3_S"
    assert (m["source"], m["family"], m["engine"]) == ("strata", "qwen", "qwen")
    assert (m["repo"], m["revision"]) == (ISTA, ISTA_REV)
    assert m["files"] == [
        {"path": "IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf", "size": 54817524224,
         "sha256": "4c1eb2ceb4915e1192f4f386021897bde56a97f40a0bb78bb86465e0f7d2aca3"},
        {"path": "IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00002-of-00002.gguf", "size": 28800138432,
         "sha256": "316b46f3a2dbd68c900f43136ab9449f9dcc3725dfd8c794847c204bc161e113"}]
    assert (m["prepare"], m["pack_args"], m["expert_profile"]) == ("iq_pack", [], "expert-profile.bin")
    assert m["mmproj"] == {"path": "mmproj-Qwen3.8-Flash-Next-BF16.gguf", "size": 907543008,
                           "sha256": "b1a82259702816a5330d7bd7607cd9676b11780e79ff7348c21103ff3ce49bd0",
                           "repo": ISTA, "revision": ISTA_REV}
    assert m["about"].startswith("3.5-bit i-quant, the best quality (matches the full model)")


def test_one_entry_per_gguf_variant_of_each_family(generated):
    doc, _ = generated
    assert Counter((m["source"], m["family"]) for m in doc["models"]) == {
        ("strata", "qwen"): 4, ("strata", "swift"): 3, ("strata", "coder"): 1, ("strata", "unsloth"): 5,
        ("strata", "gyro"): 2, ("strata", "qwen36"): 3, ("maya", "glm53"): 5, ("maya", "deepseek"): 4, ("maya", "minimax"): 2}
    ids = set(by_id(doc))
    assert {"qwen3.8-flash-next-q2_0", "qwen3.8-flash-next-iq2_xs", "qwen3.8-flash-next-iq3_xxs",
            "qwen3.8-flash-next-iq3_s"} <= ids
    # upstream offers Swift 1.5 without Q2_0 (MODELS["Q2_0"]["families"] == ("qwen",), #171)
    assert {m["id"] for m in doc["models"] if m["family"] == "swift"} == {
        "swift-1.5-iq2_xs", "swift-1.5-iq3_xxs", "swift-1.5-iq3_s"}


def test_only_hugging_face_gguf_files_o4(generated):
    doc, notes = generated
    for m in doc["models"]:
        assert REPO_NAME.fullmatch(m["repo"]), m["id"]
        for f in every_file(m):
            assert f["path"].lower().endswith(".gguf"), (m["id"], f["path"])
            assert "safetensors" not in f["path"].lower()
            assert not f["path"].startswith("/") and ".." not in f["path"].split("/")
        for repo in [m["repo"]] + ([m["mmproj"]["repo"]] if m["mmproj"] else []):
            assert REPO_NAME.fullmatch(repo)
            # NVFP4 packs (Maximilian228, nvidia), EXL3 (turboderp), DFlash2 (CC BY-NC-ND: never) stay out
            for refused in ("Maximilian228/", "nvidia/", "turboderp/", "DFlash"):
                assert refused not in repo, (m["id"], repo)
    text = "\n".join(notes)
    for left_out in ("NVFP4", "NVFP4-NVIDIA", "NVFP4-QAD", "EXL3-3.05bpw", "EXL3-4.05bpw"):
        assert re.search(rf"\b{re.escape(left_out)}\b", text), left_out


def test_every_file_is_pinned(generated):
    doc, notes = generated
    assert not [n for n in notes if "sha256" in n]       # nothing dropped from the real pinned trees
    for m in doc["models"]:
        assert all(HEX64.fullmatch(f["sha256"]) for f in every_file(m)), m["id"]
        if m["source"] == "strata":
            assert HEX40.fullmatch(m["revision"]), m["id"]
            assert m["mmproj"] is None or HEX40.fullmatch(m["mmproj"]["revision"])
            for f in every_file(m):
                assert type(f["size"]) is int and f["size"] > 0, (m["id"], f["path"])
        elif m["family"] in ("deepseek", "minimax"):   # Maya's DeepSeek and MiniMax manifests pin a commit and every size
            assert HEX40.fullmatch(m["revision"]) and m["mmproj"] is None, m["id"]
            assert all(type(f["size"]) is int and f["size"] > 0 for f in m["files"]), m["id"]
        else:
            assert m["revision"] == "main" and m["mmproj"]["revision"] == "main"
            assert all(f["size"] is None for f in every_file(m))


def test_ids_unique_well_formed_and_sorted(generated):
    doc, _ = generated
    ids = [m["id"] for m in doc["models"]]
    assert len(ids) == len(set(ids))
    assert all(ID.fullmatch(i) for i in ids)
    assert ids == sorted(ids)


def test_schema_shape_and_provenance(generated):
    doc, _ = generated
    assert list(doc) == ["schema_version", "generated_from", "models"]
    assert doc["schema_version"] == 1
    assert doc["generated_from"] == {"upstream": "v0.2.4.2", "maya": "v1.1.27"}
    for m in doc["models"]:
        assert set(m) == KEYS, m["id"]
        assert m["source"] in ("strata", "maya") and m["engine"] in ("qwen", "glm53", "deepseek", "minimax")
        assert m["prepare"] in ("iq_pack", "none", "maya_pack", "unsupported")
        assert isinstance(m["pack_args"], list) and all(a.startswith("--") for a in m["pack_args"])
        assert m["about"] and m["about"].isascii() and m["name"].isascii()
        for f in m["files"]:
            assert list(f) == ["path", "size", "sha256"]


def test_family_rules_follow_upstream_setup(generated):
    doc, _ = generated
    models = by_id(doc)
    q4 = models["qwen3.8-flash-next-unsloth-ud-q4_k_xl"]
    assert (q4["repo"], q4["revision"]) == ("unsloth/Qwen3.8-Flash-Next-GGUF",
                                            "38bb39ee97821de2c9009abb7e93950eec396e66")
    assert [f["path"] for f in q4["files"]] == [
        f"UD-Q4_K_XL/Qwen3.8-Flash-Next-UD-Q4_K_XL-0000{i}-of-00004.gguf" for i in range(1, 5)]
    assert q4["pack_args"] == ["--compat-bf16"] and q4["mmproj"]["repo"] == ISTA
    assert len(models["qwen3.8-flash-next-unsloth-ud-iq4_xs"]["files"]) == 3
    assert models["qwen3.8-flash-next-unsloth-q8_0"]["files"][0]["path"] == \
        "Q8_0/Qwen3.8-Flash-Next-Q8_0-00001-of-00006.gguf"
    gyro = models["qwen3.8-flash-next-gyro-gyro-s"]
    assert gyro["files"] == [{"path": "Qwen3.8-Flash-Next-Gyro-S-TQ1_0.gguf", "size": 58487015808,
                              "sha256": "c6ea1b08ca312f99095b27588ba35976b53f948268e7f91e7761926aa01d9c87"}]
    assert gyro["pack_args"] == ["--compat-bf16"] and gyro["prepare"] == "iq_pack"
    coder = models["qwen3.8-flash-next-coder-iq1_m"]
    assert coder["expert_profile"] == "expert-profile-coder.bin"
    assert coder["files"][0]["path"] == "IQ1_M/Qwen3.8-Flash-Next-GSQ-RCO-IQ1_M-00001-of-00002.gguf"
    swift = models["swift-1.5-iq3_s"]
    assert swift["files"][0]["path"] == "Swift-Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf"
    assert swift["mmproj"]["path"] == "mmproj-Swift-Qwen3.8-Flash-Next-BF16.gguf"
    q36 = models["qwen3.6-35b-a3b-ud-q4_k_xl"]
    assert (q36["prepare"], q36["pack_args"], q36["expert_profile"]) == ("none", [], None)
    assert q36["files"] == [{"path": "Qwen3.6-35B-A3B-UD-Q4_K_XL.gguf", "size": 22853663008,
                             "sha256": "55983c5a75a1ab969824077b3bb3de4146e82a9234072b48ad4e8f92ad3fe9f1"}]
    assert (q36["mmproj"]["repo"], q36["mmproj"]["path"]) == ("unsloth/Qwen3.6-35B-A3B-MTP-GGUF", "mmproj-F16.gguf")
    for m in doc["models"]:
        if m["expert_profile"] is not None:
            assert (REPO / "upstream" / "data" / m["expert_profile"]).is_file(), m["id"]


def test_maya_entries_pin_sha256_at_main(generated):
    doc, _ = generated
    models = by_id(doc)
    assert {m["id"] for m in doc["models"] if m["source"] == "maya" and m["family"] == "glm53"} == {
        "glm-5.3-flash-maya-s-v2-iq2_xxs", "glm-5.3-flash-maya-s24", "glm-5.3-flash-maya-m",
        "glm-5.3-flash-maya-m-derisked", "glm-5.3-flash-maya-l"}
    s = models["glm-5.3-flash-maya-s-v2-iq2_xxs"]
    # Maya's setup step packs a GLM download (strata-prepare download --maya, owner 2026-10-27): no longer download only
    assert (s["family"], s["engine"], s["prepare"], s["repo"]) == ("glm53", "glm53", "maya_pack", MAYA_REPO)
    assert s["name"] == "GLM-5.3-Flash Maya-S-v2-IQ2_XXS"
    # shard 1 was published twice; the newest (listed first, "glm5-next", v1.0.28) is what main serves
    assert s["files"][0] == {"path": "Maya-S-v2-IQ2_XXS/GLM-5.3-Flash-Maya-S-v2-IQ2_XXS-00001-of-00003.gguf",
                             "size": None,
                             "sha256": "e56ef50efd71d81ddf6249e08bd2ef5af96ed348c2b7a004491d46168d0ecaa7"}
    assert len(models["glm-5.3-flash-maya-l"]["files"]) == 4
    derisked = models["glm-5.3-flash-maya-m-derisked"]
    assert derisked["repo"] == "peasantsmith/GLM-5.3-Flash-Maya-M-Derisked-IQ2_S-GGUF"
    assert derisked["files"][0]["path"] == "GLM-5.3-Flash-Maya-M-Derisked-IQ2_S-00001-of-00003.gguf"
    assert derisked["mmproj"] == {"path": "vision/mmproj-GLM-5.3-Flash-F16.gguf", "size": None,
                                  "sha256": "3627575df16bd152db0f3fd7e488d270b33f3a9e6c7fa3b1b8ac381faafde882",
                                  "repo": MAYA_REPO, "revision": "main"}
    assert all((m["pack_args"], m["expert_profile"]) == ([], None) for m in doc["models"] if m["source"] == "maya")


def test_maya_deepseek_entries_come_from_its_gguf_manifest(generated):
    """DeepSeek-V4.1 (2026-10-27): maya.py's DeepSeek GGUF manifest (tools/ds4/download_gguf.json) - unsloth's two quants
    and Maya's own, every file pinned by its size and sha256 at the manifest's revision, packed by Maya's DeepSeek step
    (strata-prepare download --maya). Maya-M lists no files and is dropped; the drafter and the vision tower are not
    GGUF files (O4)."""
    doc, notes = generated
    models = by_id(doc)
    assert {m["id"] for m in doc["models"] if m["family"] == "deepseek"} == {
        "deepseek-v4.1-flash-ud-q8_k_xl", "deepseek-v4.1-flash-ud-q4_k_xl", "deepseek-v4.1-flash-maya-s",
        "deepseek-v4.1-flash-maya-xs"}
    q8 = models["deepseek-v4.1-flash-ud-q8_k_xl"]
    assert (q8["source"], q8["engine"], q8["prepare"], q8["repo"], q8["revision"]) == (
        "maya", "deepseek", "maya_pack", "unsloth/DeepSeek-V4.1-Flash-1017-GGUF", "c991b28e7c109eed10a55075ddb46f830b5b3059")
    assert q8["name"] == "DeepSeek-V4.1-Flash-1017 UD-Q8_K_XL" and len(q8["files"]) == 9
    assert q8["files"][0] == {"path": "UD-Q8_K_XL/DeepSeek-V4.1-Flash-1017-UD-Q8_K_XL-00001-of-00009.gguf", "size": 5780589,
                              "sha256": "cc15d470afcb030558bbc4d8e2ccd840182e2e95de1a95198138efe63a5fd99f"}
    s = models["deepseek-v4.1-flash-maya-s"]
    assert (s["repo"], s["revision"], len(s["files"])) == (
        "peasantsmith/DeepSeek-V4.1-Flash-1017-Maya-GGUF", "c273fcbdda76a1db074c2f414cb70edbfca2f2ef", 6)
    assert s["files"][0] == {"path": "Maya-S/DeepSeek-V4.1-Flash-1017-Maya-S-IQ2_XXS-00001-of-00006.gguf",
                             "size": 47887697312, "sha256": "1f3437f57042e69f94c141ffbcba0d5610dab63feb07244fb468be4940d6763f"}
    assert len(models["deepseek-v4.1-flash-maya-xs"]["files"]) == 5
    assert all(f["path"].endswith(".gguf") for m in doc["models"] if m["family"] == "deepseek" for f in m["files"])
    assert all((m["pack_args"], m["expert_profile"], m["mmproj"]) == ([], None, None)
               for m in doc["models"] if m["family"] == "deepseek")
    assert any("deepseek Maya-M" in n and "no files" in n for n in notes)


def test_maya_minimax_entries_come_from_its_gguf_manifest(generated):
    """MiniMax-M3.1-Flash (owner 2026-10-28, under the MiniMax Community License): Maya v1.1.30's MiniMax GGUF manifest
    (tools/mm3/download_gguf.json, vendored byte for byte in engine/third_party/maya) - Maya's two quants, every file
    pinned by its size and sha256 at the manifest's revision, the indexers side file among them, packed by Maya's MiniMax
    step (strata-prepare download --maya)."""
    doc, _ = generated
    models = by_id(doc)
    assert {m["id"] for m in doc["models"] if m["family"] == "minimax"} == {"minimax-m3.1-flash-maya-xs", "minimax-m3.1-flash-maya-s"}
    xs = models["minimax-m3.1-flash-maya-xs"]
    assert (xs["source"], xs["engine"], xs["prepare"], xs["repo"], xs["revision"]) == (
        "maya", "minimax", "maya_pack", "peasantsmith/MiniMax-M3.1-Flash-Maya-GGUF", "4b40cc791e2cdf59c63822efcf097f59e46ed59b")
    assert xs["name"] == "MiniMax-M3.1-Flash Maya-XS" and [f["path"] for f in xs["files"]] == [
        "Maya-XS/MiniMax-M3.1-Flash-Maya-XS-IQ1_S-00001-of-00002.gguf", "Maya-XS/MiniMax-M3.1-Flash-Maya-XS-IQ1_S-00002-of-00002.gguf",
        "Maya-XS/MiniMax-M3.1-Flash-Maya-XS-maya-indexers.gguf"]
    assert xs["files"][0]["size"] == 47846435136 and xs["files"][0]["sha256"].startswith("a7ce35ed3a9059ea")
    assert len(models["minimax-m3.1-flash-maya-s"]["files"]) == 4
    assert all((m["pack_args"], m["expert_profile"], m["mmproj"]) == ([], None, None)
               for m in doc["models"] if m["family"] == "minimax")


def test_render_is_ascii_lf_two_space_indent(generated):
    doc, _ = generated
    data = dc.render(doc)
    data.decode("ascii")
    assert b"\r" not in data and data.endswith(b"}\n") and not data.endswith(b"\n\n")
    assert data == (json.dumps(json.loads(data), indent=2) + "\n").encode("ascii")
    assert dc.render(dc.build(dc.UPSTREAM, dc.MAYA, dc.UPSTREAM_VERSION)[0]) == data


def run_tool(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", str(SCRIPT), *args], cwd=REPO, capture_output=True, text=True,
                          check=False)


def test_committed_catalog_is_current():
    result = run_tool("--check")
    assert result.returncode == 0, result.stderr


def test_check_fails_on_a_tampered_or_missing_copy(tmp_path):
    tampered = tmp_path / "downloads.json"
    tampered.write_bytes(COMMITTED.read_bytes().replace(b"4c1eb2ce", b"4c1eb2cf", 1))
    result = run_tool("--check", "--out", str(tampered))
    assert result.returncode == 1 and "differs" in result.stderr
    result = run_tool("--check", "--out", str(tmp_path / "absent.json"))
    assert result.returncode == 1


def test_write_mode_writes_the_committed_bytes(tmp_path):
    out = tmp_path / "downloads.json"
    result = run_tool("--out", str(out))
    assert result.returncode == 0, result.stderr
    assert out.read_bytes() == COMMITTED.read_bytes()


# ---------------------------------------------------------------- copies of the pinned files with one fact changed
def copies(tmp_path: Path, setup_edit=None, maya_edit=None) -> tuple[Path, Path]:
    up, maya = tmp_path / "upstream", tmp_path / "maya"
    up.mkdir()
    maya.mkdir()
    text = (dc.UPSTREAM / "setup.py").read_text(encoding="utf-8")
    (up / "setup.py").write_text(setup_edit(text) if setup_edit else text, encoding="utf-8")
    text = (dc.MAYA / "maya.py").read_text(encoding="utf-8")
    (maya / "maya.py").write_text(maya_edit(text) if maya_edit else text, encoding="utf-8")
    shutil.copyfile(dc.MAYA / "CHANGELOG.md", maya / "CHANGELOG.md")
    (maya / "tools" / "ds4").mkdir(parents=True)   # (the DeepSeek GGUF manifest, read as data)
    shutil.copyfile(dc.MAYA / "tools" / "ds4" / "download_gguf.json", maya / "tools" / "ds4" / "download_gguf.json")
    return up, maya


def replace_once(old: str, new: str):
    def edit(text: str) -> str:
        assert text.count(old) >= 1, old
        return text.replace(old, new, 1)
    return edit


def test_reads_the_files_without_running_them(tmp_path):
    sentinel = tmp_path / "ran.txt"
    side_effect = f"open({str(sentinel)!r}, 'w').write('ran')\nraise SystemExit(7)\n"
    up, maya = copies(tmp_path, lambda t: side_effect + t, lambda t: side_effect + t)
    doc, _ = dc.build(up, maya, dc.UPSTREAM_VERSION)
    assert not sentinel.exists()
    assert dc.render(doc) == COMMITTED.read_bytes()


def test_a_variant_without_a_sha256_is_dropped_and_reported(tmp_path):
    up, maya = copies(
        tmp_path,
        replace_once('"4c1eb2ceb4915e1192f4f386021897bde56a97f40a0bb78bb86465e0f7d2aca3"', '"4c1eb2ce"'),
        replace_once('"1af62cd72de6460c85369d988abbb790d6ec3a97507df51faf9bf32aa2b600d0"', '""'))
    doc, notes = dc.build(up, maya, dc.UPSTREAM_VERSION)
    ids = set(by_id(doc))
    assert "qwen3.8-flash-next-iq3_s" not in ids and "glm-5.3-flash-maya-l" not in ids
    assert "qwen3.8-flash-next-iq2_xs" in ids and "glm-5.3-flash-maya-m" in ids
    dropped = [n for n in notes if "sha256" in n]
    assert len(dropped) == 2
    assert any("IQ3_S" in n for n in dropped) and any("Maya-L" in n for n in dropped)


def test_a_variant_without_a_size_or_commit_is_dropped(tmp_path):
    up, maya = copies(tmp_path, lambda t: replace_once(
        '(22853663008, "55983c5a', '(0, "55983c5a')(replace_once(
            '"agentionai/Qwen3.8-Flash-Next-Gyro-GGUF": "1ada08422e404aca51ab9df665f089f34c419c6b"',
            '"agentionai/Qwen3.8-Flash-Next-Gyro-GGUF": "main"')(t)))
    doc, notes = dc.build(up, maya, dc.UPSTREAM_VERSION)
    ids = set(by_id(doc))
    assert "qwen3.6-35b-a3b-ud-q4_k_xl" not in ids and "qwen3.6-35b-a3b-q8_0" in ids
    assert not any(m["family"] == "gyro" for m in doc["models"])
    assert any("size" in n and "UD-Q4_K_XL" in n for n in notes)
    assert any("revision" in n and "Gyro-S" in n for n in notes)


def test_an_expression_it_cannot_read_statically_is_refused(tmp_path):
    up, maya = copies(tmp_path, replace_once('"download_gb": 66.4,', '"download_gb": float("66.4"),'))
    with pytest.raises(dc.CatalogError, match=r"setup\.py:\d+"):
        dc.build(up, maya, dc.UPSTREAM_VERSION)


def test_a_changed_upstream_rule_is_refused(tmp_path):
    up, maya = copies(tmp_path, replace_once('fam.get("shards", 2))', 'fam.get("shards", 3))'))
    with pytest.raises(dc.CatalogError, match="rule"):
        dc.build(up, maya, dc.UPSTREAM_VERSION)


def test_a_changed_maya_rule_is_refused(tmp_path):
    up, maya = copies(tmp_path, maya_edit=replace_once(
        'path=f"{folder}/{file}" if folder else file)', 'path=f"{folder}-{file}" if folder else file)'))
    with pytest.raises(dc.CatalogError, match="rule"):
        dc.build(up, maya, dc.UPSTREAM_VERSION)


def test_a_table_changed_outside_its_literal_is_refused(tmp_path):
    up, maya = copies(tmp_path, lambda t: t + '\nMODELS["Q2_0"]["about"] = "changed at run time"\n')
    with pytest.raises(dc.CatalogError, match="MODELS"):
        dc.build(up, maya, dc.UPSTREAM_VERSION)
