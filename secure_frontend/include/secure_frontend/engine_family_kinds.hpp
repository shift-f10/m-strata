#pragma once

// The engine-family kinds (plugin spec 2026-10-30 section 5): the enums a family's descriptor (family_descriptor.hpp)
// is made of, apart from the code that uses them, so the descriptor sits under the config layer. Each header that
// held one includes this one; they are moved unchanged, with their comments.

#include <array>
#include <string_view>

namespace secure_frontend {

// deepseek and raon1 (unified-engine design §2.5/§3.3) are served by the one merged engine
// binary over the converged `unified` protocol flavor; qwen/glm53/gemma4 keep their own flavors.
// flint (multiengine_flint) speaks Gemma 4's strata_0143 serve loop with its own template and tokenizer.
enum class EngineProfile { qwen, glm53, gemma4, deepseek, raon1, flint, minimax };
// An engine plugin's family (plugin_family.hpp) holds a profile past the built-ins': the first plugin's is this one, the
// next one's the one after it. Only family_named hands one out.
inline constexpr int kFirstPluginProfile = static_cast<int>(EngineProfile::minimax) + 1;

// An explicit startup-selected prompt/tokenizer contract.  This is not
// inferred from a model name or tokenizer asset: that would allow a model
// artifact to select behavior in the HTTP-serving process.
enum class NativeChatProfile {
  qwen,
  glm53,
  gemma4,     // Gemma 4 plan 1 Task 3 (spec §6.1)
  deepseek,   // unified-engine design §3.2/§3.3: merged-engine families, placeholder templates
  raon1,
  flint,      // multiengine_flint: the GGUF's template (render_flint_text_prompt)
  minimax,    // Maya's maya-mm3 (owner 2026-10-28, MiniMax Community License): Maya's template (render_minimax_text_prompt)
};

// The original Strata profile has no prefill progress record. The separately
// built LightTransport GLM-5.3 engine may emit its documented PP progress
// record while it reads a prompt. The Maya GLM engine (v1.1.8 through
// v1.1.21) frames the same conversation additionally with NOTE conversation
// notes and the STAT expert-tier cadence, and prints the five-field PP (D-8).
// These are three explicit engine-wire contracts, one per flavor, selected at
// construction — never auto-detected from untrusted output, and each flavor
// fails closed on the other flavors' records.
enum class EngineProtocolFlavor {
  strata_0143,           // upstream Strata v0.2.2 (RESUME/REUSED, 3-5 field PP)
  lighttransport_glm53,  // the SP6-built LightTransport engine (exact 3- or 5-field PP)
  maya_118,              // Project Maya's engine, v1.1.8 through v1.1.21 (NOTE, STAT cadence,
                         // 3-6 field PP; v1.1.21's pre-READY VRAM/NOTE records). The name
                         // keeps the first pinned version: the v1.1.21 records are additive.
  unified,               // the merged unified engine (design §3.3, decision 3): one converged
                         // superset dialect — READY/GEN/T/DONE plus a strict 4-field PP, an
                         // optional STAT cadence, and ERR. No NOTE, no RESUME/REUSED; every
                         // other record (including the other flavors' private ones) fails closed.
};

// Gemma 4 plan 1 Task 4 (spec §6.2): how a pack's merges are stored and applied. The mode
// is chosen by the engine profile (gemma4 -> spm_bpe, raon1 -> raon_bpe, flint -> flint_bpe, deepseek ->
// deepseek_bpe, glm53 -> glm_bpe, every other -> byte_bpe), never inferred from the pack's files: byte_bpe is GPT-2's
// byte alphabet with merges.txt and upstream's Qwen pre-tokenizer (qwen35_pretokenize); spm_bpe is SentencePiece-style
// BPE over raw UTF-8 characters with merges.json; raon_bpe is byte_bpe's files and alphabet with raon-1-mini's own text
// handling (raon_nfc, raon_pretokenize); flint_bpe is byte_bpe's files and alphabet with flint's pre-tokenizer
// (flint_pretokenize); deepseek_bpe is byte_bpe's files and alphabet with DeepSeek-V4.1's pre-tokenizer as Maya splits
// it (deepseek_pretokenize); glm_bpe is byte_bpe's files and alphabet with GLM-5.3's pre-tokenizer as Maya splits it
// (glm_pretokenize) and its ignore_merges rule; minimax_bpe is byte_bpe's files and alphabet with MiniMax-M3.1's NFC and
// pre-tokenizer as Maya runs them (minimax_pretokenize; minimax -> minimax_bpe).
enum class TokenizerMode { byte_bpe, spm_bpe, raon_bpe, flint_bpe, deepseek_bpe, glm_bpe, minimax_bpe };

// SP6 §4.9 (mechanism, R5): which engine family the environment is rendered
// for. A family's hook may force and suppress rows (family_environment_rows).
// `strata` has no rows, so it renders exactly the §3.9 allowlist. `glm` is
// reserved for SP6: it currently renders strata rows, is not reachable from
// config, and SP6 fills its table.
enum class EngineFamily { strata, glm };

// Which engine CLI the argv tail is built for. `unified` is the merged engine (deepseek, raon1):
// a closed allowlist of the merged engine's own flags, with --native/--gpu frontend-composed.
// strata_flint is multiengine_flint's own serve flags (engine/src/flint/f_cli.cpp), a closed allowlist like Gemma's.
// plugin is an engine plugin's: the flags its manifest declares (its family's descriptor holds them), nothing else.
enum class EngineArgumentsProfile { strata_0143, lighttransport_glm53, strata_gemma4, unified, strata_flint, maya_ds4, maya_mm3, plugin };

// The Monitor's expert-tier whitelist (owner: the VRAM and RAM cards show experts loaded; serve_monitor.cpp): the
// engines' own key names, INFO split for the main engine and STAT for the GLM engine. Owner 2026-10-28 ("the System Ram
// reports MB usage only"): each family's own words for its RAM tier too - the GLM and raon1 INFO's ram_slots/vram_slots,
// DeepSeek's ram_experts, MiniMax's experts_vram, Gemma 4's and flint's pinned_gib, upstream's engine's arena_mib
// (engine_memory_view reads them). An engine plugin's monitor block names its tiers from this list.
inline constexpr std::array<std::string_view, 15> kExpertReadingKeys{
    "expert_slots", "expert_slots_primary", "expert_cache_mib", "expert_cache_primary_mib", "vram_free_mib",
    "vram_used", "ram_used", "vram_gb", "ram_gb", "ram_slots", "vram_slots", "ram_experts", "experts_vram",
    "pinned_gib", "arena_mib"};
}  // namespace secure_frontend
