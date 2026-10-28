"""DL-05 contribution (SP4 spec §2.6, §10.3): every catalog repo, revision and file digest is recorded in PROVENANCE."""

from __future__ import annotations

import json
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HF = re.compile(r"https://huggingface\.co/([A-Za-z0-9._-]+/[A-Za-z0-9._-]+)/resolve/([0-9a-f]{40})/.+")


def check(catalog_dir: Path, provenance: str) -> list[str]:
    errors = []
    for path in sorted(catalog_dir.glob("*/*.json")):
        manifest = json.loads(path.read_text(encoding="ascii"))
        for entry in manifest["files"]:
            if entry["sha256"] not in provenance:
                errors.append(f"{path.relative_to(catalog_dir)}: {entry['path']} sha256 not in PROVENANCE")
            for url in entry["sources"]:
                match = HF.fullmatch(url)
                if match is None:
                    errors.append(f"{path.relative_to(catalog_dir)}: {entry['path']} source is not a pinned Hub URL")
                    continue
                for pinned in match.groups():
                    if pinned not in provenance:
                        errors.append(f"{path.relative_to(catalog_dir)}: {pinned} not in PROVENANCE")
    return errors


def test_catalog_pins_recorded_in_provenance():
    provenance = (REPO / "docs" / "PROVENANCE.md").read_text(encoding="utf-8")
    assert check(REPO / "secure_frontend" / "catalog", provenance) == []


def test_check_reports_missing_pins(tmp_path):
    (tmp_path / "m").mkdir()
    (tmp_path / "m" / "t.json").write_text(json.dumps({"files": [{
        "path": "weights/a.gguf", "sha256": "ab" * 32,
        "sources": ["https://huggingface.co/o/r/resolve/" + "c" * 40 + "/a.gguf"]}]}), encoding="ascii")
    errors = check(tmp_path, "nothing pinned here")
    assert len(errors) == 3 and "sha256 not in PROVENANCE" in errors[0]
    assert check(tmp_path, "ab" * 32 + " o/r " + "c" * 40) == []
