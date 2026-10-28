"""SP7 M2 Task 6: packaging builders (scripts/release/build_release.sh, Windows staging).

Ruling R-T6-a: the existing ``secure_distribution_check.py --release`` contract
stays byte-identical; D-9's on-top evidence (SHA256SUMS completeness, signed
D-3 envelope verification, the exact ``unsigned: not releasable`` mark) lives
in the build_release.sh gate steps, so Task 5's committed e2e keeps passing.
The Windows makensis RUN is an owner-box step (plan Task 6); this suite proves
the staged tree and the generated installer.nsi, and that an NSIS attempt was
recorded either way.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPTS = REPO / "scripts" / "release"
CHECK = REPO / "scripts" / "secure_distribution_check.py"
LINUX_BUILD = REPO / "secure_frontend" / "out" / "build" / "linux-gcc-release"
WIN_BUILD = REPO / "secure_frontend" / "out" / "build" / "windows-llvm-mingw-cross"
VERSION = "0.0.0-t6"


def _bundle_files(bundle: Path) -> set[str]:
    return {p.relative_to(bundle).as_posix() for p in bundle.rglob("*") if p.is_file()}


def _sums_complete(bundle: Path) -> bool:
    sums = (bundle / "SHA256SUMS").read_text(encoding="utf-8").splitlines()
    listed = {line.split()[-1] for line in sums if line.strip()}
    return listed == _bundle_files(bundle) - {"SHA256SUMS"}


def _linux_bundle(tmp_path: Path, extra_env: dict[str, str] | None = None) -> tuple[Path, str]:
    env = {k: v for k, v in os.environ.items() if k != "STRATA_SIGNING_KEY"}
    if extra_env:
        env.update(extra_env)
    run = subprocess.run(["bash", str(SCRIPTS / "build_release.sh"),
                          "--version", VERSION, "--skip-build",
                          "--out", str(tmp_path / "out")],
                         capture_output=True, text=True, env=env)
    assert run.returncode == 0, run.stdout + run.stderr
    return tmp_path / "out" / f"strata-{VERSION}-linux-x86_64", run.stdout


def test_release_scripts_exist() -> None:
    for name in ("build_release.sh", "stage_windows.sh", "make_update_manifest.py"):
        path = SCRIPTS / name
        assert path.is_file(), f"scripts/release/{name} exists"
    for name in ("build_release.sh", "stage_windows.sh"):
        assert os.access(SCRIPTS / name, os.X_OK), f"{name} is executable"


def test_linux_bundle_content_and_evidence(tmp_path: Path) -> None:
    bundle, stdout = _linux_bundle(tmp_path)
    assert (bundle / "bin" / "strata").is_file(), "bin/strata shipped"
    assert (bundle / "bin" / "strata-secure-server").is_file(), "bin/strata-secure-server shipped"
    assert (bundle / "VERSION").read_text(encoding="utf-8").strip() == VERSION, "VERSION file"
    assert (bundle / "secure-release-manifest.json").is_file(), "profile manifest generated"
    assert (bundle / "sbom.spdx.json").is_file(), "SBOM generated"
    check = subprocess.run([sys.executable, str(CHECK), "--package-dir", str(bundle),
                            "--release"], capture_output=True, text=True)
    assert check.returncode == 0, f"the judge must pass the real bundle: {check.stdout}{check.stderr}"
    assert "unsigned: not releasable" in stdout, "an unsigned build marks itself not releasable"
    assert not (bundle / "manifest.json.sig").exists(), "no signature without a key"


def test_linux_bundle_update_manifest_matches_the_d3_schema(tmp_path: Path) -> None:
    bundle, _ = _linux_bundle(tmp_path)
    doc = json.loads((bundle / "manifest.json").read_text(encoding="utf-8"))
    assert set(doc) == {"format", "version", "channel", "components"}, "D-3 top-level keys exactly"
    assert doc["format"] == "strata-release-v1", "D-3 format"
    assert doc["version"] == VERSION, "D-3 version"
    assert doc["channel"] == "stable", "channel"
    names = [comp["name"] for comp in doc["components"]]
    assert len(names) == len(set(names)), "component names unique (the C++ schema insists)"
    assert set(names) <= {"frontend", "engine", "cuda-runtime"}, "closed set"
    frontend = next(comp for comp in doc["components"] if comp["name"] == "frontend")
    assert frontend["file"] == "strata", "the frontend component names a plain file"
    payload = (bundle / "bin" / frontend["file"]).read_bytes()
    assert frontend["size"] == len(payload), "size matches the shipped bytes"
    assert frontend["sha256"] == hashlib.sha256(payload).hexdigest(), "sha256 matches the shipped bytes"
    assert frontend["notes_url"].startswith("https://"), "notes_url is https"


def test_linux_bundle_sha256sums_complete(tmp_path: Path) -> None:
    bundle, _ = _linux_bundle(tmp_path)
    assert _sums_complete(bundle), "SHA256SUMS lists every bundled file but itself"
    run = subprocess.run(["sha256sum", "-c", "--quiet", "SHA256SUMS"], cwd=bundle,
                         capture_output=True, text=True)
    assert run.returncode == 0, f"digests re-verify in place: {run.stdout}{run.stderr}"


def test_linux_bundle_artifacts(tmp_path: Path) -> None:
    out = tmp_path / "out"
    env = {k: v for k, v in os.environ.items() if k != "STRATA_SIGNING_KEY"}
    run = subprocess.run(["bash", str(SCRIPTS / "build_release.sh"),
                          "--version", VERSION, "--skip-build", "--out", str(out)],
                         capture_output=True, text=True, env=env)
    assert run.returncode == 0, run.stdout + run.stderr
    tarball = out / f"strata-{VERSION}-linux-x86_64.tar.gz"
    assert tarball.is_file(), "tarball is the user path"
    listing = subprocess.run(["tar", "-tzf", str(tarball)], capture_output=True, text=True).stdout
    assert f"strata-{VERSION}-linux-x86_64/bin/strata" in listing, "tarball carries the bundle dir"
    debs = list(out.glob("*.deb"))
    if shutil.which("dpkg-deb"):
        assert debs, "dpkg-deb present -> .deb built"
        contents = subprocess.run(["dpkg-deb", "-c", str(debs[0])],
                                  capture_output=True, text=True).stdout
        assert "./usr/bin/strata" in contents and "./usr/bin/strata-secure-server" in contents, \
            "the admin path installs both binaries"
    else:
        assert not debs, "no dpkg-deb -> no .deb"


def test_signed_run_produces_a_verifiable_envelope(tmp_path: Path) -> None:
    vectors = json.loads((REPO / "secure_frontend" / "tests" / "fixtures" / "release" /
                          "rfc8032-vectors.json").read_text(encoding="utf-8"))
    seed = vectors["1"]["sk"][:64]
    key_file = tmp_path / "signing.key"
    keygen = subprocess.run([sys.executable, str(SCRIPTS / "keygen.py"),
                             "--seed-hex", seed, "--out-key", str(key_file)],
                            capture_output=True, text=True, check=True)
    public = None
    for line in keygen.stdout.splitlines():
        if "pinned public key:" in line:
            public = line.split()[-1]
    assert public, "keygen prints the pinned public key"
    bundle, stdout = _linux_bundle(tmp_path, {"STRATA_SIGNING_KEY": str(key_file)})
    assert "unsigned: not releasable" not in stdout, "a key lifts the not-releasable mark"
    envelope = bundle / "manifest.json.sig"
    assert envelope.is_file(), "the signed envelope is attached"
    verify = subprocess.run([sys.executable, str(SCRIPTS / "sign.py"), "--verify",
                             "--key-hex", public, "--envelope", str(envelope)],
                            capture_output=True, text=True)
    assert verify.returncode == 0 and "verifies" in verify.stdout, verify.stdout + verify.stderr
    assert envelope.read_bytes().startswith((bundle / "manifest.json").read_bytes()), \
        "the envelope signs exactly the manifest bytes"
    assert "SHA256SUMS" in _bundle_files(bundle), "signed runs still carry the sums"


def test_windows_staging_tree_installer_and_nsis_status(tmp_path: Path) -> None:
    build = tmp_path / "winbuild"
    build.mkdir()
    for name in ("strata.exe", "strata-secure-server.exe", "strata_widget_tests.exe",
                 "secure_frontend_non_elevated_run.exe"):
        (build / name).write_bytes(b"MZ\x90 stub")
    run = subprocess.run(["bash", str(SCRIPTS / "stage_windows.sh"),
                          "--version", VERSION, "--build-dir", str(build),
                          "--out", str(tmp_path / "out")],
                         capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    bundle = tmp_path / "out" / f"strata-{VERSION}-windows-x86_64"
    assert (bundle / "bin" / "strata.exe").is_file(), "strata.exe staged"
    assert (bundle / "bin" / "strata-secure-server.exe").is_file(), "strata-secure-server.exe staged"
    names = _bundle_files(bundle)
    assert not [n for n in names if n.endswith(".py")], "no Python in a native staging tree"
    assert not [n for n in names if "_tests.exe" in n or "non_elevated" in n], \
        "test and helper executables never stage"
    assert (bundle / "VERSION").read_text(encoding="utf-8").strip() == VERSION, "VERSION file"
    assert (bundle / "secure-release-manifest.json").is_file(), "profile manifest generated"
    assert (bundle / "sbom.spdx.json").is_file(), "SBOM generated"
    nsi = (bundle / "installer.nsi").read_text(encoding="utf-8")
    assert "strata.exe" in nsi and "strata-secure-server.exe" in nsi, "installer carries both binaries"
    assert "RequestExecutionLevel user" in nsi, "no elevation: user level, per the spec's non-goals"
    assert _sums_complete(bundle), "SHA256SUMS lists every staged file but itself"
    status = (bundle / "NSIS_STATUS.txt").read_text(encoding="utf-8")
    assert "makensis:" in status, "the NSIS attempt is recorded either way"
    check = subprocess.run([sys.executable, str(CHECK), "--package-dir", str(bundle),
                            "--release"], capture_output=True, text=True)
    assert check.returncode == 0, f"the judge passes the staged tree: {check.stdout}{check.stderr}"


def test_staging_refuses_a_test_hooks_tree(tmp_path: Path) -> None:
    """Negative control: the windows-llvm-mingw-cross gate preset carries test
    hooks; staging from it must be refused by the distribution judge."""
    assert WIN_BUILD.is_dir(), "the cross gate build tree exists (gate battery ran first)"
    run = subprocess.run(["bash", str(SCRIPTS / "stage_windows.sh"),
                          "--version", "0.0.0-hooks", "--build-dir", str(WIN_BUILD),
                          "--out", str(tmp_path / "out")],
                         capture_output=True, text=True)
    assert run.returncode != 0, "hooks binaries must fail the judge"
    assert "SECURE_FRONTEND_TEST_HOOKS" in run.stdout + run.stderr, "and say why"
