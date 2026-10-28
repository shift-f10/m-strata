#pragma once
// strata-prepare tokenizer: Project Maya's tools/strata_tokenizer.py (v1.1.30) - extract() and its main()'s report, the
// pack's tokenizer/ directory from the GGUF's metadata (vocab.json, merges.txt, token_type.json, tokenizer.json,
// chat_template.jinja), written byte for byte as the Python module writes them, after the same checks its Tokenizer
// makes (a byte-level BPE "gpt2" model, a transcribed pre-tokenizer, no duplicate tokens, merges that are pairs of
// vocabulary tokens). The server's tokenizer pack reader reads it back. Spec docs/superpowers/specs/2026-10-27-strata-prepare-design.md.

#include <filesystem>
#include <ostream>

namespace strata_prepare {

// `python tools/strata_tokenizer.py --gguf <gguf> --out <out>`: writes <out>/tokenizer/; its report (or the reason it
// refused) on `log`; 0 on success, else 1.
[[nodiscard]] int export_tokenizer(const std::filesystem::path& gguf, const std::filesystem::path& out, std::ostream& log);

}  // namespace strata_prepare
