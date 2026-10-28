#!/usr/bin/env python3
"""Generate secure_frontend/catalog/downloads.json: one card per GGUF variant upstream Strata and Project Maya offer.

Model downloads plan (docs/superpowers/plans/2026-10-23-model-downloads.md), Task 2; owner rule O4: Hugging Face only,
`.gguf` files only.  The facts come from the pinned references, read with `ast` and never imported or executed:

  upstream/setup.py          MODELS, FAMILIES, OWN_PATH_FAMILIES, REPO_PINS (bytes + SHA-256 per file), HF_REVISIONS
  engines/maya-ref/maya.py   MODELS (SHA-256 per shard at revision "main", no sizes), MODEL_NAME

A table is read only when it is a literal (constants, tuples, lists, dicts, `**` merges, names of other such tables,
`+` of strings and numbers, `*` of numbers, and upstream's `hf("<repo>")` folder helper); anything else is refused with
its line.  How setup.py and maya.py turn those tables into files (shard names, folders, the pack step, the expert
profile, the image encoder) is code, not data: the generator restates those rules and refuses to run when the pinned
source no longer contains them word for word (RULES below), so an upstream change cannot silently change the catalog.

    python -I tools/secure/download_catalog.py            write the catalog
    python -I tools/secure/download_catalog.py --check    exit 1 when the committed catalog differs from a fresh one

Left out by O4 (reported on stderr): the NVFP4 packs, Maya's EXL3 safetensors.  A variant whose files are not all
pinned (a 64-hex SHA-256; upstream: also a size and a 40-hex commit) is dropped and reported.

`mmproj` carries `repo` and `revision` besides the schema's path/size/sha256: the image encoder often lives in another
repository than the model (Unsloth's and Gyro's in ISTA-DASLab's, Maya-M-Derisked's in Maya's main repository).
"""

from __future__ import annotations

import argparse
import ast
import collections
import json
import os
import re
import string
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
UPSTREAM = REPO / "upstream"
MAYA = REPO / "engines" / "maya-ref"
UPSTREAM_VERSION = REPO / "upstream.version"
OUT = REPO / "secure_frontend" / "catalog" / "downloads.json"

SCHEMA_VERSION = 1
HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-fA-F]{64}")
ID = re.compile(r"[a-z0-9._-]{1,64}")
HF_REPO = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*/[A-Za-z0-9][A-Za-z0-9._-]*")
VERSION = re.compile(r"v\d+(?:\.\d+)+")
DEFAULT_FAMILIES = ("qwen", "swift")          # setup.py: MODELS[m].get("families", ("qwen", "swift"))
DEFAULT_PROFILE = "expert-profile.bin"        # setup.py: fam.get("profile", "expert-profile.bin")
MAYA_TITLE = "GLM-5.3-Flash"
MAYA_FAMILY = "glm53"

# The code the catalog restates, as the pinned sources spell it (whitespace-insensitive).
RULES = {
    "setup.py": (
        # hf(repo): the repository's folder at its HF_REVISIONS commit
        'return f"{hf_endpoint()}/{repo}/resolve/{HF_REVISIONS[repo]}/"',
        # file_pin(url): a file's (bytes, sha256) is REPO_PINS[repo][file name]
        'return REPO_PINS.get(m.group("repo"), {}).get(m.group("path").rsplit("/", 1)[-1])',
        # a FAMILIES model's shard names and count
        'return MODELS.get(model, {}).get("file", fam["file"]).format(q=model, i=i)',
        'return MODELS.get(model, {}).get("shards", fam.get("shards", 2))',
        # which models a family offers
        'names = [m for m in MODELS if family in MODELS[m].get("families", ("qwen", "swift"))]',
        # where the shards and the image encoder are fetched from
        'download(fam["hf"].format(q=model) + s.name, s)',
        'download(fam["mmproj_hf"] + fam["mmproj"], mmproj, "vision encoder")',
        # whether a model reads pictures (an image encoder to fetch)
        'return MODELS.get(model, {}).get("vision", FAMILIES.get(family, {}).get("vision")) is not False',
        # the pack step (iq_pack.py with the family's pack args) and the expert profile
        'run([sys.executable, str(ROOT / "tools" / "iq_pack.py"), "--gguf", str(shards[0]), "--out", str(pack), '
        '*fam.get("pack_args", [])], env=env)',
        '"--expert-profile", str(ROOT / "data" / fam.get("profile", "expert-profile.bin"))',
        # OWN_PATH_FAMILIES (Qwen3.6): the GGUF and the encoder at the repo's top, only the tokenizer prepared
        'download(fam["hf"] + gguf.name, gguf)',
        'download(fam["mmproj_hf"] + mmproj.name, mmproj, "vision encoder")',
        'pack = data / "packs" / tag.lower() # only the tokenizer: the engine reads the GGUF itself',
    ),
    "maya.py": (
        'HF = "https://huggingface.co/{repo}/resolve/{revision}/{path}"',
        'return HF.format(repo=repo, revision=revision, path=f"{folder}/{file}" if folder else file)',
        'return [(pattern or m["file"]).format(i=i, n=m["shards"]) for i in range(1, m["shards"] + 1)]',
        'repo, rev = m.get("repo", spec["repo"]), m.get("revision", spec["revision"])',
        'return MODELS.get(quant, {}).get("format") == "exl3"',
        # DeepSeek-V4.1's GGUFs: the manifest maya.py downloads them by (read as data, below)
        'DS4_GGUF_MANIFEST = ROOT / "tools" / "ds4" / "download_gguf.json"',
    ),
}
# Names whose literal the catalog reads; any other assignment to them, anywhere in the file, is refused.
READ = {"setup.py": ("MODELS", "FAMILIES", "OWN_PATH_FAMILIES", "REPO_PINS", "HF_REVISIONS"),
        "maya.py": ("MODELS", "MODEL_NAME", "DS4_MODEL_NAME")}
DS4_FAMILY = "deepseek"
# MiniMax-M3.1-Flash (owner 2026-10-28): Maya v1.1.30's manifest, vendored in our copy of Maya's tree
MM3_FAMILY = "minimax"
MM3_MANIFEST = REPO / "engine" / "third_party" / "maya" / "tools" / "mm3" / "download_gguf.json"
MUTATORS = {"update", "setdefault", "pop", "popitem", "clear", "append", "extend", "insert", "remove", "__setitem__"}
# Non-ASCII the about texts may use, spelled in ASCII; anything else becomes "?".
ASCII = {"—": "-", "–": "-", "−": "-", "‘": "'", "’": "'", "“": '"', "”": '"',
         "…": "...", "×": "x", "≈": "~", " ": " ", "→": "->", "≥": ">=", "≤": "<="}


class CatalogError(Exception):
    """A pinned table the generator cannot read statically, or a rule it relies on that changed."""


class Dropped(Exception):
    """A variant left out of the catalog; the message says why."""


# setup.py's hf("<repo>") (+ "<suffix>"): the repository's folder at its pinned commit.
HfFolder = collections.namedtuple("HfFolder", ("repo", "suffix"), defaults=("",))


_BUSY = object()


class Source:
    """One pinned Python file, parsed (never imported): its top-level literal tables, evaluated on demand."""

    def __init__(self, path: Path):
        self.path = path
        self.name = path.name
        try:
            self.text = path.read_text(encoding="utf-8")
            tree = ast.parse(self.text, filename=str(path))
        except (OSError, UnicodeDecodeError, SyntaxError) as e:
            raise CatalogError(f"{self.name}: cannot be read ({e.__class__.__name__})") from None
        self.assigned: dict[str, list[ast.expr]] = {}
        for stmt in tree.body:
            if isinstance(stmt, ast.Assign) and all(isinstance(t, ast.Name) for t in stmt.targets):
                for t in stmt.targets:
                    self.assigned.setdefault(t.id, []).append(stmt.value)
            elif isinstance(stmt, ast.AnnAssign) and isinstance(stmt.target, ast.Name) and stmt.value is not None:
                self.assigned.setdefault(stmt.target.id, []).append(stmt.value)
        self.changed = self._changed_elsewhere(tree)
        self.values: dict[str, object] = {}

    @staticmethod
    def _root(node: ast.expr) -> str | None:
        while isinstance(node, (ast.Subscript, ast.Attribute)):
            node = node.value
        return node.id if isinstance(node, ast.Name) else None

    def _changed_elsewhere(self, tree: ast.Module) -> dict[str, int]:
        """Names written anywhere other than by their plain top-level assignment: another binding or deletion, an
        item/attribute store, a mutating method call, a `global`/`nonlocal` (name -> first line)."""
        top = {id(t) for stmt in tree.body if isinstance(stmt, (ast.Assign, ast.AnnAssign))
               for t in (stmt.targets if isinstance(stmt, ast.Assign) else [stmt.target]) if isinstance(t, ast.Name)}
        found: dict[str, int] = {}
        for node in ast.walk(tree):
            name = None
            if isinstance(node, ast.Name) and isinstance(node.ctx, (ast.Store, ast.Del)) and id(node) not in top:
                name = node.id
            elif isinstance(node, (ast.Subscript, ast.Attribute)) and isinstance(node.ctx, (ast.Store, ast.Del)):
                name = self._root(node)
            elif isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr in MUTATORS:
                name = self._root(node.func.value)
            elif isinstance(node, (ast.Global, ast.Nonlocal)):
                for n in node.names:
                    found.setdefault(n, node.lineno)
            if name is not None:
                found.setdefault(name, node.lineno)
        return found

    def check_rules(self, rules: tuple[str, ...]) -> None:
        flat = " ".join(self.text.split())
        for rule in rules:
            if " ".join(rule.split()) not in flat:
                raise CatalogError(f"{self.name}: a rule the catalog restates changed or is gone: {rule}")

    def value(self, name: str):
        if name in self.values:
            if self.values[name] is _BUSY:
                raise CatalogError(f"{self.name}: {name} refers to itself")
            return self.values[name]
        nodes = self.assigned.get(name)
        if not nodes:
            raise CatalogError(f"{self.name}: no top-level literal {name}")
        if len(nodes) > 1:
            raise CatalogError(f"{self.name}:{nodes[1].lineno}: {name} is assigned more than once")
        if name in self.changed:
            raise CatalogError(f"{self.name}:{self.changed[name]}: {name} is changed outside its literal")
        self.values[name] = _BUSY
        self.values[name] = self._eval(nodes[0])
        return self.values[name]

    def _refuse(self, node: ast.AST, what: str = ""):
        raise CatalogError(f"{self.name}:{getattr(node, 'lineno', 0)}: cannot read "
                           f"{what or type(node).__name__} statically")

    def _eval(self, node: ast.expr):
        if isinstance(node, ast.Constant):
            if isinstance(node.value, (str, int, float, bool)) or node.value is None:
                return node.value
            self._refuse(node, type(node.value).__name__)
        if isinstance(node, (ast.Tuple, ast.List)):
            if any(isinstance(e, ast.Starred) for e in node.elts):
                self._refuse(node, "a starred element")
            items = [self._eval(e) for e in node.elts]
            return tuple(items) if isinstance(node, ast.Tuple) else items
        if isinstance(node, ast.Dict):
            out: dict = {}
            for k, v in zip(node.keys, node.values):
                if k is None:                              # {**OTHER}
                    merged = self._eval(v)
                    if not isinstance(merged, dict):
                        self._refuse(v, "a ** merge of a non-dict")
                    out.update(merged)
                    continue
                key = self._eval(k)
                if not isinstance(key, (str, int)):
                    self._refuse(k, "a dict key that is not a string")
                out[key] = self._eval(v)
            return out
        if isinstance(node, ast.Name):
            return self.value(node.id)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
            v = self._eval(node.operand)
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                return -v
        if isinstance(node, ast.BinOp):
            left, right = self._eval(node.left), self._eval(node.right)
            if isinstance(node.op, ast.Add):
                if isinstance(left, str) and isinstance(right, str):
                    return left + right
                if isinstance(left, HfFolder) and isinstance(right, str):
                    return HfFolder(left.repo, left.suffix + right)
            numbers = all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in (left, right))
            if numbers and isinstance(node.op, (ast.Add, ast.Mult)):
                return left + right if isinstance(node.op, ast.Add) else left * right
            self._refuse(node, f"a {type(node.op).__name__} of these values")
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "hf"
                and self.name == "setup.py" and len(node.args) == 1 and not node.keywords):
            repo = self._eval(node.args[0])
            if isinstance(repo, str):
                return HfFolder(repo)
        self._refuse(node)


def fill(pattern: str, **fields) -> str:
    """str.format restricted to the plain fields the tables use ({q}, {i}, {n}, {i:05d}, {n:05d})."""
    if not isinstance(pattern, str):
        raise Dropped("a file pattern that is not a string")
    for _, field, spec, conversion in string.Formatter().parse(pattern):
        if field is None:
            continue
        if field not in fields or conversion is not None or spec not in ("", "05d"):
            raise Dropped(f"an unknown file pattern field {{{field}}}")
    return pattern.format(**fields)


def ascii_text(text: str) -> str:
    return " ".join("".join(c if ord(c) < 128 else ASCII.get(c, "?") for c in text).split())


def gguf_path(path: str) -> str:
    if not path.lower().endswith(".gguf"):
        raise Dropped(f"{path} is not a .gguf file")
    if path.startswith("/") or "\\" in path or any(part in ("", ".", "..") for part in path.split("/")):
        raise Dropped(f"{path} is not a plain path in its repository")
    return path


def hf_repo(repo) -> str:
    if not isinstance(repo, str) or not HF_REPO.fullmatch(repo):
        raise Dropped(f"{repo!r} is not a Hugging Face owner/repo")
    return repo


def sha256_of(value, name: str) -> str:
    if not isinstance(value, str) or not HEX64.fullmatch(value):
        raise Dropped(f"{name} has no 64-hex sha256")
    return value.lower()


# ------------------------------------------------------------------------------------------------ upstream Strata
def upstream_file(pins: dict, repo: str, path: str) -> dict:
    """A pinned file of upstream's: REPO_PINS[repo][file name] = (bytes, sha256)."""
    name = gguf_path(path).rsplit("/", 1)[-1]
    pin = pins.get(repo, {}).get(name) if isinstance(pins.get(repo), dict) else None
    if not (isinstance(pin, tuple) and len(pin) == 2):
        raise Dropped(f"{name} has no size and sha256 pin")
    size, sha = pin
    if type(size) is not int or size <= 0:
        raise Dropped(f"{name} has no size")
    return {"path": path, "size": size, "sha256": sha256_of(sha, name)}


def upstream_revision(revisions: dict, repo: str) -> str:
    rev = revisions.get(repo)
    if not isinstance(rev, str) or not HEX40.fullmatch(rev):
        raise Dropped(f"{repo} has no 40-hex pinned revision")
    return rev


def upstream_mmproj(pins: dict, revisions: dict, folder, name) -> dict:
    if not isinstance(folder, HfFolder) or folder.suffix or not isinstance(name, str):
        raise Dropped("its image encoder's folder is not a pinned repository top")
    repo = hf_repo(folder.repo)
    return {**upstream_file(pins, repo, name), "repo": repo, "revision": upstream_revision(revisions, repo)}


def upstream_entries(up: Source, notes: list[str]) -> list[dict]:
    up.check_rules(RULES["setup.py"])
    models, families, own = up.value("MODELS"), up.value("FAMILIES"), up.value("OWN_PATH_FAMILIES")
    pins, revisions = up.value("REPO_PINS"), up.value("HF_REVISIONS")
    out = []
    for fam_key, fam in families.items():
        offered = [m for m in models if fam_key in models[m].get("families", DEFAULT_FAMILIES)]
        if fam.get("ready"):
            notes.extend(f"left out: strata {fam_key} {m}: a ready-made pack, not a GGUF (O4)" for m in offered)
            continue
        for model in offered:
            spec = models[model]
            try:
                folder = fam.get("hf")
                if not isinstance(folder, HfFolder):
                    raise Dropped("its folder is not a pinned Hugging Face repository")
                repo = hf_repo(folder.repo)
                prefix = fill(folder.suffix, q=model)
                shards = spec.get("shards", fam.get("shards", 2))
                if type(shards) is not int or shards < 1:
                    raise Dropped("no shard count")
                files = [upstream_file(pins, repo, prefix + fill(spec.get("file", fam.get("file")), q=model, i=i))
                         for i in range(1, shards + 1)]
                vision = spec.get("vision", fam.get("vision")) is not False
                pack_args = list(fam.get("pack_args", []))
                if not all(isinstance(a, str) and re.fullmatch(r"--[a-z0-9][a-z0-9-]*", a) for a in pack_args):
                    raise Dropped("pack args that are not plain flags")
                out.append({
                    "id": f"{fam['name']}-{model.lower()}", "name": f"{fam['title']} {model}", "source": "strata",
                    "family": fam_key, "engine": "qwen", "about": ascii_text(spec["about"]), "repo": repo,
                    "revision": upstream_revision(revisions, repo), "files": files, "prepare": "iq_pack",
                    "pack_args": pack_args, "expert_profile": fam.get("profile", DEFAULT_PROFILE),
                    "mmproj": upstream_mmproj(pins, revisions, fam.get("mmproj_hf"), fam.get("mmproj"))
                    if vision else None})
            except Dropped as why:
                notes.append(f"dropped: strata {fam_key} {model}: {why}")
    for fam_key, fam in own.items():
        for model, spec in fam["models"].items():
            try:
                folder = fam.get("hf")
                if not isinstance(folder, HfFolder) or folder.suffix:
                    raise Dropped("its folder is not a pinned repository top")
                repo = hf_repo(folder.repo)
                out.append({
                    "id": f"{fam['name']}-{model.lower()}", "name": f"{fam['title']} {model}", "source": "strata",
                    "family": fam_key, "engine": "qwen", "about": ascii_text(spec["about"]), "repo": repo,
                    "revision": upstream_revision(revisions, repo),
                    "files": [upstream_file(pins, repo, fill(fam["file"], q=model))],
                    # the engine reads this GGUF as it is: no pack, no expert profile (upstream exports the
                    # tokenizer only)
                    "prepare": "none", "pack_args": [], "expert_profile": None,
                    "mmproj": upstream_mmproj(pins, revisions, fam.get("mmproj_hf"), fam.get("mmproj"))
                    if fam.get("vision") is not False else None})
            except Dropped as why:
                notes.append(f"dropped: strata {fam_key} {model}: {why}")
    return out


# ------------------------------------------------------------------------------------------------ Project Maya
def maya_revision(rev) -> str:
    if rev == "main" or (isinstance(rev, str) and HEX40.fullmatch(rev)):
        return rev
    raise Dropped(f"revision {rev!r} is neither a 40-hex commit nor main")


def maya_sha(table, name: str) -> str:
    """Maya's sha256 for a file: one digest, or a list of every version published (newest first): the newest."""
    want = table.get(name) if isinstance(table, dict) else None
    if isinstance(want, list):
        want = want[0] if want else None
    return sha256_of(want, name)


def maya_entries(maya: Source, notes: list[str]) -> list[dict]:
    maya.check_rules(RULES["maya.py"])
    models, base = maya.value("MODELS"), maya.value("MODEL_NAME")
    out = []
    for key, spec in models.items():
        if spec.get("format") is not None or "files" in spec:
            notes.append(f"left out: maya {key}: {spec.get('format', 'not a')} safetensors, not a GGUF (O4)")
            continue
        try:
            repo, rev, folder = hf_repo(spec["repo"]), maya_revision(spec["revision"]), spec["folder"]
            shards = spec["shards"]
            if type(shards) is not int or shards < 1:
                raise Dropped("no shard count")
            files = []
            for i in range(1, shards + 1):
                name = fill(spec["file"], i=i, n=shards)
                files.append({"path": gguf_path(f"{folder}/{name}" if folder else name), "size": None,
                              "sha256": maya_sha(spec["sha256"], name)})
            vision, mmproj = spec.get("vision"), None
            if vision is not None:
                v_folder, v_name = vision["folder"], vision["mmproj"]
                mmproj = {"path": gguf_path(f"{v_folder}/{v_name}" if v_folder else v_name), "size": None,
                          "sha256": maya_sha(vision["sha256"], v_name),
                          "repo": hf_repo(vision.get("repo", spec["repo"])),
                          "revision": maya_revision(vision.get("revision", spec["revision"]))}
            out.append({
                "id": f"{base}-{key.lower()}", "name": f"{MAYA_TITLE} {key}", "source": "maya", "family": MAYA_FAMILY,
                "engine": "glm53", "about": ascii_text(spec["about"]), "repo": repo, "revision": rev, "files": files,
                # Maya's GLM engine reads these from Maya's pack (maya.py pack_step), which the server builds with
                # `strata-prepare download --maya` (owner 2026-10-27): runnable once packed
                "prepare": "maya_pack", "pack_args": [], "expert_profile": None, "mmproj": mmproj})
        except (Dropped, KeyError) as why:
            notes.append(f"dropped: maya {key}: {why if isinstance(why, Dropped) else f'no {why} in its entry'}")
    return out


def maya_ds4_entries(maya: Source, manifest: Path, notes: list[str]) -> list[dict]:
    """DeepSeek-V4.1's GGUF quants from maya.py's DeepSeek manifest (DS4_GGUF_MANIFEST, JSON data: unsloth's quants at
    the top, Maya's own under "maya"), every file [bytes, sha256] at the manifest's revision. Packed by Maya's DeepSeek
    step (maya.py ds4_pack_step; strata-prepare download --maya). The drafter and the vision tower are safetensors and
    .bin files, not GGUF (O4); an earlier layout ("earlier") is only used by a folder that already holds it."""
    base = maya.value("DS4_MODEL_NAME")
    try:
        doc = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as e:
        raise CatalogError(f"cannot read {manifest.name} ({e.__class__.__name__})") from None
    notes.append("left out: maya deepseek drafter and vision tower: not GGUF files (O4)")
    out = []
    groups = [(doc, ascii_text(doc.get("_", ""))), (doc.get("maya") or {}, None)]
    for group, about in groups:
        for key, spec in (group.get("quants") or {}).items():
            try:
                repo, rev = hf_repo(group["repo"]), maya_revision(group["revision"])
                if not HEX40.fullmatch(rev):
                    raise Dropped("not pinned to a commit")
                files = []
                for path, pin in spec["files"].items():
                    if not (isinstance(pin, list) and len(pin) == 2 and type(pin[0]) is int and pin[0] > 0):
                        raise Dropped(f"{path} has no size")
                    files.append({"path": gguf_path(path), "size": pin[0], "sha256": sha256_of(pin[1], path)})
                if not files:
                    raise Dropped("no files")
                out.append({
                    "id": f"{base}-{key.lower()}", "name": f"{doc['name']} {key}", "source": "maya", "family": DS4_FAMILY,
                    "engine": DS4_FAMILY, "about": about or ascii_text(spec["about"]), "repo": repo, "revision": rev,
                    "files": files, "prepare": "maya_pack", "pack_args": [], "expert_profile": None, "mmproj": None})
            except (Dropped, KeyError) as why:
                notes.append(f"dropped: maya deepseek {key}: {why if isinstance(why, Dropped) else f'no {why} in its entry'}")
    return out


def maya_mm3_entries(manifest: Path, notes: list[str]) -> list[dict]:
    """MiniMax-M3.1-Flash's GGUF quants (owner 2026-10-28, under the MiniMax Community License) from Maya v1.1.30's MiniMax
    manifest (tools/mm3/download_gguf.json, vendored byte for byte in engine/third_party/maya: the pinned maya-ref predates
    it), every file [bytes, sha256] at the manifest's revision, the indexers side file among them. Packed by Maya's
    MiniMax step (maya.py mm3_pack_step; strata-prepare download --maya)."""
    try:
        doc = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as e:
        raise CatalogError(f"cannot read {manifest.name} ({e.__class__.__name__})") from None
    out = []
    name = doc.get("name", "")
    for key, spec in (doc.get("quants") or {}).items():
        try:
            repo, rev = hf_repo(doc["repo"]), maya_revision(doc["revision"])
            if not HEX40.fullmatch(rev):
                raise Dropped("not pinned to a commit")
            files = []
            for path, pin in spec["files"].items():
                if not (isinstance(pin, list) and len(pin) == 2 and type(pin[0]) is int and pin[0] > 0):
                    raise Dropped(f"{path} has no size")
                files.append({"path": gguf_path(path), "size": pin[0], "sha256": sha256_of(pin[1], path)})
            if not files:
                raise Dropped("no files")
            tier = key[len(name) + 1:] if key.startswith(name + "-") else key   # (the quant's tier: Maya-XS, Maya-S)
            out.append({
                "id": key.lower(), "name": f"{name} {tier}", "source": "maya", "family": MM3_FAMILY, "engine": MM3_FAMILY,
                "about": ascii_text(spec["about"]), "repo": repo, "revision": rev, "files": files, "prepare": "maya_pack",
                "pack_args": [], "expert_profile": None, "mmproj": None})
        except (Dropped, KeyError) as why:
            notes.append(f"dropped: maya minimax {key}: {why if isinstance(why, Dropped) else f'no {why} in its entry'}")
    return out


# ------------------------------------------------------------------------------------------------ the catalog
def read_versions(upstream_version: Path, maya: Path) -> dict:
    try:
        up = upstream_version.read_text(encoding="ascii").strip()
        changelog = (maya / "CHANGELOG.md").read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as e:
        raise CatalogError(f"cannot read the pinned versions ({e.__class__.__name__})") from None
    found = re.search(r"^## (v\d+(?:\.\d+)+)\b", changelog, re.MULTILINE)
    if not VERSION.fullmatch(up) or found is None:
        raise CatalogError("the pinned versions (upstream.version, Maya's CHANGELOG.md) are not tags")
    return {"upstream": up, "maya": found.group(1)}


def build(upstream: Path, maya: Path, upstream_version: Path, mm3_manifest: Path = MM3_MANIFEST) -> tuple[dict, list[str]]:
    """The catalog document and the notes (what was left out by O4 and what was dropped, and why)."""
    notes: list[str] = []
    generated_from = read_versions(upstream_version, maya)
    maya_source = Source(maya / "maya.py")
    models = (upstream_entries(Source(upstream / "setup.py"), notes) + maya_entries(maya_source, notes) +
              maya_ds4_entries(maya_source, maya / "tools" / "ds4" / "download_gguf.json", notes) +
              maya_mm3_entries(mm3_manifest, notes))
    ids = [m["id"] for m in models]
    bad = [i for i in ids if not ID.fullmatch(i)]
    if bad or len(set(ids)) != len(ids):
        raise CatalogError(f"catalog ids are not unique [a-z0-9._-]{{1,64}}: {sorted(set(bad) or ids)}")
    models.sort(key=lambda m: m["id"])
    return {"schema_version": SCHEMA_VERSION, "generated_from": generated_from, "models": models}, notes


def render(doc: dict) -> bytes:
    return (json.dumps(doc, indent=2, ensure_ascii=True) + "\n").encode("ascii")


def write(path: Path, data: bytes) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--upstream", type=Path, default=UPSTREAM, help="the upstream tree (setup.py)")
    ap.add_argument("--maya", type=Path, default=MAYA, help="the Maya tree (maya.py, CHANGELOG.md)")
    ap.add_argument("--upstream-version", type=Path, default=UPSTREAM_VERSION, help="the upstream tag file")
    ap.add_argument("--out", type=Path, default=OUT, help="the catalog file")
    ap.add_argument("--check", action="store_true", help="exit 1 when --out differs from a fresh generation")
    a = ap.parse_args(argv)
    try:
        doc, notes = build(a.upstream, a.maya, a.upstream_version)
    except CatalogError as e:
        print(f"download_catalog: error: {e}", file=sys.stderr)
        return 2
    for note in notes:
        print(f"download_catalog: {note}", file=sys.stderr)
    data = render(doc)
    if a.check:
        try:
            current = a.out.read_bytes()
        except OSError:
            print(f"download_catalog: {a.out} is missing", file=sys.stderr)
            return 1
        if current != data:
            print(f"download_catalog: {a.out} differs from a fresh generation: run tools/secure/download_catalog.py",
                  file=sys.stderr)
            return 1
        print(f"download_catalog: {a.out} is current ({len(doc['models'])} models)", file=sys.stderr)
        return 0
    write(a.out, data)
    print(f"download_catalog: wrote {a.out} ({len(doc['models'])} models)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
