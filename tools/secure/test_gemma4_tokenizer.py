"""Gemma 4 plan 1 Task 6: `secure_tokenizer.py --profile gemma4` packs and the spm_bpe reference encoder.

The GGUF here is a tiny metadata-only v3 file written with struct (like test_secure_tokenizer.py): a
vocabulary shaped like Gemma 4's (the special tokens, the 256 <0xXX> byte tokens, newline runs, U+2581,
merges that hold newlines and tabs). No weights, no numpy, no network.
"""
from __future__ import annotations

import hashlib
import json
import struct
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import secure_tokenizer  # noqa: E402
from secure_tokenizer import extract_gemma4, main  # noqa: E402
from spm_bpe import Encoder  # noqa: E402

GGUF_UINT32, GGUF_INT32, GGUF_BOOL, GGUF_STRING, GGUF_ARRAY = 4, 5, 7, 8, 9
SPACE = "▁"

SPECIALS = [("<pad>", 3), ("<eos>", 3), ("<bos>", 3), ("<unk>", 3), ("<mask>", 3), ("<|tool>", 3), ("<tool|>", 3),
            ("<|tool_call>", 4), ("<tool_call|>", 4), ("<|tool_response>", 4), ("<tool_response|>", 4), ('<|"|>', 4),
            ("<|think|>", 3), ("<|channel>", 4), ("<channel|>", 4), ("<|turn>", 3), ("<turn|>", 3)]
MERGES = [(SPACE, SPACE), ("H", "i"), ("o", "r"), (SPACE, "w"), (SPACE + "w", "or"), ("l", "d"), (SPACE + "wor", "ld"),
          ("b", "c"), ("a", "b"), ("a", "a"), ("\n", "\n"), ("\t", "\t"), ("\n\n", "\n")]


def vocabulary() -> tuple[list[str], list[int]]:
    tokens = [t for t, _ in SPECIALS]
    types = [ty for _, ty in SPECIALS]
    tokens += ["<0x%02X>" % b for b in range(256)]
    types += [6] * 256
    for token in ["\n", "\n\n", "\t", SPACE] + [chr(c) for c in range(0x21, 0x7F)]:
        if token not in tokens:
            tokens.append(token)
            types.append(1)
    for left, right in MERGES:
        if left + right not in tokens:
            tokens.append(left + right)
            types.append(1)
    return tokens, types


def _string(text: str) -> bytes:
    raw = text.encode("utf-8")
    return struct.pack("<Q", len(raw)) + raw


def _strings(values: list[str]) -> bytes:
    return struct.pack("<IQ", GGUF_STRING, len(values)) + b"".join(_string(v) for v in values)


def _int32s(values: list[int]) -> bytes:
    return struct.pack("<IQ", GGUF_INT32, len(values)) + b"".join(struct.pack("<i", v) for v in values)


def write_gguf(path: Path, model: str = "gemma4", merges: list[tuple[str, str]] | None = None,
               tokens: list[str] | None = None, types: list[int] | None = None, tail: bytes = b"") -> Path:
    default_tokens, default_types = vocabulary()
    tokens = default_tokens if tokens is None else tokens
    types = default_types if types is None else types
    merges = MERGES if merges is None else merges
    entries = [
        ("general.architecture", GGUF_STRING, _string("gemma4")),
        ("tokenizer.ggml.model", GGUF_STRING, _string(model)),
        ("tokenizer.ggml.tokens", GGUF_ARRAY, _strings(tokens)),
        ("tokenizer.ggml.token_type", GGUF_ARRAY, _int32s(types)),
        ("tokenizer.ggml.merges", GGUF_ARRAY, _strings(["%s %s" % m for m in merges])),
        ("tokenizer.ggml.bos_token_id", GGUF_UINT32, struct.pack("<I", 2)),
        ("tokenizer.ggml.eos_token_id", GGUF_UINT32, struct.pack("<I", 1)),
        ("tokenizer.ggml.padding_token_id", GGUF_UINT32, struct.pack("<I", 0)),
        ("tokenizer.ggml.unknown_token_id", GGUF_UINT32, struct.pack("<I", 3)),
        ("tokenizer.ggml.add_bos_token", GGUF_BOOL, struct.pack("<?", True)),
        ("tokenizer.ggml.add_space_prefix", GGUF_BOOL, struct.pack("<?", False)),
    ]
    blob = struct.pack("<IIQQ", 0x46554747, 3, 0, len(entries))  # "GGUF", v3, 0 tensors
    for key, type_id, payload in entries:
        blob += _string(key) + struct.pack("<I", type_id) + payload
    path.write_bytes(blob + tail)
    return path


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def test_extraction_writes_the_four_pack_files(tmp_path):
    gguf = write_gguf(tmp_path / "g.gguf", tail=b"\x00" * 64)  # tensor-data stand-in: never read, never hashed
    out = tmp_path / "pack"
    cfg = extract_gemma4(gguf, out)
    tok = out / "tokenizer"
    assert sorted(p.name for p in tok.iterdir()) == ["merges.json", "token_type.json", "tokenizer.json", "vocab.json"]
    tokens, types = vocabulary()
    vocab = read_json(tok / "vocab.json")
    assert vocab == {t: i for i, t in enumerate(tokens)}
    assert read_json(tok / "token_type.json") == types
    assert read_json(tok / "merges.json") == [list(m) for m in MERGES]
    shipped = read_json(tok / "tokenizer.json")
    assert shipped == cfg
    assert shipped["model"] == "gemma4" and shipped["profile"] == "gemma4"
    assert shipped["add_bos"] is True and shipped["add_space_prefix"] is False
    assert shipped["special_ids"] == {"bos": 2, "eos": 1, "pad": 0, "unk": 3, "turn_open": 15, "turn_close": 16,
                                      "think": 12, "channel_open": 13, "channel_close": 14}
    assert shipped["vocab_size"] == len(tokens) and shipped["n_merges"] == len(MERGES)
    header = gguf.read_bytes()[:-64]
    assert shipped["source_header_bytes"] == len(header)
    assert shipped["source_header_sha256"] == hashlib.sha256(header).hexdigest()


def test_merges_json_keeps_newline_and_tab_merges(tmp_path):
    extract_gemma4(write_gguf(tmp_path / "g.gguf"), tmp_path / "pack")
    merges = read_json(tmp_path / "pack" / "tokenizer" / "merges.json")
    assert ["\n", "\n"] in merges and ["\t", "\t"] in merges and ["\n\n", "\n"] in merges
    raw = (tmp_path / "pack" / "tokenizer" / "merges.json").read_text(encoding="utf-8")
    assert '"\\n"' in raw and '"\\t"' in raw, "newline and tab are JSON escapes, never raw line breaks"
    assert "\n" not in raw and "\t" not in raw


def test_profile_and_model_must_agree(tmp_path):
    with pytest.raises(SystemExit, match="tokenizer.ggml.model is gpt2, not gemma4"):
        main(["--profile", "gemma4", "--gguf", str(write_gguf(tmp_path / "q.gguf", model="gpt2")), "--out", str(tmp_path / "a")])
    with pytest.raises(SystemExit, match="a gemma4 GGUF needs --profile gemma4"):
        main(["--gguf", str(write_gguf(tmp_path / "g.gguf")), "--out", str(tmp_path / "b")])
    assert not (tmp_path / "a").exists() and not (tmp_path / "b").exists(), "nothing is written on a refusal"
    assert main(["--profile", "gemma4", "--gguf", str(tmp_path / "g.gguf"), "--out", str(tmp_path / "c")]) == 0
    assert (tmp_path / "c" / "tokenizer" / "merges.json").is_file()


def test_malformed_vocabularies_are_refused(tmp_path):
    tokens, types = vocabulary()
    with pytest.raises(SystemExit, match="duplicate token"):
        extract_gemma4(write_gguf(tmp_path / "d.gguf", tokens=tokens + ["a"], types=types + [1]), tmp_path / "d")
    with pytest.raises(SystemExit, match="one token type per token"):
        extract_gemma4(write_gguf(tmp_path / "t.gguf", types=types[:-1]), tmp_path / "t")
    with pytest.raises(SystemExit, match="merge"):
        extract_gemma4(write_gguf(tmp_path / "m.gguf", merges=[("a", "zz")]), tmp_path / "m")
    with pytest.raises(SystemExit, match="not a GGUF"):
        bad = tmp_path / "x.gguf"
        bad.write_bytes(b"NOPE" + b"\x00" * 32)
        extract_gemma4(bad, tmp_path / "x")


def encoder(tmp_path) -> Encoder:
    extract_gemma4(write_gguf(tmp_path / "g.gguf"), tmp_path / "pack")
    return Encoder.from_pack(tmp_path / "pack" / "tokenizer")


def test_reference_encoder_matches_hand_computed_ids(tmp_path):
    enc = encoder(tmp_path)
    tokens, _ = vocabulary()
    ids = {t: i for i, t in enumerate(tokens)}
    assert enc.encode("Hi world") == [ids["Hi"], ids[SPACE + "world"]]
    assert enc.encode("  x") == [ids[SPACE + SPACE], ids["x"]]
    assert enc.encode("x   ") == [ids["x"], ids[SPACE + SPACE], ids[SPACE]]
    assert enc.encode("abc") == [ids["a"], ids["bc"]]
    assert enc.encode("aaa") == [ids["aa"], ids["a"]]
    assert enc.encode("a\n\nb") == [ids["a"], ids["\n\n"], ids["b"]]
    assert enc.encode("\n\n\n") == [ids["\n\n\n"]]          # a known newline run is that token
    # an unknown run: \n+\n (rank 10) merges at 0 and at 2 before \n\n+\n (rank 12) can apply
    assert enc.encode("\n\n\n\n") == [ids["\n\n"], ids["\n\n"]]
    assert enc.encode("\r\n") == [ids["<0x0D>"], ids["\n"]]
    assert enc.encode("é") == [ids["<0xC3>"], ids["<0xA9>"]]
    assert enc.encode("") == []
    with pytest.raises(ValueError, match="special token literal"):
        enc.encode("a <|turn> b")


def test_reference_encoder_round_trips_the_upstream_corpus(tmp_path):
    enc = encoder(tmp_path)
    corpus = ["", "Hello, world!", "  leading and trailing  ", "a\n\n\nb", "def f(x):\n\treturn x  # comment\n",
              "你好，世界", "مرحبا", "\U0001f600\U0001f680\U0001f1fa\U0001f1f8",
              "é́ combining", "\x00\x01\x7f control", " non-breaking space", "1234567890",
              "MixedCASE_and-dashes", "\r\n\r\n", "  ", "x" * 5000]
    for text in corpus:
        assert enc.decode(enc.encode(text)) == text, text[:20]
    tokens, _ = vocabulary()
    with pytest.raises(ValueError, match="not ordinary output"):
        enc.decode([tokens.index("<|channel>")])


def test_the_upstream_path_is_unchanged_for_other_models(tmp_path):
    """No --profile on a gpt2 GGUF still runs upstream's extract() (the GLM/Qwen packs)."""
    called = {}

    def fake_extract(gguf_path, out_dir):
        called["args"] = (gguf_path, out_dir)
        return {"pre": "qwen35", "vocab_size": 1, "n_merges": 0}

    original = secure_tokenizer.extract
    secure_tokenizer.extract = fake_extract
    try:
        assert main(["--gguf", str(write_gguf(tmp_path / "q.gguf", model="gpt2")), "--out", str(tmp_path / "q")]) == 0
    finally:
        secure_tokenizer.extract = original
    assert called["args"][1] == str(tmp_path / "q")
