#pragma once
// strata-prepare pack --maya-ds4 / --maya-mm3: Project Maya's tools/ds4/pack_gguf.py and tools/mm3/pack_gguf.py
// (v1.1.30) in C++ - the packs Maya's DeepSeek-V4.1 and MiniMax-M3.1 programs (maya-ds4, maya-mm3: multiengine_maya_ds4
// and _mm3) read beside a GGUF set's files, no weights copied, written byte for byte as the Python tools write them:
//   DeepSeek: ds4.txt (the geometry under config.json's names), tensors.txt (every tensor under the checkpoint's
//             names), engram.bin (the Engram hash tables, from the set itself, a side file or an Engram set beside it)
//             and tokenizer/ (pre "deepseek3", Maya's chat template or the GGUF's)
//   MiniMax:  mm3.txt, tensors.txt (with the dense layers' indexers side file) and tokenizer/ (pre "minimax", Maya's
//             chat template)
// Not ported: what comes from files other than the GGUF set (DeepSeek's DSpark drafter and vision tower from
// DeepSeek's checkpoint, MiniMax's EXL3 companions): a model with them beside it is packed text-only, and the log says
// so. Spec docs/superpowers/specs/2026-10-27-strata-prepare-design.md.

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

namespace strata_prepare {

struct MoePackOptions {
  std::filesystem::path model;            // --model: the folder of the GGUF's shards
  std::filesystem::path out;              // --out (empty: <model>/maya-ds4 or <model>/maya-mm3)
  std::vector<std::string> shards;        // --shards: the set's files in the folder (empty: every .gguf there)
  std::filesystem::path chat_template;    // Maya's chat_template.jinja (tools/ds4 or tools/mm3); DeepSeek: empty = the GGUF's
  std::filesystem::path engram;           // DeepSeek --engram: the Engram tables' GGUF set (empty: ../Engram when whole)
  std::filesystem::path indexers;         // MiniMax --indexers: the indexers side file (empty: the folder's *-maya-indexers.gguf)
};

// tools/ds4/pack_gguf.py main(): its messages on `log`, 0 when the pack is written, else 1.
[[nodiscard]] int maya_ds4_pack(const MoePackOptions& options, std::ostream& log);
// tools/mm3/pack_gguf.py main() with --text-only's result: its messages on `log`, 0 when the pack is written, else 1.
[[nodiscard]] int maya_mm3_pack(const MoePackOptions& options, std::ostream& log);

}  // namespace strata_prepare
