"""tools/secure/secure_tokenizer.py - GLM pre-tokenizer aliases over upstream's tokenizer, without editing it.

Upstream's `upstream/tools/strata_tokenizer.py` (the git submodule, never edited) knows only the `qwen35`
pre-tokenizer.  GLM-5.3 tokenizer packs declare `glm4` or the older `chatglm-bpe` alias, whose regex caps
numeric runs at three digits.  This module imports upstream's tokenizer and adds:

  * GLM4_PATTERN and pre_tokenizer_pattern(pre) -> (regex, provenance);
  * Tokenizer, a subclass that recompiles `_re` from the declared `pre`;
  * extract(), which calls upstream's extract() and rewrites `pre_pattern`/`pre_pattern_source` in the
    pack's tokenizer.json, because upstream ships the Qwen35 pattern for every pack.

The GLM regex was transcribed from lighttransport/Strata `glm53f` @ 4894c996 `tools/strata_tokenizer.py`
(see docs/PROVENANCE.md).  Upstream's module inserts its own directory into sys.path for gguf_reader, so it
is imported here as a top-level module from upstream/tools.

Coupling to upstream internals (not a public API; re-verify on every upstream pointer bump, see
docs/AGENT_HANDOFF.md "Update upstream"):

  * `strata_tokenizer.Tokenizer._re` - the compiled pre-tokenizer regex this subclass replaces;
  * `strata_tokenizer.Tokenizer.__init__` positional argument order
    `(tokens, merges, token_types, pre, special_ids)` and its `self.pre` attribute;
  * `strata_tokenizer.extract(gguf_path, out_dir)` returning the config dict (with `pre`, `vocab_size`,
    `n_merges`) and writing it to `<out_dir>/tokenizer/tokenizer.json`, the file rewritten here (extract()
    raises if that file is missing rather than creating it);
  * the module-level `QWEN35_PATTERN` and `bytes_to_unicode`.

Gemma 4 (plan 1 Task 6; spec §2.5, §6.2): `--profile gemma4` writes the frontend's `spm_bpe` pack
(`vocab.json`, `merges.json`, `token_type.json`, `tokenizer.json`) from a GGUF whose
`tokenizer.ggml.model` is `gemma4`, through this module's own header-only GGUF reader (no upstream
code, no tensor data read); any other model with the flag, and a gemma4 GGUF without it, is refused
before anything is written. `merges.json` is a JSON array of [left, right] pairs in rank order, so
Gemma's merges that hold newlines and tabs survive (a line-based merges.txt cannot hold them).
`spm_bpe.Encoder` (tools/secure/spm_bpe.py) is the matching reference encoder.
"""
from __future__ import annotations

import hashlib
import json
import pathlib
import struct
import sys

import regex

_UPSTREAM_TOOLS = pathlib.Path(__file__).resolve().parents[2] / "upstream" / "tools"
if not (_UPSTREAM_TOOLS / "strata_tokenizer.py").is_file():
    raise ImportError("upstream/tools/strata_tokenizer.py is missing: run `git submodule update --init` "
                      "from the repository root")
if str(_UPSTREAM_TOOLS) not in sys.path:
    sys.path.insert(0, str(_UPSTREAM_TOOLS))
import strata_tokenizer as _upstream  # noqa: E402

QWEN35_PATTERN = _upstream.QWEN35_PATTERN
bytes_to_unicode = _upstream.bytes_to_unicode

# llama.cpp's LLAMA_VOCAB_PRE_TYPE_CHATGLM4.  GLM-5.3 tokenizer packs identify
# this as either `glm4` or the older `chatglm-bpe` alias.  The three-digit cap
# is semantic: using the Qwen pattern here tokenizes every digit separately,
# while accepting arbitrarily long numeric runs changes BPE merge boundaries.
GLM4_PATTERN = (
    r"(?:'[sS]|'[tT]|'[rR][eE]|'[vV][eE]|'[mM]|'[lL][lL]|'[dD])"
    r"|[^\r\n\p{L}\p{N}]?\p{L}+"
    r"|\p{N}{1,3}"
    r"| ?[^\s\p{L}\p{N}]+[\r\n]*"
    r"|\s*[\r\n]+|\s+(?!\S)|\s+"
)
GLM4_PRE_NAMES = ("glm4", "chatglm-bpe")

# llama.cpp master (e86f5fd1) LLAMA_VOCAB_PRE_TYPE_FLINT: three regexes run in turn, each over the pieces the one before
# it left. The frontend's flint_bpe mode implements them as llama.cpp runs them (over its collapsed text, where every
# \p{L*} class is the one letter class); Python's `regex` would split case changes, so this module does not encode
# flint text itself.
FLINT_PATTERNS = (
    r"\p{N}{1,3}",
    "[一-龥぀-ゟ゠-ヿ]+",
    r"[^\r\n\p{L}\p{N}]?[\p{Lu}\p{Lt}\p{Lm}\p{Lo}\p{M}]*[\p{Ll}\p{Lm}\p{Lo}\p{M}]+"
    r"(?:'[sS]|'[tT]|'[rR][eE]|'[vV][eE]|'[mM]|'[lL][lL]|'[dD])?"
    r"|[^\r\n\p{L}\p{N}]?[\p{Lu}\p{Lt}\p{Lm}\p{Lo}\p{M}]+[\p{Ll}\p{Lm}\p{Lo}\p{M}]*"
    r"(?:'[sS]|'[tT]|'[rR][eE]|'[vV][eE]|'[mM]|'[lL][lL]|'[dD])?"
    r"| ?[\p{P}\p{S}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+",
)
FLINT_PRE_NAME = "flint"
FLINT_SOURCE = ("llama.cpp master e86f5fd1 src/llama-vocab.cpp (LLAMA_VOCAB_PRE_TYPE_FLINT: pre_patterns, run in turn; "
                "pre_pattern is the last)")


def pre_tokenizer_pattern(pre: str) -> tuple[str, str]:
    """Return the fixed regex and provenance for a declared GGUF pre-tokenizer.

    Unknown pre-tokenizers retain the historic Qwen35 fallback so this utility
    remains usable for existing Strata packs.  A caller that needs exact
    compatibility must validate `pre` against its model profile before use.
    flint's is the last of its three (FLINT_PATTERNS holds them all).
    """
    if pre in GLM4_PRE_NAMES:
        return (GLM4_PATTERN,
                ".ref/llama.cpp src/llama-vocab.cpp (LLAMA_VOCAB_PRE_TYPE_CHATGLM4)")
    if pre == FLINT_PRE_NAME:
        return (FLINT_PATTERNS[-1], FLINT_SOURCE)
    return (QWEN35_PATTERN,
            ".ref/llama.cpp src/llama-vocab.cpp L396 (LLAMA_VOCAB_PRE_TYPE_QWEN35)")


class Tokenizer(_upstream.Tokenizer):
    """Upstream's tokenizer with the pre-tokenizer regex chosen by the declared `pre`."""

    def __init__(self, tokens: list[str], merges: list[str], token_types: list[int] | None = None,
                 pre: str = "qwen35", special_ids: dict[str, int] | None = None):
        super().__init__(tokens, merges, token_types, pre, special_ids)
        # Upstream __init__ binds self._split itself (bound findall on QWEN35_PATTERN, or a closure
        # for deepseek-family pres) — overriding only self._re stopped redirecting anything. Rebind
        # the split itself to the declared pre-pattern. Pinned by
        # test_tokenizer_splits_with_the_declared_pre_pattern.
        self._re = regex.compile(pre_tokenizer_pattern(self.pre)[0])
        self._split = self._re.findall


def extract(gguf_path, out_dir) -> dict:
    """Upstream's extract(), then ship the declared pre-tokenizer's pattern in tokenizer.json."""
    cfg = _upstream.extract(gguf_path, out_dir)
    cfg["pre_pattern"], cfg["pre_pattern_source"] = pre_tokenizer_pattern(cfg["pre"])
    if cfg["pre"] == FLINT_PRE_NAME:
        cfg["pre_patterns"] = list(FLINT_PATTERNS)
    path = pathlib.Path(out_dir) / "tokenizer" / "tokenizer.json"
    if not path.is_file():
        raise RuntimeError("upstream extract() did not write %s: upstream layout changed; re-verify the wrapper "
                           "(tools/secure/secure_tokenizer.py, see its module docstring)" % path)
    path.write_text(json.dumps(cfg, ensure_ascii=False, indent=1), encoding="utf-8")
    return cfg


# ---------------------------------------------------------------- Gemma 4 (plan 1 Task 6)
GEMMA4_PROFILE = "gemma4"
_GGUF_MAGIC = 0x46554747  # "GGUF" little-endian
_GGUF_SCALARS = {0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "f", 7: "?", 10: "Q", 11: "q", 12: "d"}
_GGUF_STRING, _GGUF_ARRAY = 8, 9
# Bounds well above Gemma 4's (58 keys, 262,144 tokens, 514,906 merges, a 12,045-character template)
# and the frontend loader's caps (1,048,576 entries, 16 KiB a token).
_MAX_KV = 4096
_MAX_ARRAY = 1 << 21
_MAX_STRING = 1 << 24
# The literals the frontend's renderer and stop rules look up by text (spec §2.5): recorded for the
# operator; the frontend never trusts these numbers, it finds each literal itself.
_GEMMA4_MARKERS = {"turn_open": "<|turn>", "turn_close": "<turn|>", "think": "<|think|>",
                   "channel_open": "<|channel>", "channel_close": "<channel|>"}


class _HeaderReader:
    """Reads a GGUF's metadata section only (magic, version, counts, key/values), hashing what it reads."""

    def __init__(self, stream):
        self._stream = stream
        self.digest = hashlib.sha256()
        self.consumed = 0

    def read(self, n: int) -> bytes:
        data = self._stream.read(n)
        if len(data) != n:
            raise SystemExit("not a GGUF: the metadata ends early")
        self.digest.update(data)
        self.consumed += n
        return data

    def unpack(self, fmt: str):
        return struct.unpack("<" + fmt, self.read(struct.calcsize("<" + fmt)))

    def string(self) -> str:
        (n,) = self.unpack("Q")
        if n > _MAX_STRING:
            raise SystemExit("not a GGUF: a metadata string is too long")
        try:
            return self.read(n).decode("utf-8")
        except UnicodeDecodeError:
            raise SystemExit("not a GGUF: a metadata string is not UTF-8") from None

    def value(self, kind: int, depth: int = 0):
        if kind in _GGUF_SCALARS:
            return self.unpack(_GGUF_SCALARS[kind])[0]
        if kind == _GGUF_STRING:
            return self.string()
        if kind == _GGUF_ARRAY and depth == 0:
            (item_kind, n) = self.unpack("IQ")
            if n > _MAX_ARRAY:
                raise SystemExit("not a GGUF: a metadata array is too long")
            if item_kind in _GGUF_SCALARS:
                fmt = _GGUF_SCALARS[item_kind]
                return list(struct.unpack("<" + fmt * n, self.read(struct.calcsize("<" + fmt) * n)))
            return [self.value(item_kind, depth + 1) for _ in range(n)]
        raise SystemExit("not a GGUF: unsupported metadata type %d" % kind)


def read_gguf_header(gguf_path) -> tuple[dict, int, str]:
    """(metadata, header byte count, SHA-256 of those bytes). Tensor info and data are never read."""
    with open(gguf_path, "rb") as stream:
        reader = _HeaderReader(stream)
        try:
            (magic, version, _n_tensors, n_kv) = reader.unpack("IIQQ")
        except SystemExit:
            raise SystemExit("not a GGUF: too short") from None
        if magic != _GGUF_MAGIC:
            raise SystemExit("not a GGUF: bad magic")
        if version not in (2, 3):
            raise SystemExit("not a GGUF: version %d is not supported" % version)
        if n_kv > _MAX_KV:
            raise SystemExit("not a GGUF: too many metadata keys")
        metadata = {}
        for _ in range(n_kv):
            key = reader.string()
            (kind,) = reader.unpack("I")
            if key in metadata:
                raise SystemExit("not a GGUF: duplicate metadata key")
            metadata[key] = reader.value(kind)
        return metadata, reader.consumed, reader.digest.hexdigest()


def extract_gemma4(gguf_path, out_dir) -> dict:
    """Write `<out_dir>/tokenizer/` as the frontend's spm_bpe pack; refuse anything not a gemma4 tokenizer."""
    metadata, header_bytes, header_sha256 = read_gguf_header(gguf_path)
    model = metadata.get("tokenizer.ggml.model")
    if model != GEMMA4_PROFILE:
        raise SystemExit("--profile gemma4: tokenizer.ggml.model is %s, not gemma4" % model)
    tokens = metadata.get("tokenizer.ggml.tokens")
    types = metadata.get("tokenizer.ggml.token_type")
    merges = metadata.get("tokenizer.ggml.merges")
    if not isinstance(tokens, list) or not all(isinstance(t, str) and t for t in tokens):
        raise SystemExit("--profile gemma4: tokenizer.ggml.tokens must be non-empty strings")
    if not isinstance(types, list) or len(types) != len(tokens) or not all(isinstance(t, int) for t in types):
        raise SystemExit("--profile gemma4: tokenizer.ggml.token_type must hold one token type per token")
    if not isinstance(merges, list) or not all(isinstance(m, str) for m in merges):
        raise SystemExit("--profile gemma4: tokenizer.ggml.merges must be strings")
    ids = {}
    for i, token in enumerate(tokens):
        if token in ids:
            raise SystemExit("--profile gemma4: duplicate token %d" % i)
        ids[token] = i
    pairs = []
    for rank, merge in enumerate(merges):
        parts = merge.split(" ")
        if len(parts) != 2 or parts[0] not in ids or parts[1] not in ids:
            raise SystemExit("--profile gemma4: merge %d is not two vocabulary tokens separated by one space" % rank)
        pairs.append(parts)
    if metadata.get("tokenizer.ggml.add_space_prefix", False):
        raise SystemExit("--profile gemma4: add_space_prefix true is not the Gemma 4 tokenizer the frontend implements")
    special_ids = {}
    for name in ("bos", "eos", "pad", "unk", "mask"):
        key = "tokenizer.ggml.%s_token_id" % ("padding" if name == "pad" else "unknown" if name == "unk" else name)
        if isinstance(metadata.get(key), int):
            special_ids[name] = metadata[key]
    for name, literal in _GEMMA4_MARKERS.items():
        if literal in ids:
            special_ids[name] = ids[literal]
    cfg = {
        "model": GEMMA4_PROFILE,
        "profile": GEMMA4_PROFILE,
        "pre": "gemma4",
        "merges_format": "merges.json: [left, right] pairs in rank order",
        "add_bos": bool(metadata.get("tokenizer.ggml.add_bos_token", True)),
        "add_space_prefix": False,
        "vocab_size": len(tokens),
        "n_merges": len(pairs),
        "special_ids": special_ids,
        "source_header_bytes": header_bytes,
        "source_header_sha256": header_sha256,
    }
    out = pathlib.Path(out_dir) / "tokenizer"
    out.mkdir(parents=True, exist_ok=True)
    (out / "vocab.json").write_text(json.dumps(ids, ensure_ascii=False), encoding="utf-8", newline="\n")
    (out / "merges.json").write_text(json.dumps(pairs, ensure_ascii=False), encoding="utf-8", newline="\n")
    (out / "token_type.json").write_text(json.dumps(types), encoding="utf-8", newline="\n")
    (out / "tokenizer.json").write_text(json.dumps(cfg, ensure_ascii=False, indent=1), encoding="utf-8", newline="\n")
    return cfg


def main(argv: list[str] | None = None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Write a pack's tokenizer/ with the declared pre-tokenizer's pattern")
    ap.add_argument("--gguf", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--profile", choices=[GEMMA4_PROFILE],
                    help="gemma4: the frontend's spm_bpe pack (merges.json) from a gemma4 GGUF")
    args = ap.parse_args(argv)
    if args.profile == GEMMA4_PROFILE:
        cfg = extract_gemma4(args.gguf, args.out)
        print("tokenizer: profile=gemma4 vocab=%d merges=%d header_sha256=%s"
              % (cfg["vocab_size"], cfg["n_merges"], cfg["source_header_sha256"]))
        return 0
    metadata, _, _ = read_gguf_header(args.gguf)
    if metadata.get("tokenizer.ggml.model") == GEMMA4_PROFILE:
        raise SystemExit("a gemma4 GGUF needs --profile gemma4 (its pack is spm_bpe with merges.json)")
    cfg = extract(args.gguf, args.out)
    print("tokenizer: pre=%s vocab=%d merges=%d" % (cfg["pre"], cfg["vocab_size"], cfg["n_merges"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
