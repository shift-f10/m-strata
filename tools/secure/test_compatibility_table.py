"""The README's compatibility table and the page's Compatibility sub-tab (owner 2026-10-30: "the same Compatibility
Table we have on the readme ... have a copy/share so users can report their stats"): the page copies the README's
columns, so a shared row pastes into the README's table as it is."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_the_page_copies_the_readmes_columns():
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    header = next(line for line in readme.splitlines() if line.startswith("| OS | CPU | GPU Type |"))
    page = (ROOT / "secure_frontend" / "web" / "app.js").read_text(encoding="utf-8")
    assert f'var COMPAT_HEADER = "{header}";' in page
    # one separator per column
    columns = header.count("|") - 1
    assert "| --- " * columns + "|" in readme
