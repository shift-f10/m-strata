"""SP7 M2 Task 5: release manifest + SBOM generators (scripts/release/).

The generators must satisfy the EXISTING check-script contract:
``scripts/secure_distribution_check.py --release`` is the judge and is not
modified here.  Provenance discipline (R-T0-b): no version, commit, or tag
is hand-transcribed into this suite — submodule pins are read live from
``git submodule status``, and the nine privacy keys are scraped from the
check script itself.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPTS = REPO / "scripts" / "release"
CHECK = REPO / "scripts" / "secure_distribution_check.py"
CREATED = "2026-01-01T00:00:00Z"
VERSION = "9.9.9-test"


def _git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(REPO), *args],
                          check=True, capture_output=True, text=True).stdout


def _submodule_pin(path: str) -> tuple[str, str]:
    """(commit, description) for one submodule, read live from git."""
    line = _git("submodule", "status", "--", path).strip().splitlines()[0].strip()
    commit = line.split()[0].lstrip("+-u")
    assert re.fullmatch(r"[0-9a-f]{40}", commit), f"live submodule commit: {line}"
    match = re.search(r"\(([^)]*)\)", line)
    return commit, (match.group(1) if match else "")


def _stage(tmp_path: Path) -> Path:
    """A staged package: stub binaries + both generated evidence files."""
    stage = tmp_path / "stage"
    (stage / "bin").mkdir(parents=True)
    (stage / "bin" / "strata").write_bytes(b"\x7fELF stub")
    (stage / "bin" / "strata-secure-server").write_bytes(b"\x7fELF stub")
    subprocess.run([sys.executable, str(SCRIPTS / "make_manifest.py"),
                    "--version", VERSION, "--created-utc", CREATED,
                    "--out", str(stage / "secure-release-manifest.json")],
                   check=True, capture_output=True)
    subprocess.run([sys.executable, str(SCRIPTS / "sbom.py"),
                    "--version", VERSION, "--created-utc", CREATED,
                    "--out", str(stage / "sbom.spdx.json")],
                   check=True, capture_output=True)
    return stage


def test_generator_scripts_exist() -> None:
    assert (SCRIPTS / "make_manifest.py").is_file(), "scripts/release/make_manifest.py exists"
    assert (SCRIPTS / "sbom.py").is_file(), "scripts/release/sbom.py exists"


def test_generators_pass_the_check_script_e2e(tmp_path: Path) -> None:
    stage = _stage(tmp_path)
    run = subprocess.run([sys.executable, str(CHECK), "--package-dir", str(stage),
                          "--release"], capture_output=True, text=True)
    assert run.returncode == 0, f"check --release must pass: {run.stdout}{run.stderr}"


def test_check_script_still_rejects_a_tampered_generated_manifest(tmp_path: Path) -> None:
    """Negative control: the e2e pass above is meaningful, not vacuous."""
    stage = _stage(tmp_path)
    manifest_path = stage / "secure-release-manifest.json"
    doc = json.loads(manifest_path.read_text(encoding="utf-8"))
    doc["privacy"]["analytics"] = True
    manifest_path.write_text(json.dumps(doc), encoding="utf-8")
    run = subprocess.run([sys.executable, str(CHECK), "--package-dir", str(stage),
                          "--release"], capture_output=True, text=True)
    assert run.returncode != 0, "a flipped privacy flag must fail the gate"
    assert "analytics" in run.stdout + run.stderr


def test_manifest_fields_match_the_check_script_contract(tmp_path: Path) -> None:
    stage = _stage(tmp_path)
    doc = json.loads((stage / "secure-release-manifest.json").read_text(encoding="utf-8"))
    check_src = CHECK.read_text(encoding="utf-8")
    block = re.search(r"for key in \(([^)]*)\):", check_src, re.S).group(1)
    judge_keys = set(re.findall(r'"([a-z_]+)"', block))
    assert judge_keys, "scraped the judge's privacy keys"
    assert set(doc["privacy"]) == judge_keys, "privacy keys match the check script"
    assert all(value is False for value in doc["privacy"].values()), "every privacy claim is false"
    assert doc["profile"] == "secure-native", "profile"
    assert doc["network_default"] == "offline", "network_default"
    assert doc["legacy_components"] == [], "legacy_components"
    assert doc["version"] == VERSION, "version comes from the caller"
    assert doc["created_utc"] == CREATED, "created_utc is pinnable for reproducible builds"
    assert isinstance(doc["components"], list) and doc["components"], "components non-empty"
    names = {comp["name"] for comp in doc["components"]}
    assert names == {"strata", "strata-secure-server"}, "exactly the shipped product binaries"
    for comp in doc["components"]:
        assert set(comp) == {"name", "version", "source"}, "documented component schema"
        assert comp["version"] == VERSION and comp["source"], "component provenance is filled in"


def test_sbom_lists_both_submodule_pins_with_licenses(tmp_path: Path) -> None:
    stage = _stage(tmp_path)
    doc = json.loads((stage / "sbom.spdx.json").read_text(encoding="utf-8"))
    assert doc["spdxVersion"].startswith("SPDX-2."), "an SPDX 2.x document"
    assert doc["SPDXID"] == "SPDXRef-DOCUMENT", "the document root"
    assert doc["documentNamespace"], "a document namespace"
    assert doc["creationInfo"]["created"] == CREATED, "creation info is pinnable"
    packages = doc["packages"]
    assert packages, "the SBOM lists packages"
    for path in ("upstream", "engines/maya-ref"):
        commit, tag = _submodule_pin(path)
        matches = [pkg for pkg in packages
                   if commit in json.dumps(pkg) and tag in json.dumps(pkg)]
        assert len(matches) == 1, f"exactly one package carries the live {path} pin {tag} @ {commit}"
        pkg = matches[0]
        assert pkg["licenseConcluded"] == "MIT", f"{path}: license concluded"
        assert pkg["licenseDeclared"] == "MIT", f"{path}: license declared"
        assert "NOASSERTION" != pkg["downloadLocation"], f"{path}: a real download location"
    frontend = [pkg for pkg in packages if VERSION in json.dumps(pkg) and "upstream" not in pkg["SPDXID"]]
    assert frontend, "the frontend build is listed with its version"
    described = {rel["relatedSpdxElement"] for rel in doc["relationships"]
                 if rel["spdxElementId"] == "SPDXRef-DOCUMENT"
                 and rel["relationshipType"] == "DESCRIBES"}
    assert described == {pkg["SPDXID"] for pkg in packages}, "the document describes every package"


def test_generators_are_byte_reproducible(tmp_path: Path) -> None:
    first = _stage(tmp_path / "a")
    second = _stage(tmp_path / "b")
    assert (first / "secure-release-manifest.json").read_bytes() == \
        (second / "secure-release-manifest.json").read_bytes(), "identical inputs, identical manifest bytes"
    assert (first / "sbom.spdx.json").read_bytes() == \
        (second / "sbom.spdx.json").read_bytes(), "identical inputs, identical SBOM bytes"
