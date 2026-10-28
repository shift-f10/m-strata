#!/usr/bin/env python3
"""Shared generator library for the release manifest and the SBOM (SP7 M2).

The release manifest is the distribution-profile document that
``scripts/secure_distribution_check.py --release`` judges; its field contract
is exactly the one already documented in ``docs/security/deployment.md`` and
enforced by that script — this module is the maker, the check script stays
the judge, and the pytest suite pins the two together.  Nothing here
fabricates version numbers or provenance: the caller supplies them.
"""
from __future__ import annotations

# Every privacy and remote-behaviour claim a release must make explicitly.
# A secure release asserts every one of them is FALSE.  The check script
# names the same nine keys; the unit test pins the two lists together.
PRIVACY_KEYS = [
    "analytics",
    "telemetry",
    "crash_upload",
    "update_check",
    "remote_configuration",
    "advertising",
    "automatic_downloads",
    "mcp",
    "plugins",
]

# Product executables a secure-native release ships. These are exactly the
# non-test, non-tool targets the release preset builds (see the
# ``add_executable`` entries in ``secure_frontend/CMakeLists.txt`` that are
# not test fixtures or probe helpers).
SHIPPED_BINARIES = [
    "strata",
    "strata-secure-server",
]


def release_manifest(version: str, created_utc: str, source: str) -> dict:
    """The distribution-profile manifest document for one release."""
    return {
        "profile": "secure-native",
        "version": version,
        "created_utc": created_utc,
        "network_default": "offline",
        "privacy": {key: False for key in PRIVACY_KEYS},
        "legacy_components": [],
        "components": [
            {"name": name, "version": version, "source": source}
            for name in SHIPPED_BINARIES
        ],
    }
