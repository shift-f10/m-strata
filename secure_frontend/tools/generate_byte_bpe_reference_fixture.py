"""Generate tests/fixtures/byte_bpe_reference_ids.json - the ids the reference tokenizers give real Qwen and GLM text.

qwen36: upstream v0.2.6's tools/strata_tokenizer.py, Tokenizer.from_gguf on the owner's Qwen3.6-35B-A3B GGUF (pre
qwen35), `_encode_plain(text)`; glm53: Maya v1.1.30's tools/strata_tokenizer.py (`git -C <Project-maya> show
v1.1.30:tools/strata_tokenizer.py` into a folder of its own), Tokenizer.from_dir on the GLM-5.3-Flash pack's tokenizer
folder (pre glm4: GLM_PATTERN and ignore_merges), `_encode_plain(text)`. Both modules are loaded under their own names
and no bytecode is written beside them. The texts: tests/fixtures/flint_tokenizer_texts.json's named texts, then the
search-round texts written here (a model's reasoning, a search result's lines).

Usage: python -I -B tools/generate_byte_bpe_reference_fixture.py <upstream tools dir> <Qwen3.6 GGUF> <Maya tools dir>
           <GLM-5.3 pack tokenizer dir> > tests/fixtures/byte_bpe_reference_ids.json
"""
import importlib.util
import json
import os
import sys

sys.dont_write_bytecode = True  # never a __pycache__ in upstream/

HERE = os.path.dirname(os.path.abspath(__file__))

SEARCH_ROUND = [
    ("reasoning", "Thinking Process:\n\n1.  **Identify the core entity and question:** The user is asking for the most "
                  "recent Formula 1 World Drivers' Championship winner.\n2.  **Search:** I will look it up."),
    ("result", "Web search results for \"2025 Formula 1 World Drivers' Championship winner\" (SearXNG):\n\n"
               "1. 2025 Formula One World Championship - Wikipedia\n   https://en.wikipedia.org/wiki/2025_Formula_One_World_Championship\n"
               "   Published: 2026-10-06T08:10:02\n   The 2025 FIA Formula One World Championship was a motor racing "
               "championship for Formula One cars.\n"),
    ("arguments", "{\"query\": \"Rust latest stable version release date site:blog.rust-lang.org\", \"max_results\": 5}"),
    ("digits", "Rust 1.99.1 was released on October 15, 2026; 12345678 is 12,345,678."),
]


def load(name, folder):
    spec = importlib.util.spec_from_file_location(name, os.path.join(folder, "strata_tokenizer.py"))
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, folder)  # from_gguf imports its gguf_reader from here
    spec.loader.exec_module(module)
    return module


def main():
    upstream = load("upstream_strata_tokenizer", sys.argv[1])
    qwen = upstream.Tokenizer.from_gguf(sys.argv[2])
    maya = load("maya_strata_tokenizer", sys.argv[3])
    glm = maya.Tokenizer.from_dir(sys.argv[4])
    with open(os.path.join(HERE, "..", "tests", "fixtures", "flint_tokenizer_texts.json"), encoding="utf-8") as f:
        named = [(t["name"], t["text"]) for t in json.load(f)["texts"]]
    rows = []
    for name, text in named + SEARCH_ROUND:
        rows.append({"name": name, "text": text, "qwen36": qwen._encode_plain(text), "glm53": glm._encode_plain(text)})
    doc = {
        "source": "upstream v0.2.6 tools/strata_tokenizer.py Tokenizer.from_gguf(Qwen3.6-35B-A3B-Q8_0.gguf)._encode_plain "
                  "(pre %s); Maya v1.1.30 tools/strata_tokenizer.py Tokenizer.from_dir(GLM-5.3-Flash pack tokenizer)"
                  "._encode_plain (pre %s); tools/generate_byte_bpe_reference_fixture.py" % (qwen.pre, glm.pre),
        "texts": rows,
    }
    sys.stdout.write(json.dumps(doc, ensure_ascii=True, indent=1) + "\n")


if __name__ == "__main__":
    main()
