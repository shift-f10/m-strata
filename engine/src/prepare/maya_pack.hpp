#pragma once
// strata-prepare pack --maya: Project Maya's fork of tools/iq_pack.py (v1.1.30) in C++ - the pack Maya's setup builds for
// GLM-5.3-Flash (maya.py pack_step: --compat-bf16, the pack in pack/ beside the GGUF files, which the engine finds at
// <pack>/..) and the engine runs with --glm-pack: index.txt, dense.bin, compat-bf16.json (--compat-bf16), tokenizer/
// (Maya's tools/strata_tokenizer.py: tokenizer_export.hpp), native_experts.txt (Maya's v3/v4) and experts.bin
// (--experts-bin), written byte for byte as the Python tool writes them. Its architectures are Maya's: glm5-next and
// glm5next (the trunk without the NextN block, the leading dense blocks, the fail-closed native set) and qwen4exp; and
// raon1 (Raon-1-mini, on our port of Maya's GLM engine), as our additions to that Python tool pack it: its own native
// set, no BF16 upgrade, a quantized tensor outside the set dequantized to F32.
// Not ported: --base and --skip-experts (maya.py never passes them). Spec docs/superpowers/specs/2026-10-27-strata-prepare-design.md.

#include "iq_pack.hpp"

#include <ostream>

namespace strata_prepare {

// Maya's iq_pack.py main(): its messages on `log`, 0 when the pack is complete, else 1 (what was written before the
// step that failed stays, as the Python tool leaves it).
[[nodiscard]] int maya_iq_pack(const PackOptions& options, std::ostream& log);

}  // namespace strata_prepare
