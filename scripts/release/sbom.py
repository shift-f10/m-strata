#!/usr/bin/env python3
"""Generate ``sbom.spdx.json`` (SPDX 2.3 JSON) for a secure release.

Every third-party fact is read live from this repository — submodule pins
from ``git submodule status``, origin URLs from ``.gitmodules``, license
identity and copyright from the vendored ``LICENSE`` files — so no hex,
tag, or copyright line is hand-transcribed and the SBOM can never drift
from the checkout that built the release.  The frontend build is listed
with the release version the caller supplies.
"""
from __future__ import annotations

import argparse
import datetime
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Submodule path -> (package name, SPDX package id).  The pins, URLs, and
# licenses for both are discovered from the repository, not written here.
VENDORED = [
    ("upstream", "Strata (upstream engine core)", "SPDXRef-Package-strata-upstream-engine"),
    ("engines/maya-ref", "project-maya reference engine", "SPDXRef-Package-maya-ref-engine"),
]


def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(REPO), *args],
                          check=True, capture_output=True, text=True).stdout


def submodule_pin(path: str) -> tuple[str, str]:
    """(commit, describe) for a submodule, read live from ``git submodule status``."""
    line = git("submodule", "status", "--", path).strip().splitlines()[0].strip()
    commit = line.split()[0].lstrip("+-u")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise SystemExit(f"sbom: unexpected submodule status line for {path}: {line}")
    match = re.search(r"\(([^)]*)\)", line)
    return commit, (match.group(1) if match else "")


def submodule_url(path: str) -> str:
    """The origin URL of a submodule, read from ``.gitmodules``."""
    listing = git("config", "-f", ".gitmodules", "--get-regexp", r"^submodule\..*\.path$")
    for line in listing.splitlines():
        key, _, value = line.partition(" ")
        if value == path:
            name = key[len("submodule."):-len(".path")]
            return git("config", "-f", ".gitmodules", "--get", f"submodule.{name}.url").strip()
    raise SystemExit(f"sbom: {path} has no path entry in .gitmodules")


def github_purl(url: str, commit: str) -> str | None:
    match = re.match(r"https://github\.com/([^/]+)/([^/]+?)(?:\.git)?$", url)
    if not match:
        return None
    return f"pkg:github/{match.group(1)}/{match.group(2)}@{commit}"


def copyright_lines(rel: str) -> str:
    """All leading copyright lines of a LICENSE file, joined for copyrightText."""
    lines: list[str] = []
    for line in (REPO / rel).read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.lower().startswith("copyright"):
            lines.append(stripped)
        elif lines and stripped:
            break
    return " ; ".join(lines) if lines else "NOASSERTION"


def license_id(rel: str) -> str:
    """MIT is the license every vendored component in this tree declares, AGPL-3.0 (only) this program's own
    (NOTICE.md); assert it from the file's own heading rather than trusting memory."""
    head = (REPO / rel).read_text(encoding="utf-8").splitlines()[0].strip()
    if re.fullmatch(r"MIT License", head, re.IGNORECASE):
        return "MIT"
    if re.fullmatch(r"GNU AFFERO GENERAL PUBLIC LICENSE", head):
        return "AGPL-3.0-only"
    raise SystemExit(f"sbom: unexpected license heading in {rel}: {head}")


def vendored_package(path: str, name: str, spdx_id: str) -> dict:
    commit, describe = submodule_pin(path)
    url = submodule_url(path)
    package = {
        "SPDXID": spdx_id,
        "name": name,
        "versionInfo": f"{describe} ({commit})" if describe else commit,
        "downloadLocation": url,
        "filesAnalyzed": False,
        "licenseConcluded": license_id(f"{path}/LICENSE"),
        "licenseDeclared": license_id(f"{path}/LICENSE"),
        "copyrightText": copyright_lines(f"{path}/LICENSE"),
    }
    purl = github_purl(url, commit)
    if purl:
        package["externalRefs"] = [{
            "referenceCategory": "PACKAGE-MANAGER",
            "referenceType": "purl",
            "referenceLocator": purl,
        }]
    return package


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="release version of the frontend build")
    parser.add_argument("--created-utc",
                        default=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                        help="pin the build timestamp for reproducible output (YYYY-MM-DDTHH:MM:SSZ)")
    parser.add_argument("--out", type=Path, default=None,
                        help="write to this file instead of stdout")
    args = parser.parse_args()

    if not args.version.strip():
        print("sbom: --version must not be empty", file=sys.stderr)
        return 2

    frontend = {
        "SPDXID": "SPDXRef-Package-secure-strata-frontend",
        "name": "strata-secure-frontend",
        "versionInfo": args.version,
        "downloadLocation": "NOASSERTION",
        "filesAnalyzed": False,
        "licenseConcluded": license_id("LICENSE"),
        "licenseDeclared": license_id("LICENSE"),
        # (LICENSE is the AGPL's own text, whose copyright line is the FSF's: this program's is in NOTICE.md)
        "copyrightText": copyright_lines("NOTICE.md"),
    }
    packages = [frontend] + [
        vendored_package(path, name, spdx_id) for path, name, spdx_id in VENDORED
    ]
    document = {
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"strata-secure-frontend-{args.version}",
        "spdxVersion": "SPDX-2.3",
        "dataFormat": "SPDX-2.3",
        "documentNamespace": f"https://secure-strata.invalid/sbom/{args.version}/{args.created_utc}",
        "creationInfo": {
            "created": args.created_utc,
            "creators": ["Organization: Secure Strata"],
            "licenseListVersion": "3.22",
        },
        "comment": ("offline hardened build; no network at build or run; "
                    "submodule pins read live from the building checkout"),
        "packages": packages,
        "relationships": [
            {"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
             "relatedSpdxElement": package["SPDXID"]}
            for package in packages
        ],
    }
    text = json.dumps(document, indent=2, sort_keys=True) + "\n"
    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
