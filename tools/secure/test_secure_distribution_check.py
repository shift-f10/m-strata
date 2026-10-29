"""scripts/secure_distribution_check.py: quarantined content is rejected at any depth.

Hermetic: every case builds a throwaway staging directory under tmp_path.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location(
    "secure_distribution_check", ROOT / "scripts" / "secure_distribution_check.py")
check = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(check)


def stage(tmp_path: Path, *files: str) -> Path:
    root = tmp_path / "stage"
    root.mkdir()
    for relative in files:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"x")
    return root


def flagged(errors: list[str], relative: str) -> bool:
    return any(error.startswith(relative.lower() + ":") for error in errors)


def test_nested_upstream_requirements_is_rejected(tmp_path):
    errors = check.inspect(stage(tmp_path, "upstream/requirements.txt"), False)
    assert flagged(errors, "upstream/requirements.txt"), errors


def test_nested_upstream_serve_asset_is_rejected(tmp_path):
    errors = check.inspect(stage(tmp_path, "upstream/serve/index.html", "upstream/setup.sh"), False)
    assert flagged(errors, "upstream/serve/index.html"), errors
    assert flagged(errors, "upstream/setup.sh"), errors


def test_forbidden_file_below_the_root_is_rejected(tmp_path):
    files = ["vendor/strata/requirements.txt", "vendor/strata/docs/MCP_SERVER.md", "opt/START-HERE.bat"]
    errors = check.inspect(stage(tmp_path, *files), False)
    for relative in files:
        assert flagged(errors, relative), (relative, errors)


def test_forbidden_directory_below_the_root_is_rejected(tmp_path):
    files = ["pkg/serve/index.html", "pkg/sycl/serve/app.js", "pkg/.strata-mcp/state.json"]
    errors = check.inspect(stage(tmp_path, *files), False)
    for relative in files:
        assert flagged(errors, relative), (relative, errors)


def test_upstream_desktop_and_host_apps_are_rejected_when_flattened(tmp_path):
    # v0.2.2 privacy re-review (docs/security/privacy-migration.md): upstream's apps/host Node server
    # (Cloudflare quick tunnel, LAN share) and its apps/desktop Tauri shell are not Python, so only a
    # directory rule catches a copy staged without its upstream/ prefix.
    files = ["pkg/apps/host/server/cloudflare.mjs", "pkg/apps/host/server/local-share.mjs",
             "pkg/apps/desktop/src-tauri/tauri.conf.json"]
    errors = check.inspect(stage(tmp_path, *files), False)
    for relative in files:
        assert flagged(errors, relative), (relative, errors)


def test_apps_lookalikes_pass(tmp_path):
    files = ["share/doc/apps-host-notes.txt", "bin/apps/strata-unified-server"]
    assert check.inspect(stage(tmp_path, *files), False) == []


def test_native_only_stage_passes(tmp_path):
    assert check.inspect(stage(tmp_path, "bin/strata-unified-server"), False) == []


def test_lookalike_names_pass(tmp_path):
    files = ["share/doc/upstream-notes.txt", "bin/requirements.txt.sig", "bin/observe/index.html",
             "share/doc/maya-reference.txt"]
    assert check.inspect(stage(tmp_path, *files), False) == []


def test_staged_maya_ref_tree_is_rejected(tmp_path):
    # SP5 M2 Task 1 (D-10): the vendored Maya reference never ships; the rule
    # must fire on content no other quarantine rule catches (README.md, .md).
    files = ["engines/maya-ref/README.md", "engines/maya-ref/docs/SETTINGS.md",
             "engines/maya-ref/serve/server.py"]
    errors = check.inspect(stage(tmp_path, *files), False)
    for relative in files:
        assert flagged(errors, relative), (relative, errors)


def test_no_cmake_file_references_maya_ref():
    # The Maya submodule is a read-only citation source: no CMake file of ours
    # may configure, build, or otherwise reference it (plan Global Constraints).
    skip_components = {"engines", "upstream", "out", "build", ".git", ".venv-dev",
                       ".toolchains", ".worktrees", ".superpowers"}
    checked = 0
    for path in ROOT.rglob("*"):
        if not path.is_file():
            continue
        if skip_components & set(path.relative_to(ROOT).parts[:-1]):
            continue
        if path.name not in {"CMakeLists.txt", "CMakePresets.json"} and path.suffix != ".cmake":
            continue
        checked += 1
        assert "maya-ref" not in path.read_text(encoding="utf-8"), path
    assert checked >= 3, "the walk must see our CMake files (root, secure_frontend, presets)"


def test_pytest_never_collects_the_engines_tree():
    # engines/maya-ref/ carries a conftest-bearing pytest tree of its own.
    lines = (ROOT / "pytest.ini").read_text(encoding="utf-8").splitlines()
    norecurse = [line for line in lines if line.split("=")[0].strip() == "norecursedirs"]
    assert norecurse, "pytest.ini must pin norecursedirs while engines/ is vendored"
    assert "engines" in norecurse[0].split("=", 1)[1].split(), norecurse[0]


def test_hooks_marker_is_rejected_in_every_mode(tmp_path):
    root = stage(tmp_path, "bin/strata")
    (root / "bin" / "strata").write_bytes(b"\x7fELF..." + check.HOOK_MARKER + b"...")
    assert flagged(check.inspect(root, False), "bin/strata")
    assert flagged(check.inspect(root, True), "bin/strata")


def test_release_build_without_marker_passes_the_marker_scan(tmp_path):
    root = stage(tmp_path, "bin/strata")
    assert not flagged(check.inspect(root, False), "bin/strata")
