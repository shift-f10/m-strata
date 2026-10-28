#!/usr/bin/env python3
"""Fail closed when a candidate is not a secure-native Strata distribution.

This is a packaging gate, not a malware scanner.  It checks the selected
staging directory; it intentionally does *not* scan this source checkout,
which holds upstream Strata as the git submodule `upstream/` for provenance
(and Project Maya as `engines/maya-ref/`, SP5 M2 D-10).  Any staged path with
an `upstream` or `maya-ref` component is rejected, and the quarantined
names below are matched at any depth, not only at the staging root.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any


# These paths are deliberate quarantine boundaries, documented in
# docs/security/privacy-migration.md.  Match whole path components (at any
# depth) rather than a substring so an unrelated README cannot cause a false
# positive.
FORBIDDEN_PATHS = {
    "start-here.bat",
    "setup.py",
    "update.bat",
    "update.sh",
    "docker-entrypoint.sh",
    "requirements.txt",
    "docs/mcp_server.md",
}
# apps/host (Node server: Cloudflare quick tunnel, LAN share) and apps/desktop
# (its Tauri shell) are not Python, so the .py rule below never catches them.
FORBIDDEN_DIRECTORIES = {"serve", "sycl/serve", ".strata-mcp", "apps/host", "apps/desktop"}
FORBIDDEN_PYTHON_NAMES = {
    "server.py",
    "mcp.py",
    "telemetry.py",
    "prometheus.py",
}
MANIFEST_NAME = "secure-release-manifest.json"
SBOM_NAME = "sbom.spdx.json"
# SP4 §10.5: hooks builds embed this marker (built from two parts so this file
# and the C++ tests that search for it never contain it whole).
HOOK_MARKER = b"SECURE_FRONTEND_TEST_" + b"HOOKS_COMPILED_IN"


def normal_relative(path: Path, root: Path) -> str:
    """Return a portable, lower-case relative path or fail for an escape."""
    try:
        relative = path.relative_to(root)
    except ValueError as exc:
        raise ValueError(f"candidate entry escapes package root: {path}") from exc
    return relative.as_posix().lower()


def _contains_components(parts: list[str], sequence: list[str]) -> bool:
    """True when `sequence` occurs as consecutive whole components of `parts`."""
    width = len(sequence)
    return any(parts[i:i + width] == sequence for i in range(len(parts) - width + 1))


def forbidden_reason(relative: str) -> str | None:
    parts = relative.split("/")
    if "upstream" in parts:
        return "upstream source tree (the git submodule upstream/) is never part of a secure-native package"
    if "maya-ref" in parts:
        return "vendored Project Maya reference (engines/maya-ref/) is never part of a secure-native package (docs/PROVENANCE.md)"
    if any(parts[-len(p.split("/")):] == p.split("/") for p in FORBIDDEN_PATHS):
        return "quarantined legacy installer, updater, dependency bootstrap, or MCP documentation"
    if any(_contains_components(parts, directory.split("/")) for directory in FORBIDDEN_DIRECTORIES):
        return "quarantined legacy Python service/MCP/telemetry directory"
    if parts[-1] in FORBIDDEN_PYTHON_NAMES:
        return "quarantined legacy Python server, MCP, telemetry, or metrics module"
    # A secure-native artifact must not silently bring an arbitrary Python
    # runtime back into the service path.  Python may only be shipped after a
    # future review adds a specific allowlist and regression coverage.
    if parts[-1].endswith(".py"):
        return "Python source is not permitted in the secure-native package"
    return None


def require_bool(mapping: dict[str, Any], key: str, expected: bool, errors: list[str]) -> None:
    if mapping.get(key) is not expected:
        errors.append(f"manifest privacy.{key} must be {str(expected).lower()}")


def check_manifest(package_root: Path, errors: list[str]) -> None:
    manifest_path = package_root / MANIFEST_NAME
    sbom_path = package_root / SBOM_NAME
    if not manifest_path.is_file():
        errors.append(f"missing required release manifest: {MANIFEST_NAME}")
        return
    if not sbom_path.is_file():
        errors.append(f"missing required SBOM: {SBOM_NAME}")
    try:
        parsed = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        errors.append(f"invalid release manifest: {exc}")
        return
    if not isinstance(parsed, dict):
        errors.append("release manifest must be a JSON object")
        return
    if parsed.get("profile") != "secure-native":
        errors.append("manifest profile must be 'secure-native'")
    if parsed.get("network_default") != "offline":
        errors.append("manifest network_default must be 'offline'")
    if parsed.get("legacy_components") != []:
        errors.append("manifest legacy_components must be an empty array")
    privacy = parsed.get("privacy")
    if not isinstance(privacy, dict):
        errors.append("manifest privacy must be an object")
    else:
        for key in (
            "analytics",
            "telemetry",
            "crash_upload",
            "update_check",
            "remote_configuration",
            "advertising",
            "automatic_downloads",
            "mcp",
            "plugins",
        ):
            require_bool(privacy, key, False, errors)
    components = parsed.get("components")
    if not isinstance(components, list) or not components:
        errors.append("manifest components must be a non-empty array")


def inspect(package_root: Path, require_release_evidence: bool) -> list[str]:
    errors: list[str] = []
    if not package_root.exists() or not package_root.is_dir():
        return [f"package directory does not exist or is not a directory: {package_root}"]
    if package_root.is_symlink():
        return [f"package directory must not itself be a symlink: {package_root}"]

    for entry in sorted(package_root.rglob("*")):
        # A package symlink can make the review target ambiguous.  Reject it
        # rather than following it into arbitrary source or host directories.
        relative = normal_relative(entry, package_root)
        if entry.is_symlink():
            errors.append(f"{relative}: symlinks are not allowed in secure distribution staging")
            continue
        reason = forbidden_reason(relative)
        if reason:
            errors.append(f"{relative}: {reason}")
        if entry.is_file():
            with entry.open("rb") as handle:
                if HOOK_MARKER in handle.read():
                    errors.append(f"{relative}: built with SECURE_FRONTEND_TEST_HOOKS; never ship a test-hooks build")

    if require_release_evidence:
        check_manifest(package_root, errors)
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--package-dir",
        required=True,
        type=Path,
        help="staging directory selected for the secure-native distribution",
    )
    parser.add_argument(
        "--release",
        action="store_true",
        help="also require the secure release manifest and SBOM placeholders",
    )
    args = parser.parse_args()
    # Do not resolve here: resolving a package-root symlink would erase the
    # evidence that the selected package is a symlink before `inspect()` can
    # reject it. `absolute()` only makes the operator's selection explicit.
    package_root = args.package_dir.absolute()
    errors = inspect(package_root, args.release)
    if errors:
        print("secure distribution check: FAILED", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print(f"secure distribution check: PASS ({package_root})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
