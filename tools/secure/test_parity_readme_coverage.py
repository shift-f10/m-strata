"""SP5 M1 Task 15 / SP5 M2 Task 2: the README parity blocks mirror the Strata
parity table for the whole generated option inventory (plan Task 15 Step 1) —
every inventory name lands in the block for its disposition, and the mirror is
proven non-vacuous by row count. The GLM (Maya) table is counted here too;
Task 10 extends the README mirror to it."""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ROW = re.compile(r'^\s*\{"([^"]+)", (kTyped|kRejected|kMapped|kOut), ([a-z_]+), "(.*)"\},?$', re.M)
ARRAY = re.compile(r"kUpstream(?:ConfigKeys|EnvKnobs)\[\] = \{(.*?)\};", re.S)
NAME = re.compile(r'"([^"]+)"')
BLOCK = {"kTyped": "parity-typed", "kRejected": "parity-rejected", "kMapped": "parity-mapped",
         "kOut": "parity-out"}
MAYA_BLOCK = {"kTyped": "maya-typed", "kRejected": "maya-rejected", "kMapped": "maya-mapped",
              "kOut": "maya-out"}


def region(readme: str, marker: str) -> str:
    begin, end = f"<!-- {marker}:begin -->", f"<!-- {marker}:end -->"
    start, stop = readme.index(begin), readme.index(end)
    assert start < stop, f"{marker}: end before begin"
    return readme[start + len(begin):stop]


def array_body(text: str, opener: str) -> str:
    start = text.index(opener)
    return text[start:text.index("}};", start)]


def test_parity_readme_coverage() -> None:
    table = (REPO / "secure_frontend/src/parity_table.cpp").read_text(encoding="utf-8")
    rows = ROW.findall(array_body(table, "kRows{{"))
    # SP7 M2 Task 3: +update.source (typed) and +STRATA_RELEASE_PUBKEY_HEX (mapped) -> 123.
    # Upstream bump to v0.2.2: +pool_cores, +STRATA_ENGINE_NICE (inventory) and the extra
    # rule rows +gpu_none, +gpu_roles (read through strata_control/gpu_roles.py), all mapped -> 127.
    # SP5-P3-style refresh at v0.2.4.2 (2026-10-25): +drafting (out), +engine_ready_s (mapped,
    # ready_timeout carries it), +pre +special_ids (mapped, the SP4 tokenizer manifest), +the six
    # STRATA_DS4_*/DSV41 names (out, SP5-P2 §Non-goals) -> 137; the upstream table gains its first
    # kOut rows, mirrored in the new parity-out block.
    assert len(rows) == 137, f"the parity table shape changed ({len(rows)} rows parsed) — update this gate and the README"

    # SP5 M2 Task 10 / SP5-P2 Task 2: the GLM (Maya) table mirrors into its own
    # README blocks, every row in the block for its disposition — the count is the
    # non-vacuity guard for this table too (847 rows after the SP5-P3-style refresh
    # at 4615f346 v1.1.27: was 730 (137 typed, 50 rejected, 73 mapped, 470 out of
    # scope) at v1.1.21; v1.1.27 adds 20 typed D-7 documented GLM knobs, 1 mapped
    # engine-argv flag, and 96 out-of-scope rows (DS4-path growth, undocumented
    # internals, launcher/alias rows); no strata-style floor, it must be whole).
    maya_rows = ROW.findall(array_body(table, "kMayaRows{{"))
    assert len(maya_rows) == 847, f"the GLM table changed ({len(maya_rows)} rows) — extend the README mirror and this gate together"


    inventory = (REPO / "secure_frontend/tests/parity_keys_upstream.hpp").read_text(encoding="utf-8")
    names = [name for body in ARRAY.findall(inventory) for name in NAME.findall(body)]
    assert len(names) == 130, f"the generated inventory changed ({len(names)} names) — re-run the pin greps"
    row_names = {row[0] for row in rows}
    missing_rows = [name for name in names if name not in row_names]
    assert not missing_rows, f"inventory names with no parity row: {missing_rows}"

    readme = (REPO / "secure_frontend/README.md").read_text(encoding="utf-8")
    blocks = {marker: region(readme, marker) for marker in BLOCK.values()}
    found = 0
    missing = []
    for name, disposition, _destination, _citation in rows:
        if re.search(rf'^\| `{re.escape(name)}` \|', blocks[BLOCK[disposition]], re.M):
            found += 1
        else:
            missing.append(f"{name} ({disposition})")
    assert not missing, f"parity rows absent from the README block for their disposition: {missing}"
    assert found >= 80, f"the README mirror counts only {found} rows — too few to be real"

    # The auto-rendered config table (gate: the C++ readme_config_table_matches_schema)
    # must also be present here as a real table; the floor catches a truncated paste.
    assert len(re.findall(r"^\| `", region(readme, "config-keys"), re.M)) >= 100, "the config table looks truncated"

    # SP5 M2 Task 10 / SP5-P2 Task 2: every Maya row lands in the README block for
    # its disposition, including the out_of_scope rows in the maya-out block; all
    # 730 must be found — the mirror is proven non-vacuous by the full count.
    maya_blocks = {disp: region(readme, marker) for disp, marker in MAYA_BLOCK.items()}
    maya_found = 0
    maya_missing = []
    for name, disposition, _destination, _citation in maya_rows:
        if re.search(rf'^\| `{re.escape(name)}` \|', maya_blocks[disposition], re.M):
            maya_found += 1
        else:
            maya_missing.append(f"{name} ({disposition})")
    assert not maya_missing, f"Maya parity rows absent from the README block for their disposition: {maya_missing}"
    assert maya_found == len(maya_rows), f"the Maya README mirror counts {maya_found} of {len(maya_rows)} rows"
