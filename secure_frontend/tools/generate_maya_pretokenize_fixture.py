"""Generate tests/fixtures/maya_pretokenize_texts.json - Maya's own pre-tokenizer splits for a fixed set of texts.

The reference is Maya v1.1.30's tools/strata_tokenizer.py, imported from a byte-exact copy of Maya's tools folder
(`git -C <Project-maya> show v1.1.30:tools/strata_tokenizer.py`): `_IsolatedSplits(DEEPSEEK3_PATTERNS).findall` for
DeepSeek-V4.1's deepseek3, NFC then `regex.findall(MINIMAX_PATTERN)` for MiniMax-M3.1's minimax, and
`regex.findall(GLM_PATTERN)` for GLM-5.3's glm4/glm5 (Tokenizer._encode_plain). qwen35 is upstream's own split
(upstream/tools/strata_tokenizer.py, `regex.findall(QWEN35_PATTERN)`, which every Qwen pack upstream serves takes),
loaded from the upstream tools folder under another name and without writing bytecode there. The texts are written
here: fixed ones that reach each alternative of the patterns, then seeded random strings over code points chosen at
the classes' edges.

Usage: python -I -B tools/generate_maya_pretokenize_fixture.py <Maya tools dir> <upstream tools dir>
           > tests/fixtures/maya_pretokenize_texts.json
"""
import importlib.util
import json
import os
import random
import sys
import unicodedata

sys.dont_write_bytecode = True  # never a __pycache__ in upstream/

FIXED = [
    "The quick brown fox jumps over the lazy dog. It's 9 o'clock, and we're done!",
    "HelloWorld CamelCase XMLHttpRequest iPhone McDonald's",
    "1234567 x123y 12 345 3.14159 1,000,000 -42 +7e10",
    "a  b", "end   ", "  \n  x", "a\r\n\r\nb", "\t\tword", "\n\n\n", " ", "", "x",
    "don't we'Re it'sx I'LL THEY'VE she'D", "'s 's", "''", "it''s",
    " ((a", "(hello)", "a/b //\n", "path/to/file.txt", "a//\n\nb", "x /\r\n",
    "def f(x):\n    return x**2  # square\n", "if (a && b) { c[i] = d->e; }",
    "#include <vector>\nint main() { return 0; }\n",
    "x́y", "́x", "́ ", "é̂", "été", "Straße naïve résumé",
    "a b", "a　b", "a b", "a\u0085b", "a\u001cb", "a\u000bb\u000cc",
    "日本語" "abc", " 日本", "こんにちは世界", "カタカナ・テスト",
    "中文，标点。", "龥龦鿿", "々〆", "぀ゟ゠ヿ",
    "한국어 텍스트", "العربية ١٢٣٤",
    "हिन्दी", "Αθήνα Москва",
    "１２３４ ⅠⅡⅢ ²³ ½",
    "\U0001f600\U0001f44d\U0001f3fb \U0001f468‍\U0001f469‍\U0001f467 ❤️",
    "© 2026 €5 — “quoted” ¿qué? ¡sí!",
    "ǅǆǄ ʰʱ \U0001d400\U0001d41a \U00010400\U00010428",
    "­​⁠﻿", "\x01\x02", "tab\there", "CR\ronly", "mixed  　 spaces end",
    "Mr.Smith!!!wow??", "--flag=value", "@user #tag $5 %d ^_^ ~home `code`",
    "สวัสดี", "აბგ", "각",
    # qwen35 / glm4 edges: a mark before punctuation (upstream's \p{M} note), Maya's x=\u0394y\u00b2, a search
    # result's lines, digit runs, edge whitespace
    "a\u0301_j x\u0301.y", "x=\u0394y\u00b2",
    "The user is asking for the most recent Formula 1 World Drivers' Championship winner.",
    "1. Rust Versions | Rust Changelogs\n   https://releases.rs/\n   Stable: 1.99.1 Beta: 1.100.0 (12 November, 2026)",
    "12345678901 2026-10-27 3.14", "  leading spaces and trailing  ", "line\n\n  indented\n\tTabbed",
]

POOL = (
    [chr(c) for c in range(0x20, 0x7f)] * 3 + list("\t\n\r\v\f") * 4 +
    [chr(c) for c in (0x00a0, 0x0085, 0x1680, 0x2000, 0x2009, 0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000,
                      0x001c, 0x001f, 0x00ad, 0x200b, 0x200d, 0xfe0f, 0x0301, 0x0308, 0x20dd, 0x0903, 0x093f,
                      0x00e9, 0x00df, 0x00f1, 0x00c0, 0x0391, 0x03b8, 0x0416, 0x0436, 0x0627, 0x0644, 0x0660, 0x0669,
                      0x0939, 0xac00, 0xd55c, 0x4e00, 0x4e2d, 0x9fa5, 0x9fa6, 0x3040, 0x3053, 0x309f, 0x30a0, 0x30ab,
                      0x30fb, 0x30ff, 0x3005, 0x3001, 0x3002, 0xff0c, 0xff10, 0xff21, 0x2160, 0x00b2, 0x00bd, 0x1f600,
                      0x1f44d, 0x1f3fb, 0x2764, 0x00a9, 0x20ac, 0x2014, 0x201c, 0x201d, 0x00bf, 0x00a1, 0x02b0, 0x01c5,
                      0x01c4, 0x01c6, 0x1d400, 0x1d41a, 0x10400, 0x10428, 0x0e2a, 0x0e31, 0x10d0, 0x1100, 0x1161,
                      0x2018, 0x2019, 0x00b7, 0x2026, 0x00d7, 0x00f7, 0x2212, 0x221e, 0x0024, 0x00a3)] * 2
)


def main():
    sys.path.insert(0, sys.argv[1])
    import regex
    import strata_tokenizer as st

    spec = importlib.util.spec_from_file_location("upstream_strata_tokenizer",
                                                  os.path.join(sys.argv[2], "strata_tokenizer.py"))
    upstream = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(upstream)

    rng = random.Random(20261027)
    texts = list(FIXED)
    for _ in range(400):
        texts.append("".join(rng.choice(POOL) for _ in range(rng.randint(1, 32))))
    deepseek = st._IsolatedSplits(st.DEEPSEEK3_PATTERNS)
    minimax = regex.compile(st.MINIMAX_PATTERN)
    glm = regex.compile(st.GLM_PATTERN)
    qwen35 = regex.compile(upstream.QWEN35_PATTERN)
    rows = []
    for text in texts:
        rows.append({
            "text": text,
            "deepseek3": deepseek.findall(text),
            "minimax": minimax.findall(unicodedata.normalize("NFC", text)),
            "glm4": glm.findall(text),
            "qwen35": qwen35.findall(text),
        })
    doc = {
        "source": "Maya v1.1.30 tools/strata_tokenizer.py: deepseek3 = _IsolatedSplits(DEEPSEEK3_PATTERNS).findall(text); "
                  "minimax = regex.findall(MINIMAX_PATTERN, NFC(text)); glm4 = regex.findall(GLM_PATTERN, text) "
                  "(Tokenizer._encode_plain); upstream v0.2.6 tools/strata_tokenizer.py: qwen35 = "
                  "regex.findall(QWEN35_PATTERN, text); tools/generate_maya_pretokenize_fixture.py",
        "regex": regex.__version__,
        "unicodedata": unicodedata.unidata_version,
        "texts": rows,
    }
    sys.stdout.write(json.dumps(doc, ensure_ascii=True, indent=1) + "\n")


if __name__ == "__main__":
    main()
